
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/syscall.h>      
#include <iostream>
#include <string>
#include <ctime>
#include "../shared.h"


static inline pid_t my_tid() { return (pid_t)syscall(SYS_gettid); }

// CLOCK_MONOTONIC — must match Arbiter `now_ns()` / `stun_apply_ns` epoch style.
static long long hip_monotonic_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

// ─────────────────────────────────────────────
//  GLOBALS
// ─────────────────────────────────────────────
static SharedState *g_state = nullptr;
static int g_shm_fd = -1;

struct PlayerThreadArg {
    int player_index;
};
struct EnemyThreadArg {
    int enemy_index;
};

static PlayerThreadArg g_thread_args[MAX_PLAYERS];
static pthread_t g_player_threads[MAX_PLAYERS];
static EnemyThreadArg g_enemy_thread_args[MAX_ENEMIES];
static pthread_t g_enemy_threads[MAX_ENEMIES];
static bool g_enemy_controller_mode = false;

// Quit flag — set by signal handler, read by main loop
static sig_atomic_t g_quit = 0;
// SIGUSR1 stun: handler only sets this; the 3 s pause runs in normal thread
// context (after syscalls return EINTR) so SFML input/rendering stay healthy.
static thread_local sig_atomic_t g_sigusr1_stun_pending = 0;

// ─────────────────────────────────────────────
//  FORWARD DECLARATIONS
// ─────────────────────────────────────────────
void attach_shared_memory();
void *player_thread(void *arg);
void *enemy_thread(void *arg);
void read_player_action(int player_idx);
void read_enemy_action(int enemy_idx);
void handle_weapon_drop(int player_idx);
void handle_deadlock_modal(int player_idx);
void display_state(int player_idx);
void cleanup_and_exit();

// ─────────────────────────────────────────────
//  SIGNAL HANDLERS — async-signal-safe ONLY
//  No pthread_mutex_lock, no shared memory writes
// ─────────────────────────────────────────────


void sig_usr1_handler(int)
{
    g_sigusr1_stun_pending = 1;
}

// Section 5 — state_lock acquired with SIGUSR1 BLOCKED, so the stun handler

static inline void lock_state()
{
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block, nullptr);
    pthread_mutex_lock(&g_state->state_lock);
}
static inline void unlock_state()
{
    pthread_mutex_unlock(&g_state->state_lock);
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
}

// Consume pending stun: sleep until Arbiter-published deadline (Section 5).
static void hip_run_stun_pause_if_pending(int entity_idx, bool is_enemy = false)
{
    while (g_sigusr1_stun_pending)
    {
        g_sigusr1_stun_pending = 0;
        long long deadline;
        lock_state();
        if (!is_enemy && entity_idx >= 0 && entity_idx < g_state->player_count)
            deadline = g_state->players[entity_idx].stun_apply_ns + CHRONO_STUN_DURATION_NS;
        else if (is_enemy && entity_idx >= 0 && entity_idx < g_state->enemy_count)
            deadline = g_state->enemies[entity_idx].stun_apply_ns + CHRONO_STUN_DURATION_NS;
        else
            deadline = hip_monotonic_ns() + CHRONO_STUN_DURATION_NS;
        unlock_state();

        for (;;)
        {
            long long n = hip_monotonic_ns();
            if (n >= deadline)
                break;
            long long rem = deadline - n;
            if (rem > 20000000LL)
                rem = 20000000LL;
            struct timespec ts;
            ts.tv_sec  = (time_t)(rem / 1000000000LL);
            ts.tv_nsec = (long)(rem % 1000000000LL);
            while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
                ;
        }
    }
}

// SIGTERM — Arbiter or quit request
void sig_term_handler(int)
{
    g_quit = 1;
}

// ─────────────────────────────────────────────
//  MAIN
// ─────────────────────────────────────────────
int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--enemy") == 0)
        g_enemy_controller_mode = true;

    struct sigaction sa;

    // Section 5 — install SIGUSR1 handler WITHOUT SA_RESTART.
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_usr1_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    // While the handler runs, also block SIGUSR1 to avoid piling up delivery.
    sigaddset(&sa.sa_mask, SIGUSR1);
    sigaction(SIGUSR1, &sa, nullptr);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_term_handler;
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);

    // Section 5 — block SIGUSR1 in the main thread BEFORE spawning workers.
    // pthread_create inherits the calling thread's signal mask, so each
    // worker starts with SIGUSR1 blocked. The worker then unblocks it

    {
        sigset_t block;
        sigemptyset(&block);
        sigaddset(&block, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &block, nullptr);
    }

    attach_shared_memory();

    pthread_mutex_lock(&g_state->state_lock);
    int player_count = g_state->player_count;
    int enemy_count = g_state->enemy_count;
    pthread_mutex_unlock(&g_state->state_lock);

    if (!g_enemy_controller_mode)
    {
        // Section 2: "A separate thread must be created for each player-controlled character"
        for (int i = 0; i < player_count; i++)
        {
            g_thread_args[i].player_index = i;
            pthread_create(&g_player_threads[i], nullptr,
                           player_thread, &g_thread_args[i]);
        }
    }
    else
    {
        // Bonus PvP: enemy side is also human-controlled, one thread per enemy entity.
        for (int i = 0; i < enemy_count; i++)
        {
            g_enemy_thread_args[i].enemy_index = i;
            pthread_create(&g_enemy_threads[i], nullptr,
                           enemy_thread, &g_enemy_thread_args[i]);
        }
    }

    // Main thread waits for quit or game over.
    // (Main thread keeps SIGUSR1 blocked — it must never run the stun handler.)
    while (true)
    {
        sleep(1);

        if (g_quit)
            break;

        pthread_mutex_lock(&g_state->state_lock);
        bool over = (g_state->phase == GamePhase::GameOver);
        pthread_mutex_unlock(&g_state->state_lock);

        if (over)
            break;
    }

    cleanup_and_exit();
    return 0;
}

// ─────────────────────────────────────────────
//  ATTACH SHARED MEMORY
// ─────────────────────────────────────────────
void attach_shared_memory()
{
    int retries = 10;
    while (retries-- > 0)
    {
        g_shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
        if (g_shm_fd >= 0)
            break;
        sleep(1);
    }
    if (g_shm_fd < 0)
    {
        perror("HIP: shm_open failed");
        exit(1);
    }
    g_state = (SharedState *)mmap(nullptr, SHM_SIZE,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED, g_shm_fd, 0);
    if (g_state == MAP_FAILED)
    {
        perror("HIP: mmap failed");
        exit(1);
    }
}

// ─────────────────────────────────────────────
//  DISPLAY GAME STATE
// ─────────────────────────────────────────────
void display_state(int player_idx)
{
    lock_state();

    int enemy_count = g_state->enemy_count;
    int player_count = g_state->player_count;
    int total_kills = g_state->total_kills;

    EnemyCharacter snap_enemies[MAX_ENEMIES];
    PlayerCharacter snap_players[MAX_PLAYERS];
    memcpy(snap_enemies, g_state->enemies, sizeof(EnemyCharacter) * enemy_count);
    memcpy(snap_players, g_state->players, sizeof(PlayerCharacter) * player_count);

    unlock_state();

    std::cout << "\n";
    std::cout << "============================================\n";
    std::cout << "  CHRONO RIFT — Player " << (player_idx + 1) << "'s Turn\n";
    std::cout << "============================================\n";

    std::cout << "\n  ENEMIES:\n";
    for (int i = 0; i < enemy_count; i++)
    {
        EnemyCharacter &e = snap_enemies[i];
        if (!e.alive)
        {
            std::cout << "    E" << (i + 1) << ": [DEFEATED]\n";
            continue;
        }
        std::cout << "    E" << (i + 1) << ": HP=" << e.hp << "/" << e.max_hp
                  << "  STM=" << (int)e.stamina << "/" << MAX_STAMINA_ENEMY;
        if (e.stunned)
            std::cout << "  [STUNNED]";
        std::cout << "\n";
    }

    std::cout << "\n  PARTY:\n";
    for (int i = 0; i < player_count; i++)
    {
        PlayerCharacter &p = snap_players[i];
        std::cout << "    P" << (i + 1);
        if (i == player_idx)
            std::cout << " [YOU]";
        std::cout << ": HP=" << p.hp << "/" << p.max_hp
                  << "  STM=" << (int)p.stamina << "/" << MAX_STAMINA_PLAYER;
        if (!p.alive)
            std::cout << "  [KO]";
        if (p.stunned)
            std::cout << "  [STUNNED]";
        std::cout << "\n";
    }

    PlayerCharacter &me = snap_players[player_idx];
    std::cout << "\n  YOUR INVENTORY:\n";
    bool has_weapon = false;
    for (int s = 0; s < INVENTORY_SLOTS; s++)
    {
        if (me.inventory[s].is_start)
        {
            std::cout << "    Slot " << s << ": "
                      << WEAPON_NAMES[(int)me.inventory[s].weapon]
                      << " (dmg=" << WEAPON_DAMAGE[(int)me.inventory[s].weapon] << ")\n";
            has_weapon = true;
        }
    }
    if (!has_weapon)
        std::cout << "    (empty)\n";

    if (me.long_term_count > 0)
    {
        std::cout << "\n  LONG-TERM STORAGE:\n";
        for (int i = 0; i < me.long_term_count; i++)
        {
            if (me.long_term[i].occupied)
                std::cout << "    [" << i << "] "
                          << WEAPON_NAMES[(int)me.long_term[i].weapon] << "\n";
        }
    }

    if (player_has_ultimate(me))
        std::cout << "\n  *** ULTIMATE READY (Solar Core + Lunar Blade!) ***\n";

    std::cout << "\n  Kills: " << total_kills << "/" << TOTAL_KILLS_TO_WIN << "\n";
    std::cout << "\n";
}

// ─────────────────────────────────────────────
//  GUI KEYBOARD INPUT HELPER
//  Sets a prompt in shared memory, blocks on gui_input_sem
//  until Arbiter renderer captures a digit and posts.
//  Returns the digit pressed (0..max).
// ─────────────────────────────────────────────
static int read_gui_digit(int player_idx, int max_d, const char *prompt_fmt, ...)
{
    // Stray posts from weapon/deadlock teardown can wake the next prompt early.
    while (sem_trywait(&g_state->gui_input_sem) == 0)
        ;

    char prompt_buf[80];
    va_list args;
    va_start(args, prompt_fmt);
    vsnprintf(prompt_buf, sizeof(prompt_buf), prompt_fmt, args);
    va_end(args);

    lock_state();
    g_state->gui_pressed_key = -1;
    g_state->gui_prompt_max = max_d;
    g_state->gui_prompt_player = player_idx;
    strncpy(g_state->gui_prompt_text, prompt_buf, sizeof(g_state->gui_prompt_text) - 1);
    g_state->gui_prompt_text[sizeof(g_state->gui_prompt_text) - 1] = '\0';
    g_state->gui_prompt_mode = 1;
    unlock_state();

    // Mirror prompt to terminal as informational
    std::cout << "  " << prompt_buf << " ";
    std::cout.flush();

    bool repaid_during_deadlock = false;

    while (!g_quit)
    {
        hip_run_stun_pause_if_pending(player_idx);

        lock_state();
        bool dmodal_now = g_state->deadlock_modal_pending;
        unlock_state();
        if (repaid_during_deadlock && !dmodal_now)
        {
            lock_state();
            g_state->gui_prompt_max = max_d;
            g_state->gui_prompt_player = player_idx;
            strncpy(g_state->gui_prompt_text, prompt_buf,
                    sizeof(g_state->gui_prompt_text) - 1);
            g_state->gui_prompt_text[sizeof(g_state->gui_prompt_text) - 1] = '\0';
            g_state->gui_prompt_mode = 1;
            unlock_state();
            repaid_during_deadlock = false;
        }

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 250000000; // 250 ms slices — avoids indefinite hang if signals/events mis-order
        if (ts.tv_nsec >= 1000000000)
        {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }

        int r = sem_timedwait(&g_state->gui_input_sem, &ts);
        if (r == 0)
        {
            lock_state();
            bool dmodal = g_state->deadlock_modal_pending;
            int dfor = g_state->deadlock_prompt_player;
            bool drop_ours = g_state->weapon_drop_pending &&
                             g_state->drop_for_player == player_idx;
            unlock_state();

            if (dmodal && dfor >= 0 && player_idx != dfor && !drop_ours)
            {
                repaid_during_deadlock = true;
                continue;
            }
            break;
        }
        if (r == -1 && errno == EINTR)
        {
            hip_run_stun_pause_if_pending(player_idx);
            continue;
        }
        if (r == -1 && errno == ETIMEDOUT)
        {
            lock_state();
            bool dmodal = g_state->deadlock_modal_pending;
            unlock_state();
            // Arbiter clears gui_prompt_* when deadlock ends while we are still
            // waiting here — re-publish so SFML accepts digits again (fixes freeze).
            if (!dmodal)
            {
                lock_state();
                g_state->gui_prompt_max = max_d;
                g_state->gui_prompt_player = player_idx;
                strncpy(g_state->gui_prompt_text, prompt_buf,
                        sizeof(g_state->gui_prompt_text) - 1);
                g_state->gui_prompt_text[sizeof(g_state->gui_prompt_text) - 1] = '\0';
                g_state->gui_prompt_mode = 1;
                unlock_state();
            }
            continue;
        }
        continue;
    }

    lock_state();
    int v = g_state->gui_pressed_key;
    g_state->gui_prompt_text[0] = '\0';
    g_state->gui_prompt_mode = 0;
    g_state->gui_prompt_max = 0;
    g_state->gui_prompt_player = -1;
    unlock_state();

    if (g_quit)
    {
        std::cout << "0\n";
        return 0;
    }

    std::cout << v << "\n";
    return v;
}

// Set the chosen-action label so the renderer can highlight it
static void set_action_label(const char *label)
{
    lock_state();
    strncpy(g_state->gui_action_label, label, sizeof(g_state->gui_action_label) - 1);
    g_state->gui_action_label[sizeof(g_state->gui_action_label) - 1] = '\0';
    unlock_state();
}

static void clear_action_label()
{
    lock_state();
    g_state->gui_action_label[0] = '\0';
    unlock_state();
}

// ─────────────────────────────────────────────
//  DISPLAY ACTION MENU + READ CHOICE (via SFML keyboard)
// ─────────────────────────────────────────────
void read_player_action(int player_idx)
{
    lock_state();
    PlayerCharacter me = g_state->players[player_idx];
    int enemy_count = g_state->enemy_count;
    bool pvp_mode = g_state->pvp_mode;
    EnemyCharacter snap_e[MAX_ENEMIES];
    memcpy(snap_e, g_state->enemies, sizeof(EnemyCharacter) * enemy_count);
    unlock_state();

    display_state(player_idx);

    std::cout << "  ACTIONS (press number key in SFML window):\n";
    std::cout << "    1. Strike        (" << (pvp_mode ? "attack opponent HP" : "attack enemy HP") << ")\n";
    std::cout << "    2. Exhaust       (" << (pvp_mode ? "attack opponent Stamina" : "attack enemy Stamina") << ")\n";
    std::cout << "    3. Use Weapon    (use inventory weapon)\n";
    std::cout << "    4. Swap In       (bring weapon from storage)\n";
    std::cout << "    5. Move to Long Term (inventory -> storage, full turn)\n";
    std::cout << "    6. Heal          (restore 10% HP)\n";
    std::cout << "    7. Skip          (stamina -> 0)\n";
    if (!pvp_mode)
        std::cout << "    8. Ultimate      (Solar+Lunar in inventory; freeze enemies 10s)\n";
    std::cout << "    0. Quit game\n\n";

    clear_action_label();

    // Single-player: always expose key 8 (matches on-screen menu); PvP hides Ultimate.
    int max_main = pvp_mode ? 7 : 8;
    char prompt[64];
    snprintf(prompt, sizeof(prompt), "P%d - Action [0-%d]:", player_idx + 1, max_main);
    int choice = read_gui_digit(player_idx, max_main, prompt);

    // Re-sync after input: menu choice can take wall-clock time; keep the
    // snapshot aligned with shared memory (fixes false "empty inventory"
    // vs display_state + avoids stale HUD for weapon checks).
    lock_state();
    me = g_state->players[player_idx];
    unlock_state();

    ActionRequest req;
    memset(&req, 0, sizeof(req));
    req.source_id = player_idx;
    req.source_is_player = true;
    req.swap_weapon = WeaponType::None;
    req.weapon_slot = -1;
    req.target_id = 0;

    switch (choice)
    {
    case 1:
    {
        req.type = ActionType::Strike;
        set_action_label("Strike");
        if (pvp_mode)
        {
            req.target_id = (player_idx == 0) ? 1 : 0;
            break;
        }
        char p[64];
        snprintf(p, sizeof(p), "Target enemy [1-%d]:", enemy_count);
        int t = read_gui_digit(player_idx, enemy_count, p) - 1;
        if (t < 0 || t >= enemy_count || !snap_e[t].alive)
        {
            for (int i = 0; i < enemy_count; i++)
                if (snap_e[i].alive) { t = i; break; }
        }
        req.target_id = t;
        break;
    }

    case 2:
    {
        req.type = ActionType::Exhaust;
        set_action_label("Exhaust");
        if (pvp_mode)
        {
            req.target_id = (player_idx == 0) ? 1 : 0;
            break;
        }
        char p[64];
        snprintf(p, sizeof(p), "Target enemy [1-%d]:", enemy_count);
        int t = read_gui_digit(player_idx, enemy_count, p) - 1;
        if (t < 0 || t >= enemy_count || !snap_e[t].alive)
        {
            for (int i = 0; i < enemy_count; i++)
                if (snap_e[i].alive) { t = i; break; }
        }
        req.target_id = t;
        break;
    }

    case 3:
    {
        req.type = ActionType::UseWeapon;
        set_action_label("Use Weapon");

        if (me.just_swapped_in)
        {
            std::cout << "  Weapon just swapped in — defaulting to Skip.\n";
            req.type = ActionType::Skip;
            clear_action_label();
            break;
        }

        std::cout << "  Inventory:\n";
        int first_slot = -1;
        int slot_count = 0;
        int slot_map[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        for (int s = 0; s < INVENTORY_SLOTS && slot_count < 10; s++)
        {
            if (me.inventory[s].is_start &&
                me.inventory[s].weapon != WeaponType::None)
            {
                if (first_slot < 0) first_slot = s;
                slot_map[slot_count] = s;
                std::cout << "    " << slot_count << ". slot " << s << ": "
                          << WEAPON_NAMES[(int)me.inventory[s].weapon]
                          << " (dmg=" << WEAPON_DAMAGE[(int)me.inventory[s].weapon] << ")\n";
                slot_count++;
            }
        }
        // Fallback: usable weapon bytes but is_start not set (SHM desync / bug).
        if (first_slot < 0)
        {
            for (int s = 0; s < INVENTORY_SLOTS && slot_count < 10; s++)
            {
                WeaponType w = me.inventory[s].weapon;
                if (w == WeaponType::None)
                    continue;
                bool run_start = (s == 0) ||
                    (me.inventory[s - 1].weapon != w);
                if (!run_start)
                    continue;
                if (first_slot < 0) first_slot = s;
                slot_map[slot_count] = s;
                std::cout << "    " << slot_count << ". slot " << s << ": "
                          << WEAPON_NAMES[(int)w]
                          << " (dmg=" << WEAPON_DAMAGE[(int)w] << ")\n";
                slot_count++;
            }
        }
        if (first_slot < 0)
        {
            
            std::cout << "  No weapons in inventory! Falling back to Skip.\n";
            req.type = ActionType::Skip;
            clear_action_label();
            break;
        }

        int slot_choice = 0;
        if (slot_count > 1)
        {
            int max_d = slot_count - 1;
            char p[64];
            snprintf(p, sizeof(p), "Weapon # [0-%d]:", max_d);
            slot_choice = read_gui_digit(player_idx, max_d, p);
        }
        if (slot_choice < 0 || slot_choice >= slot_count)
            slot_choice = 0;
        req.weapon_slot = slot_map[slot_choice];

        if (pvp_mode)
        {
            req.target_id = (player_idx == 0) ? 1 : 0;
            break;
        }
        char p2[64];
        snprintf(p2, sizeof(p2), "Target enemy [1-%d]:", enemy_count);
        int t = read_gui_digit(player_idx, enemy_count, p2) - 1;
        if (t < 0 || t >= enemy_count || !snap_e[t].alive)
        {
            for (int i = 0; i < enemy_count; i++)
                if (snap_e[i].alive) { t = i; break; }
        }
        req.target_id = t;
        break;
    }

    case 4:
    {
        req.type = ActionType::SwapIn;
        set_action_label("Swap In");

        if (me.long_term_count == 0)
        {
            std::cout << "  Long-term storage empty! Falling back to Skip.\n";
            req.type = ActionType::Skip;
            clear_action_label();
            break;
        }

        std::cout << "  Long-term:\n";
        int max_idx = me.long_term_count - 1;
        if (max_idx > 9) max_idx = 9;
        for (int i = 0; i <= max_idx; i++)
        {
            if (me.long_term[i].occupied)
                std::cout << "    " << i << ". "
                          << WEAPON_NAMES[(int)me.long_term[i].weapon] << "\n";
        }

        char p[64];
        snprintf(p, sizeof(p), "Pick weapon [0-%d]:", max_idx);
        int idx = read_gui_digit(player_idx, max_idx, p);
        if (idx < 0 || idx >= me.long_term_count || !me.long_term[idx].occupied)
            idx = 0;
        req.swap_weapon = me.long_term[idx].weapon;
        break;
    }

    case 5:
    {
        req.type = ActionType::MoveToLongTerm;
        set_action_label("Move to Long Term");

        if (me.long_term_count >= MAX_LONG_TERM)
        {
            std::cout << "  Long-term storage full! Falling back to Skip.\n";
            req.type = ActionType::Skip;
            clear_action_label();
            break;
        }

        std::cout << "  Inventory (pick one to move to long-term):\n";
        int first_slot = -1;
        int slot_count = 0;
        int slot_map[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        for (int s = 0; s < INVENTORY_SLOTS && slot_count < 10; s++)
        {
            if (me.inventory[s].is_start &&
                me.inventory[s].weapon != WeaponType::None)
            {
                if (first_slot < 0) first_slot = s;
                slot_map[slot_count] = s;
                std::cout << "    " << slot_count << ". slot " << s << ": "
                          << WEAPON_NAMES[(int)me.inventory[s].weapon]
                          << " (dmg=" << WEAPON_DAMAGE[(int)me.inventory[s].weapon] << ")\n";
                slot_count++;
            }
        }
        if (first_slot < 0)
        {
            for (int s = 0; s < INVENTORY_SLOTS && slot_count < 10; s++)
            {
                WeaponType w = me.inventory[s].weapon;
                if (w == WeaponType::None)
                    continue;
                bool run_start = (s == 0) ||
                    (me.inventory[s - 1].weapon != w);
                if (!run_start)
                    continue;
                if (first_slot < 0) first_slot = s;
                slot_map[slot_count] = s;
                std::cout << "    " << slot_count << ". slot " << s << ": "
                          << WEAPON_NAMES[(int)w]
                          << " (dmg=" << WEAPON_DAMAGE[(int)w] << ")\n";
                slot_count++;
            }
        }
        if (first_slot < 0)
        {
            std::cout << "  No weapons in inventory! Falling back to Skip.\n";
            req.type = ActionType::Skip;
            clear_action_label();
            break;
        }

        int slot_choice = 0;
        if (slot_count > 1)
        {
            int max_d = slot_count - 1;
            char p[64];
            snprintf(p, sizeof(p), "Weapon # [0-%d]:", max_d);
            slot_choice = read_gui_digit(player_idx, max_d, p);
        }
        if (slot_choice < 0 || slot_choice >= slot_count)
            slot_choice = 0;
        req.swap_weapon = me.inventory[slot_map[slot_choice]].weapon;
        break;
    }

    case 6:
        req.type = ActionType::Heal;
        set_action_label("Heal");
        break;

    case 7:
        req.type = ActionType::Skip;
        set_action_label("Skip");
        break;

    case 8:
        if (!pvp_mode && player_has_ultimate(me))
        {
            req.type = ActionType::Ultimate;
            set_action_label("Ultimate");
        }
        else
        {
            std::cout << "  Need Solar+Lunar! Defaulting to Skip.\n";
            req.type = ActionType::Skip;
            clear_action_label();
        }
        break;

    case 0:
        std::cout << "  Quitting game...\n";
        kill(g_state->arbiter_pid, SIGTERM);
        sleep(2);
        cleanup_and_exit();
        break;

    default:
        std::cout << "  Invalid - default to Skip.\n";
        req.type = ActionType::Skip;
        clear_action_label();
        break;
    }

    lock_state();
    g_state->pending_action = req;
    g_state->turn.action_submitted = true;
    unlock_state();

    sem_post(&g_state->action_ready_sem);
}

// ─────────────────────────────────────────────
//  HANDLE WEAPON DROP PROMPT
// ─────────────────────────────────────────────
void handle_weapon_drop(int player_idx)
{
    while (sem_trywait(&g_state->gui_input_sem) == 0)
        ;

    lock_state();
    WeaponType w  = g_state->dropped_weapon;
    long long dl  = g_state->weapon_drop_deadline_ns;
    unlock_state();

    std::cout << "\n  *** WEAPON DROP ***\n";
    std::cout << "  " << WEAPON_NAMES[(int)w]
              << " dropped! (dmg=" << WEAPON_DAMAGE[(int)w]
              << ", slots=" << WEAPON_SLOTS[(int)w] << ")\n";

    char p[64];
    // Section 6 — controls: 0 picks up, 1 leaves for enemy.
    snprintf(p, sizeof(p), "Pick up %s? [0=Yes / 1=No]:", WEAPON_NAMES[(int)w]);

    // Set up the prompt so the renderer paints the modal + countdown.
    lock_state();
    g_state->gui_pressed_key   = -1;
    g_state->gui_prompt_max    = 1;
    g_state->gui_prompt_player = -1;
    strncpy(g_state->gui_prompt_text, p, sizeof(g_state->gui_prompt_text) - 1);
    g_state->gui_prompt_text[sizeof(g_state->gui_prompt_text) - 1] = '\0';
    g_state->gui_prompt_mode = 1;
    unlock_state();

    std::cout << "  " << p << " ";
    std::cout.flush();

    // Wait for the player's key press OR for the deadline to expire.

    int choice = -1;
    while (true)
    {
        struct timespec ts;
        // Convert CLOCK_MONOTONIC deadline to CLOCK_REALTIME (sem_timedwait
        // uses CLOCK_REALTIME). We compute remaining ns and add to "now".
        struct timespec mono;
        clock_gettime(CLOCK_MONOTONIC, &mono);
        long long now = (long long)mono.tv_sec * 1000000000LL +
                        (long long)mono.tv_nsec;
        long long remain = dl - now;
        if (remain <= 0)
            break;  // deadline already passed — timeout decline
        clock_gettime(CLOCK_REALTIME, &ts);
        long long ns = (long long)ts.tv_nsec + (remain % 1000000000LL);
        ts.tv_sec  += remain / 1000000000LL + ns / 1000000000LL;
        ts.tv_nsec  = ns % 1000000000LL;

        int r = sem_timedwait(&g_state->gui_input_sem, &ts);
        if (r == 0) {
            // Got a key press
            lock_state();
            choice = g_state->gui_pressed_key;
            unlock_state();
            break;
        }
        if (errno == EINTR)
        {
            hip_run_stun_pause_if_pending(player_idx);
            continue; // signal — retry
        }
        if (errno == ETIMEDOUT)
            break;           // 5 s up — treat as decline
        break;               // unexpected — treat as decline
    }

    // Clear the prompt so the modal disappears.
    lock_state();
    g_state->gui_prompt_text[0] = '\0';
    g_state->gui_prompt_mode    = 0;
    g_state->gui_prompt_max     = 0;
    g_state->gui_prompt_player  = -1;
    g_state->player_accepted_drop = (choice == 0);
    g_state->drop_decision_ready  = true;
    unlock_state();

    if (choice == 0)
        std::cout << "0  -> PICK UP\n";
    else if (choice == 1)
        std::cout << "1  -> LEAVE FOR ENEMY\n";
    else
        std::cout << "(timeout) -> LEAVE FOR ENEMY\n";

    // Wake the Arbiter regardless of whether we got a key or timed out.
    sem_post(&g_state->weapon_drop_sem);
}

// ─────────────────────────────────────────────
//  PvP Solar/Lunar deadlock — GUI picks who releases (Arbiter waits on sem)
// ─────────────────────────────────────────────
void handle_deadlock_modal(int player_idx)
{
    while (sem_trywait(&g_state->gui_input_sem) == 0)
        ;

    lock_state();
    long long dl = g_state->deadlock_modal_deadline_ns;
    int preset = g_state->gui_pressed_key;
    unlock_state();

    lock_state();
    // Renderer may have accepted 1/2 before we set prompt_mode; keep it.
    if (preset != 1 && preset != 2)
        g_state->gui_pressed_key = -1;
    g_state->gui_prompt_max  = 2;
    g_state->gui_prompt_player = g_state->deadlock_prompt_player;
    g_state->gui_prompt_text[0] = '\0';
    g_state->gui_prompt_mode = 1;
    unlock_state();

    int choice = -1;
    if (preset == 1 || preset == 2)
        choice = preset;

    while (choice < 1 || choice > 2)
    {
        struct timespec mono;
        clock_gettime(CLOCK_MONOTONIC, &mono);
        long long now = (long long)mono.tv_sec * 1000000000LL +
                        (long long)mono.tv_nsec;
        long long remain = dl - now;
        if (remain <= 0)
            break;

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        long long ns = (long long)ts.tv_nsec + (remain % 1000000000LL);
        ts.tv_sec  += remain / 1000000000LL + ns / 1000000000LL;
        ts.tv_nsec  = ns % 1000000000LL;

        int r = sem_timedwait(&g_state->gui_input_sem, &ts);
        if (r == 0)
        {
            lock_state();
            int k = g_state->gui_pressed_key;
            unlock_state();
            if (k == 1 || k == 2)
            {
                choice = k;
                break;
            }
            continue;
        }
        if (r == -1 && errno == EINTR)
        {
            hip_run_stun_pause_if_pending(player_idx);
            continue;
        }
        if (r == -1 && errno == ETIMEDOUT)
            break;
        break;
    }

    if (choice < 1 || choice > 2)
        choice = 1;

    lock_state();
    g_state->deadlock_user_choice = choice;
    g_state->gui_prompt_mode      = 0;
    g_state->gui_prompt_max       = 0;
    g_state->gui_prompt_player    = -1;
    g_state->gui_prompt_text[0]   = '\0';
    g_state->gui_pressed_key      = -1;
    unlock_state();

    while (sem_trywait(&g_state->gui_input_sem) == 0)
        ;

    sem_post(&g_state->deadlock_choice_sem);
}

// ─────────────────────────────────────────────
//  PLAYER THREAD
//  One per player character 
// ─────────────────────────────────────────────
void *player_thread(void *arg)
{
    PlayerThreadArg *parg = (PlayerThreadArg *)arg;
    int idx = parg->player_index;

    // Section 5 — publish our kernel TID so the Arbiter can tgkill us

    pthread_mutex_lock(&g_state->state_lock);
    g_state->player_tid[idx] = my_tid();
    pthread_mutex_unlock(&g_state->state_lock);

    {
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, SIGUSR1);
        pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
    }

    while (true)
    {
        // Block until Arbiter signals this player's turn.
        // Retry on EINTR: sem_wait is never auto-restarted after signal
        // delivery. Stun pause runs in hip_run_stun_pause_if_pending after EINTR.
        hip_run_stun_pause_if_pending(idx);
        while (sem_wait(&g_state->player_turn_sem[idx]) == -1 && errno == EINTR)
            hip_run_stun_pause_if_pending(idx);

        if (g_quit)
            break;

        lock_state();
        bool over            = (g_state->phase == GamePhase::GameOver);
        bool is_alive        = g_state->players[idx].alive;
        bool drop_pending    = g_state->weapon_drop_pending;
        int  drop_for        = g_state->drop_for_player;
        bool decision_ready  = g_state->drop_decision_ready;
        bool dlock_modal     = g_state->deadlock_modal_pending;
        int  dlock_for       = g_state->deadlock_prompt_player;
        bool is_my_turn      = (g_state->turn.owner == TurnOwner::Player &&
                                g_state->turn.entity_index == idx &&
                                !g_state->turn.action_submitted);
        unlock_state();

        if (over)
            break;
        if (!is_alive)
            continue;

        // PvP Solar/Lunar deadlock prompt (Arbiter woke this thread specifically).
        if (dlock_modal && dlock_for == idx)
        {
            handle_deadlock_modal(idx);
            continue;
        }

        // Section 6 — drop handling:

        if (drop_pending && drop_for == idx && !decision_ready)
        {
            handle_weapon_drop(idx);
            continue;
        }

        if (!is_my_turn)
            continue;

        // Only THIS thread processes input — others blocked on sem_wait
        read_player_action(idx);
    }

    return nullptr;
}

void read_enemy_action(int enemy_idx)
{
    lock_state();
    int player_count = g_state->player_count;
    unlock_state();

    int action = read_gui_digit(-1, 1,
                                "E%d action: 0=Strike 1=Skip",
                                enemy_idx + 1);
    if (g_quit)
        return;

    ActionRequest req;
    memset(&req, 0, sizeof(req));
    req.source_is_player = false;
    req.source_id = enemy_idx;

    if (action == 1)
    {
        req.type = ActionType::Skip;
        req.target_id = 0;
    }
    else
    {
        req.type = ActionType::Strike;
        int t = read_gui_digit(-1, player_count,
                               "E%d target P(1-%d):",
                               enemy_idx + 1, player_count) - 1;
        if (g_quit)
            return;

        lock_state();
        if (t < 0 || t >= g_state->player_count || !g_state->players[t].alive)
        {
            t = -1;
            for (int i = 0; i < g_state->player_count; i++)
            {
                if (g_state->players[i].alive)
                {
                    t = i;
                    break;
                }
            }
            if (t < 0)
                t = 0;
        }
        unlock_state();
        req.target_id = t;
    }

    lock_state();
    bool is_my_turn = (g_state->turn.owner == TurnOwner::Enemy &&
                       g_state->turn.entity_index == enemy_idx &&
                       !g_state->turn.action_submitted);
    if (is_my_turn)
    {
        g_state->pending_action = req;
        g_state->turn.action_submitted = true;
        sem_post(&g_state->action_ready_sem);
    }
    unlock_state();
}

void *enemy_thread(void *arg)
{
    EnemyThreadArg *earg = (EnemyThreadArg *)arg;
    int idx = earg->enemy_index;

    pthread_mutex_lock(&g_state->state_lock);
    g_state->enemy_tid[idx] = my_tid();
    pthread_mutex_unlock(&g_state->state_lock);

    {
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, SIGUSR1);
        pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
    }

    while (true)
    {
        hip_run_stun_pause_if_pending(idx, true);
        while (sem_wait(&g_state->enemy_turn_sem[idx]) == -1 && errno == EINTR)
            hip_run_stun_pause_if_pending(idx, true);

        if (g_quit)
            break;

        lock_state();
        bool over = (g_state->phase == GamePhase::GameOver);
        bool alive = g_state->enemies[idx].alive;
        bool is_my_turn = (g_state->turn.owner == TurnOwner::Enemy &&
                           g_state->turn.entity_index == idx &&
                           !g_state->turn.action_submitted);
        unlock_state();

        if (over)
            break;
        if (!alive || !is_my_turn)
            continue;

        read_enemy_action(idx);
    }

    return nullptr;
}

// ─────────────────────────────────────────────
//  CLEANUP AND EXIT
// ─────────────────────────────────────────────
void cleanup_and_exit()
{
    if (g_state)
    {
        munmap(g_state, SHM_SIZE);
        g_state = nullptr;
    }
    if (g_shm_fd >= 0)
    {
        close(g_shm_fd);
        g_shm_fd = -1;
    }
    exit(0);
}
