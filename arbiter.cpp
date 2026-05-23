#include <SFML/Graphics.hpp>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <cerrno>
#include <climits> 
#include <cmath>   
#include <pthread.h>
#include <semaphore.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/syscall.h>   
#include <iostream>
#include <string>
#include <cstdarg>
#include "../shared.h" 


static inline int thread_kill(pid_t tgid, pid_t tid, int sig)
{
    return (int)syscall(SYS_tgkill, tgid, tid, sig);
}

// ─────────────────────────────────────────────
//  ROLL NUMBER 
// ─────────────────────────────────────────────
#define ROLL_NUMBER 727   
#define ROLL_LAST_DIGIT 7 
#define ROLL_LAST_2 27    
#define ROLL_SECOND_LAST 2 

// Section 10 — enemy slot stats (HP / damage / speed / stamina max via constants)
static void init_enemy_slot_from_spec(EnemyCharacter &e)
{
    e.alive = true;
    e.max_hp = ROLL_LAST_2 + 50 + (rand() % 151); // last 2 digits + rand in [50,200]
    e.hp = e.max_hp;
    e.damage = ROLL_SECOND_LAST + 10;
    e.speed = (float)(10 + (rand() % 21)); // rand in [10,30] inclusive
    e.stamina = 0.0f;
    e.stunned = false;
    e.ready_to_act = false;
    e.held_weapon = WeaponType::None;
    e.fill_tick = INT_MAX;
    e.stun_apply_ns = 0;
}

// ─────────────────────────────────────────────
//  WINDOW DIMENSIONS  (FF6-style 4:3)
// ─────────────────────────────────────────────
#define WIN_W 960
#define WIN_H 640

// ─────────────────────────────────────────────
//  GLOBALS
// ─────────────────────────────────────────────
static SharedState *g_state = nullptr; // pointer into shared memory
static int g_shm_fd = -1;
static pid_t g_hip_pid = 0;
static pid_t g_asp_pid = 0;
static bool g_pvp_mode = false;

// Signal flags — set by handlers, read by scheduler
static sig_atomic_t g_ultimate_fired = 0;
static sig_atomic_t g_quit_requested = 0;
// Section 2 — Lifecycle Management: detect premature HIP/ASP termination
static sig_atomic_t g_hip_died = 0;
static sig_atomic_t g_asp_died = 0;
static int g_scheduler_tick = 0;
static int g_eclipse_spawn_kill = 3;

// ─────────────────────────────────────────────
//  FORWARD DECLARATIONS
// ─────────────────────────────────────────────
void init_shared_memory();
void init_game_state(int player_count);
void run_bonus_pvp_draft_phase();
void fork_hip(bool enemy_controller);
void fork_asp();
void *scheduler_thread(void *);
void *deadlock_monitor_thread(void *);
static void strip_weapon_slots(PlayerCharacter &p, WeaponType w);
static void apply_interactive_solar_lunar_resolution(int choice, int idx_solar, int idx_lunar);
void *rendering_thread(void *);
void apply_action(const ActionRequest &req);
void handle_enemy_death(int enemy_idx);
void handle_player_death(int player_idx);          // Section 2 lifecycle mgmt
void release_entity_artifacts(int entity_idx,
                              bool is_player);     // Section 2 lifecycle mgmt
bool check_win_lose();
void log_action(const char *fmt, ...);
void cleanup_and_exit(GameResult result);
int inventory_place_weapon(PlayerCharacter &p, WeaponType w);
void inventory_swap_to_long_term(PlayerCharacter &p, WeaponType w);
// Section 6 viz — set inventory animation hints under state_lock
void set_inv_anim_place(int player_idx, int slot, int size);
void set_inv_anim_evict(int player_idx, int slot, int size);
void set_inv_anim_swapin(int player_idx, int slot, int size);
// Find which player a PlayerCharacter& belongs to (small array, O(P)).
int  player_index_of(const PlayerCharacter &p);

// Signal handlers
void sig_alrm_handler(int);
void sig_term_handler(int);
void sig_usr1_handler(int);
void sig_chld_handler(int);                        // Section 2 lifecycle mgmt

// ─────────────────────────────────────────────
//  Section 5 — wall-clock helper for exact 3-second stun timing
// ─────────────────────────────────────────────
static inline long long now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}
// ─────────────────────────────────────────────
//  SIGNAL HANDLERS
// ─────────────────────────────────────────────

// SIGALRM — fired 10 seconds after Ultimate Ability
// Resumes ASP (sends SIGCONT) — async-signal-safe only
void sig_alrm_handler(int)
{
    g_ultimate_fired = 1;
    if (g_asp_pid > 0)
        kill(g_asp_pid, SIGCONT);
}

// SIGTERM — player chose to quit (sent by HIP)
void sig_term_handler(int)
{
    g_quit_requested = 1;
}

// SIGUSR1 — stun signal (sent by Arbiter to HIP or ASP)
void sig_usr1_handler(int) {}

// SIGCHLD — Section 2 Lifecycle Management

void sig_chld_handler(int)
{
    int saved_errno = errno;
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        if (pid == g_hip_pid)
        {
            g_hip_died = 1;
            g_hip_pid = 0; // prevent later kill() to a dead pid
        }
        else if (pid == g_asp_pid)
        {
            g_asp_died = 1;
            g_asp_pid = 0;
        }
    }
    errno = saved_errno;
}

// ─────────────────────────────────────────────
//  MAIN
// ─────────────────────────────────────────────
int main()
{
    srand((unsigned)ROLL_NUMBER ^ (unsigned)time(nullptr));

    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_alrm_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, nullptr);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_term_handler;
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_usr1_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, nullptr);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_chld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, nullptr);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigaction(SIGTSTP, &sa, nullptr);

    // ── Step 1: Shared memory ──────────────────
    init_shared_memory();

    // ── Step 2: Prompt player count ───────────
    int player_count = 0;
    while (player_count < 1 || player_count > 4)
    {
        std::cout << "=== CHRONO RIFT ===\n";
        std::cout << "Select party size (1-4): ";
        std::cin >> player_count;
        if (player_count < 1 || player_count > 4)
            std::cout << "Invalid. Enter 1 to 4.\n";
    }

    int pvp_choice = -1;
    while (pvp_choice != 1 && pvp_choice != 2)
    {
        std::cout << "\nSelect game mode:\n";
        std::cout << "  1 - Single Mode\n";
        std::cout << "  2 - Bonus PvP Mode\n";
        std::cout << "Enter choice (1 or 2): ";
        std::cin >> pvp_choice;
        if (pvp_choice != 1 && pvp_choice != 2)
            std::cout << "Invalid. Enter 1 or 2.\n";
    }
    g_pvp_mode = (pvp_choice == 2);
    if (g_pvp_mode)
    {
        player_count = 2;
        std::cout << "Bonus PvP mode enabled: fixed duel P1 vs P2.\n";
    }

    // ── Step 3: Initialize game state ─────────
    init_game_state(player_count);
    if (g_pvp_mode)
        run_bonus_pvp_draft_phase();

    // ── Step 4: Store our PID ─────────────────
    g_state->arbiter_pid = getpid();

    for (int i = 0; i < MAX_PLAYERS; i++) g_state->player_tid[i] = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) g_state->enemy_tid[i] = 0;

    // ── Step 5: Fork HIP and ASP (PvP duel has no ASP / no NPC enemies) ─────────
    fork_hip(false);
    if (!g_pvp_mode)
        fork_asp();

    // Give children time to attach to shared memory
    sleep(1);

    g_state->phase = GamePhase::Running;

    // ── Step 6: Launch threads ─────────────────
    pthread_t sched_tid, deadlock_tid, render_tid;
    pthread_create(&render_tid, nullptr, rendering_thread, nullptr);
    pthread_create(&sched_tid, nullptr, scheduler_thread, nullptr);
    if (!g_pvp_mode)
        pthread_create(&deadlock_tid, nullptr, deadlock_monitor_thread, nullptr);

    // Wait for scheduler to finish (game over)
    pthread_join(sched_tid, nullptr);

    sleep(2);

    // ── Step 7: Cleanup ───────────────────────
    if (g_quit_requested)
        cleanup_and_exit(GameResult::Quit);
    else if (g_state->result == GameResult::Win)
        cleanup_and_exit(GameResult::Win);
    else
        cleanup_and_exit(GameResult::Lose);

    return 0;
}

// ─────────────────────────────────────────────
//  SHARED MEMORY INIT
// ─────────────────────────────────────────────
void init_shared_memory()
{
    // Unlink any stale segment from previous run
    shm_unlink(SHM_NAME);

    g_shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (g_shm_fd < 0)
    {
        perror("shm_open");
        exit(1);
    }
    if (ftruncate(g_shm_fd, SHM_SIZE) < 0)
    {
        perror("ftruncate");
        exit(1);
    }
    g_state = (SharedState *)mmap(nullptr, SHM_SIZE,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED, g_shm_fd, 0);
    if (g_state == MAP_FAILED)
    {
        perror("mmap");
        exit(1);
    }
    memset(g_state, 0, SHM_SIZE);

    // Initialize shared recursive mutexes.
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_state->state_lock, &mattr);
    pthread_mutex_init(&g_state->resource_table.table_lock, &mattr);
    pthread_mutexattr_destroy(&mattr);

    // ── Initialize semaphores (unnamed, shared) ──
    for (int i = 0; i < MAX_PLAYERS; i++)
        sem_init(&g_state->player_turn_sem[i], 1, 0);
    for (int i = 0; i < MAX_ENEMIES; i++)
        sem_init(&g_state->enemy_turn_sem[i], 1, 0);
    sem_init(&g_state->action_ready_sem, 1, 0);
    sem_init(&g_state->weapon_drop_sem, 1, 0); // HIP posts when drop decision made
    sem_init(&g_state->deadlock_choice_sem, 1, 0);
    sem_init(&g_state->gui_input_sem, 1, 0);   // SFML keyboard input handoff

    // GUI input state
    g_state->gui_pressed_key = -1;
    g_state->gui_prompt_mode = 0;
    g_state->gui_prompt_max = 0;
    g_state->gui_prompt_player = -1;
    g_state->gui_prompt_text[0] = '\0';
    g_state->gui_action_label[0] = '\0';

    // Animation state
    g_state->anim_attacker = -1;
    g_state->anim_target = -1;
    g_state->anim_attacker_is_player = false;
    g_state->anim_target_is_player = false;
    g_state->anim_damage = 0;
    g_state->anim_kind = 0;
    g_state->anim_frames_left = 0;

    // ── Initialize artifact table ──
    for (int i = 0; i < (int)ArtifactID::COUNT; i++)
    {
        g_state->resource_table.entries[i].exists = (i < 2); // Solar+Lunar exist at start
        g_state->resource_table.entries[i].is_free = true;
        g_state->resource_table.entries[i].held_by_player = false;
        g_state->resource_table.entries[i].holder_index = -1;
        g_state->resource_table.entries[i].has_waiter = false;
        g_state->resource_table.entries[i].waiter_is_player = false;
        g_state->resource_table.entries[i].waiter_index = -1;
    }
    // Eclipse Relic does not exist yet
    g_state->resource_table.entries[(int)ArtifactID::EclipseRelic].exists = false;

    g_state->phase = GamePhase::Init;
    g_state->result = GameResult::None;
    g_state->total_kills = 0;

    std::cout << "Shared memory initialized.\n";
}

// ─────────────────────────────────────────────
//  GAME STATE INIT
// ─────────────────────────────────────────────
void init_game_state(int player_count)
{
    g_state->player_count = player_count;
    g_state->pvp_mode = g_pvp_mode;
    g_eclipse_spawn_kill = 1 + (rand() % 10);

    int enemy_count = g_pvp_mode ? 0 : (2 + (rand() % 8));
    g_state->enemy_count = enemy_count;

    // ── Players ──────────────────────────────
    for (int i = 0; i < player_count; i++)
    {
        PlayerCharacter &p = g_state->players[i];
        memset(&p, 0, sizeof(PlayerCharacter));
        p.alive = true;
        p.max_hp = g_pvp_mode ? 1000 : (ROLL_NUMBER + 100 + (rand() % 901));
        p.hp = p.max_hp;
        p.damage = ROLL_LAST_DIGIT + 10;
        p.speed = (float)MAX_STAMINA_PLAYER / (float)player_count; // 100 / party size
        p.stamina = 0.0f;
        p.stunned = false;
        p.ready_to_act = false;
        p.long_term_count = 0;
        p.just_swapped_in = false;
        p.fill_tick = INT_MAX;
        p.stun_apply_ns = 0;

        // Clear inventory
        for (int s = 0; s < INVENTORY_SLOTS; s++)
        {
            p.inventory[s].weapon = WeaponType::None;
            p.inventory[s].is_start = false;
        }
    }

    // ── Enemies ───────────────────────────────
    for (int i = 0; i < enemy_count; i++)
    {
        EnemyCharacter &e = g_state->enemies[i];
        memset(&e, 0, sizeof(EnemyCharacter));
        init_enemy_slot_from_spec(e);
    }

    // Bonus PvP
    if (g_pvp_mode)
    {
        pthread_mutex_lock(&g_state->resource_table.table_lock);
        for (int i = 0; i < (int)ArtifactID::COUNT; i++)
        {
            ArtifactEntry &ae = g_state->resource_table.entries[i];
            ae.exists = false;
            ae.is_free = true;
            ae.held_by_player = false;
            ae.holder_index = -1;
            ae.has_waiter = false;
            ae.waiter_is_player = false;
            ae.waiter_index = -1;
        }
        pthread_mutex_unlock(&g_state->resource_table.table_lock);
    }

    // Solar Core + Lunar Blade: two random enemies carry them so each appears at least once
    // via the same weapon-drop prompt flow when that carrier is defeated.
    if (enemy_count >= 2)
    {
        int idx_solar = rand() % enemy_count;
        int idx_lunar = rand() % enemy_count;
        while (idx_lunar == idx_solar)
            idx_lunar = rand() % enemy_count;
        g_state->enemies[idx_solar].held_weapon = WeaponType::SolarCore;
        g_state->enemies[idx_lunar].held_weapon = WeaponType::LunarBlade;
        pthread_mutex_lock(&g_state->resource_table.table_lock);
        ArtifactEntry &as = g_state->resource_table.entries[(int)ArtifactID::SolarCore];
        as.is_free = false;
        as.held_by_player = false;
        as.holder_index = idx_solar;
        ArtifactEntry &al = g_state->resource_table.entries[(int)ArtifactID::LunarBlade];
        al.is_free = false;
        al.held_by_player = false;
        al.holder_index = idx_lunar;
        pthread_mutex_unlock(&g_state->resource_table.table_lock);
    }

    // ── Turn control ─────────────────────────
    g_state->turn.owner = TurnOwner::Player;
    g_state->turn.entity_index = (g_pvp_mode ? (rand() % 2) : 0);
    g_state->turn.action_submitted = false;
    g_state->turn.action_applied = false;

    // ── Action log ───────────────────────────
    memset(&g_state->log, 0, sizeof(ActionLog));
    g_state->log.head = 0;
    g_state->log.count = 0;

    // ── Weapon drop flags ────────────────────
    g_state->weapon_drop_pending = false;
    g_state->drop_decision_ready = false;
    g_state->weapon_drop_deadline_ns = 0;

    // Section 6 — inventory anim hints
    g_state->inv_anim_player      = -1;
    g_state->inv_anim_place_slot  = -1;
    g_state->inv_anim_place_size  = 0;
    g_state->inv_anim_place_ttl   = 0;
    g_state->inv_anim_evict_slot  = -1;
    g_state->inv_anim_evict_size  = 0;
    g_state->inv_anim_evict_ttl   = 0;
    g_state->inv_anim_swapin_slot = -1;
    g_state->inv_anim_swapin_size = 0;
    g_state->inv_anim_swapin_ttl  = 0;

    std::cout << "Game state initialized. Players: " << player_count
              << "  Concurrent enemies (fixed this run): " << enemy_count << "\n";
}

void run_bonus_pvp_draft_phase()
{
    static const WeaponType pvp_pool[] = {
        WeaponType::IronHalberd,
        WeaponType::VenomDagger,
        WeaponType::Thunderstaff,
        WeaponType::ObsidianAxe,
        WeaponType::Frostbow,
        WeaponType::SplinterStick
    };

    int opt_idx[3];
    for (int i = 0; i < 3; i++)
    {
        int c;
        bool used;
        do {
            c = rand() % (int)(sizeof(pvp_pool) / sizeof(pvp_pool[0]));
            used = false;
            for (int j = 0; j < i; j++)
                if (opt_idx[j] == c) used = true;
        } while (used);
        opt_idx[i] = c;
    }

    int first = rand() % 2; // 0 => P1 first, 1 => P2 first
    int order[4];
    if (first == 0)
    {
        order[0] = 0; order[1] = 1; order[2] = 1; order[3] = 0;
    }
    else
    {
        order[0] = 1; order[1] = 0; order[2] = 0; order[3] = 1;
    }

    std::cout << "\n=== BONUS PVP DRAFT PHASE ===\n";
    std::cout << "Shared options (same for both players):\n";
    for (int i = 0; i < 3; i++)
    {
        WeaponType w = pvp_pool[opt_idx[i]];
        std::cout << "  " << i << " - " << WEAPON_NAMES[(int)w]
                  << "  (dmg=" << WEAPON_DAMAGE[(int)w]
                  << ", slots=" << WEAPON_SLOTS[(int)w] << ")\n";
    }
    std::cout << "Pick order: P" << (order[0] + 1) << ", P" << (order[1] + 1)
              << ", P" << (order[2] + 1) << ", P" << (order[3] + 1) << "\n";

    for (int r = 0; r < 4; r++)
    {
        int p = order[r];
        int choice = -1;
        while (choice < 0 || choice > 2)
        {
            std::cout << "P" << (p + 1) << " pick option [0-2]: ";
            std::cin >> choice;
            if (choice < 0 || choice > 2)
                std::cout << "Invalid. Enter 0, 1, or 2.\n";
        }
        WeaponType picked = pvp_pool[opt_idx[choice]];
        inventory_place_weapon(g_state->players[p], picked);
        log_action("P%d drafted %s.", p + 1, WEAPON_NAMES[(int)picked]);
    }
}

// ─────────────────────────────────────────────
//  FORK HIP
// ─────────────────────────────────────────────
void fork_hip(bool enemy_controller)
{
    (void)enemy_controller;
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork HIP");
        exit(1);
    }
    if (pid == 0)
    {
        // Child — exec the HIP executable
        execl("./hip_bin", "hip_bin", NULL);
        perror("execl hip");
        exit(1);
    }
    g_hip_pid = pid;
    g_state->hip_pid = pid;
    std::cout << "HIP forked. PID=" << pid << "\n";
}

// ─────────────────────────────────────────────
//  FORK ASP

void fork_asp()
{
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork ASP");
        exit(1);
    }
    if (pid == 0)
    {
        execl("./asp_bin", "asp_bin", NULL);
        perror("execl asp");
        exit(1);
    }
    g_asp_pid = pid;
    g_state->asp_pid = pid;
    std::cout << "ASP forked. PID=" << pid << "\n";
}

// ─────────────────────────────────────────────
//  LOG ACTION  (circular buffer in shared memory)
// ─────────────────────────────────────────────
void log_action(const char *fmt, ...)
{
    if (!g_state)
        return;
    pthread_mutex_lock(&g_state->state_lock);

    ActionLog &log = g_state->log;
    va_list args;
    va_start(args, fmt);
    vsnprintf(log.lines[log.head], ACTION_LOG_LEN, fmt, args);
    va_end(args);

    log.head = (log.head + 1) % ACTION_LOG_LINES;
    if (log.count < ACTION_LOG_LINES)
        log.count++;

    pthread_mutex_unlock(&g_state->state_lock);
}

// ─────────────────────────────────────────────
//  Section 6 visualisation helpers — caller must already hold state_lock.
//  TTL is in renderer frames (~30 fps).
// ─────────────────────────────────────────────
int player_index_of(const PlayerCharacter &p)
{
    for (int i = 0; i < g_state->player_count; i++)
        if (&g_state->players[i] == &p) return i;
    return -1;
}
void set_inv_anim_place(int player_idx, int slot, int size)
{
    g_state->inv_anim_player     = player_idx;
    g_state->inv_anim_place_slot = slot;
    g_state->inv_anim_place_size = size;
    g_state->inv_anim_place_ttl  = 36; // ~1.2 s at 30 fps
}
void set_inv_anim_evict(int player_idx, int slot, int size)
{
    g_state->inv_anim_player     = player_idx;
    g_state->inv_anim_evict_slot = slot;
    g_state->inv_anim_evict_size = size;
    g_state->inv_anim_evict_ttl  = 30;
}
void set_inv_anim_swapin(int player_idx, int slot, int size)
{
    g_state->inv_anim_player      = player_idx;
    g_state->inv_anim_swapin_slot = slot;
    g_state->inv_anim_swapin_size = size;
    g_state->inv_anim_swapin_ttl  = 36;
}

// ─────────────────────────────────────────────
//  INVENTORY ALLOCATOR  (Section 6)

int inventory_place_weapon(PlayerCharacter &p, WeaponType w)
{
    int need = WEAPON_SLOTS[(int)w];
    if (need <= 0 || need > INVENTORY_SLOTS) return -1;

    int pidx = player_index_of(p); // for renderer hints

    auto place_at = [&](int start) {
        for (int j = 0; j < need; j++)
        {
            p.inventory[start + j].weapon = w;
            p.inventory[start + j].is_start = (j == 0);
        }
        if (pidx >= 0) set_inv_anim_place(pidx, start, need);
    };

    // ── Phase 1: first-fit free run of size `need` ───────────────────
    for (int i = 0; i <= INVENTORY_SLOTS - need; i++)
    {
        bool ok = true;
        for (int j = 0; j < need; j++)
            if (p.inventory[i + j].weapon != WeaponType::None) { ok = false; break; }
        if (ok) { place_at(i); return i; }
    }

    // ── Phase 2: pick start with minimum distinct-weapon eviction count ─
    int best_start    = -1;
    int best_evict_n  = INT_MAX;
    int best_evict_starts[INVENTORY_SLOTS] = {0};

    for (int i = 0; i <= INVENTORY_SLOTS - need; i++)
    {
        // Distinct weapon-start indices that overlap [i, i+need).
        int evict_starts[INVENTORY_SLOTS];
        int evict_n = 0;
        bool seen[INVENTORY_SLOTS] = {false};

        for (int j = 0; j < need; j++)
        {
            int s = i + j;
            if (p.inventory[s].weapon == WeaponType::None) continue;
            // Walk back to the start slot of this weapon (is_start marker).
            int k = s;
            while (k > 0 && !p.inventory[k].is_start) k--;
            if (!seen[k]) { seen[k] = true; evict_starts[evict_n++] = k; }
        }

        if (evict_n < best_evict_n)
        {
            best_evict_n = evict_n;
            best_start   = i;
            for (int j = 0; j < evict_n; j++) best_evict_starts[j] = evict_starts[j];
            if (evict_n == 0) break; // can't beat zero
        }
    }

    if (best_start < 0) return -1;

    // ── Evict the chosen weapons (in left-to-right order so storage

    for (int a = 0; a < best_evict_n - 1; a++)
        for (int b = a + 1; b < best_evict_n; b++)
            if (best_evict_starts[b] < best_evict_starts[a])
            { int t = best_evict_starts[a]; best_evict_starts[a] = best_evict_starts[b]; best_evict_starts[b] = t; }

    for (int k = 0; k < best_evict_n; k++)
    {
        WeaponType victim = p.inventory[best_evict_starts[k]].weapon;
        if (victim != WeaponType::None)
            inventory_swap_to_long_term(p, victim);
    }

    // Verify the chosen window is fully free now (it must be, by construction).
    for (int j = 0; j < need; j++)
        if (p.inventory[best_start + j].weapon != WeaponType::None)
            return -1;

    place_at(best_start);
    return best_start;
}

void inventory_swap_to_long_term(PlayerCharacter &p, WeaponType w)
{
    if (p.long_term_count >= MAX_LONG_TERM)
        return;

    // Find FIRST instance of this weapon (by is_start marker)
    int start = -1;
    for (int i = 0; i < INVENTORY_SLOTS; i++)
    {
        if (p.inventory[i].weapon == w && p.inventory[i].is_start)
        {
            start = i;
            break;
        }
    }
    if (start < 0)
        return;

    // Remove only THIS one instance (its contiguous slots)
    int slot_size = WEAPON_SLOTS[(int)w];
    for (int j = 0; j < slot_size && (start + j) < INVENTORY_SLOTS; j++)
    {
        p.inventory[start + j].weapon = WeaponType::None;
        p.inventory[start + j].is_start = false;
    }

    p.long_term[p.long_term_count].weapon = w;
    p.long_term[p.long_term_count].occupied = true;
    p.long_term_count++;

    // Section 6 viz: red flash on the slots that were just emptied
    int pidx = player_index_of(p);
    if (pidx >= 0) set_inv_anim_evict(pidx, start, slot_size);

    log_action("P%d: %s moved to long-term storage.",
               pidx >= 0 ? pidx + 1 : 0, WEAPON_NAMES[(int)w]);
}

// ─────────────────────────────────────────────
//  Set animation hint for renderer
//  MUST be called with state_lock held
// ─────────────────────────────────────────────
static void set_anim(int kind, int attacker, bool atk_is_player,
                     int target, bool tgt_is_player, int dmg)
{
    g_state->anim_kind = kind;
    g_state->anim_attacker = attacker;
    g_state->anim_attacker_is_player = atk_is_player;
    g_state->anim_target = target;
    g_state->anim_target_is_player = tgt_is_player;
    g_state->anim_damage = dmg;
    g_state->anim_frames_left = 30; // 1.0s at 30 fps — smoother
}

// ─────────────────────────────────────────────
//  APPLY ACTION
//  Called by scheduler after HIP/ASP submit action
// ─────────────────────────────────────────────
void apply_action(const ActionRequest &req)
{
    pthread_mutex_lock(&g_state->state_lock);

    if (req.source_is_player)
    {
        PlayerCharacter &src = g_state->players[req.source_id];

        switch (req.type)
        {

        case ActionType::Strike:
        {
            src.stamina = 0.0f;
            if (g_state->pvp_mode)
            {
                PlayerCharacter &tgt = g_state->players[req.target_id];
                tgt.hp -= src.damage;
                if (tgt.hp < 0)
                    tgt.hp = 0;
                set_anim(1, req.source_id, true, req.target_id, true, src.damage);
                log_action("P%d strikes P%d for %d dmg!", req.source_id + 1, req.target_id + 1, src.damage);
                if (tgt.alive && !tgt.stunned && (rand() % 5 == 0))
                {
                    tgt.stunned = true;
                    tgt.stun_apply_ns = now_ns();
                    pid_t target_tid = g_state->player_tid[req.target_id];
                    if (g_hip_pid > 0 && target_tid > 0)
                        thread_kill(g_hip_pid, target_tid, SIGUSR1);
                    log_action("P%d STUNNED P%d for 3s!", req.source_id + 1, req.target_id + 1);
                }
                break;
            }
            EnemyCharacter &tgt = g_state->enemies[req.target_id];
            tgt.hp -= src.damage;
            if (tgt.hp < 0)
                tgt.hp = 0;
            set_anim(1, req.source_id, true, req.target_id, false, src.damage);
            log_action("P%d strikes E%d for %d dmg!", req.source_id + 1, req.target_id + 1, src.damage);

            if (tgt.alive && !tgt.stunned && (rand() % 5 == 0))
            {
                tgt.stunned = true;
                tgt.stun_apply_ns = now_ns();
                pid_t target_tid = g_state->enemy_tid[req.target_id];
                if (g_asp_pid > 0 && target_tid > 0)
                    thread_kill(g_asp_pid, target_tid, SIGUSR1);
                log_action("P%d STUNNED E%d for 3s!", req.source_id + 1, req.target_id + 1);
            }
            break;
        }

        case ActionType::Exhaust:
        {
            src.stamina = 0.0f;
            if (g_state->pvp_mode)
            {
                PlayerCharacter &tgt = g_state->players[req.target_id];
                tgt.stamina -= src.damage;
                if (tgt.stamina < 0)
                    tgt.stamina = 0;
                set_anim(3, req.source_id, true, req.target_id, true, src.damage);
                log_action("P%d exhausts P%d stamina by %d!", req.source_id + 1, req.target_id + 1, src.damage);
                break;
            }
            EnemyCharacter &tgt = g_state->enemies[req.target_id];
            tgt.stamina -= src.damage;
            if (tgt.stamina < 0)
                tgt.stamina = 0;
            set_anim(3, req.source_id, true, req.target_id, false, src.damage);
            log_action("P%d exhausts E%d stamina by %d!", req.source_id + 1, req.target_id + 1, src.damage);
            break;
        }

        case ActionType::UseWeapon:
        {
            // weapon_slot holds the inventory slot index
            WeaponType w = src.inventory[req.weapon_slot].weapon;
            int dmg = WEAPON_DAMAGE[(int)w];
            bool can_use_weapon = true;

            if (!g_state->pvp_mode && weapon_is_artifact(w))
            {
                pthread_mutex_lock(&g_state->resource_table.table_lock);
                ArtifactID aid = ArtifactID::SolarCore;
                if (w == WeaponType::LunarBlade)
                    aid = ArtifactID::LunarBlade;
                if (w == WeaponType::EclipseRelic)
                    aid = ArtifactID::EclipseRelic;

                ArtifactEntry &ae = g_state->resource_table.entries[(int)aid];
                if (ae.exists && ae.is_free)
                {
                    ae.is_free = false;
                    ae.held_by_player = true;
                    ae.holder_index = req.source_id;
                    if (ae.has_waiter && ae.waiter_is_player && ae.waiter_index == req.source_id)
                    {
                        ae.has_waiter = false;
                        ae.waiter_is_player = false;
                        ae.waiter_index = -1;
                    }
                }
                else if (!ae.is_free &&
                         !(ae.held_by_player && ae.holder_index == req.source_id))
                {
                    ae.has_waiter = true;
                    ae.waiter_is_player = true;
                    ae.waiter_index = req.source_id;
                    can_use_weapon = false;
                }
                pthread_mutex_unlock(&g_state->resource_table.table_lock);
            }

            src.stamina = 0.0f;
            if (!can_use_weapon)
            {
                set_anim(5, req.source_id, true, req.source_id, true, 0);
                log_action("P%d cannot use %s now (artifact locked by another holder).",
                           req.source_id + 1, WEAPON_NAMES[(int)w]);
                break;
            }

            if (g_state->pvp_mode)
            {
                PlayerCharacter &tgt = g_state->players[req.target_id];
                tgt.hp -= dmg;
                if (tgt.hp < 0)
                    tgt.hp = 0;
                set_anim(2, req.source_id, true, req.target_id, true, dmg);
                log_action("P%d uses %s on P%d for %d dmg!",
                           req.source_id + 1, WEAPON_NAMES[(int)w], req.target_id + 1, dmg);
            }
            else
            {
                EnemyCharacter &tgt = g_state->enemies[req.target_id];
                tgt.hp -= dmg;
                if (tgt.hp < 0)
                    tgt.hp = 0;
                set_anim(2, req.source_id, true, req.target_id, false, dmg);
                log_action("P%d uses %s on E%d for %d dmg!",
                           req.source_id + 1, WEAPON_NAMES[(int)w], req.target_id + 1, dmg);
            }
            break;
        }

        case ActionType::SwapIn:
        {
            WeaponType w = req.swap_weapon;
            for (int i = 0; i < src.long_term_count; i++)
            {
                if (src.long_term[i].weapon == w && src.long_term[i].occupied)
                {
                    src.long_term[i].occupied = false;
                    for (int j = i; j < src.long_term_count - 1; j++)
                        src.long_term[j] = src.long_term[j + 1];
                    src.long_term_count--;
                    break;
                }
            }
            int placed_at = inventory_place_weapon(src, w);
            src.just_swapped_in = true;
            src.stamina = 0.0f;
            if (placed_at >= 0)
                set_inv_anim_swapin(req.source_id, placed_at,
                                    WEAPON_SLOTS[(int)w]);
            set_anim(6, req.source_id, true, req.source_id, true, 0);
            log_action("P%d swaps in %s (cannot use this turn).",
                       req.source_id + 1, WEAPON_NAMES[(int)w]);
            break;
        }

        case ActionType::MoveToLongTerm:
        {
            WeaponType w = req.swap_weapon;
            if (w == WeaponType::None || find_weapon_in_inventory(src, w) < 0)
            {
                src.stamina = 0.0f;
                set_anim(5, req.source_id, true, req.source_id, true, 0);
                log_action("P%d move to long-term failed (weapon not in inventory).",
                           req.source_id + 1);
                break;
            }
            if (src.long_term_count >= MAX_LONG_TERM)
            {
                src.stamina = 0.0f;
                set_anim(5, req.source_id, true, req.source_id, true, 0);
                log_action("P%d move to long-term failed (storage full).",
                           req.source_id + 1);
                break;
            }
            inventory_swap_to_long_term(src, w);
            src.stamina = 0.0f;
            set_anim(6, req.source_id, true, req.source_id, true, 0);
            break;
        }

        case ActionType::Heal:
        {
            int heal = src.max_hp / 10; // 10%
            src.hp += heal;
            if (src.hp > src.max_hp)
                src.hp = src.max_hp;
            src.stamina = 0.0f;
            set_anim(4, req.source_id, true, req.source_id, true, heal);
            log_action("P%d heals for %d HP!", req.source_id + 1, heal);
            break;
        }

        case ActionType::Skip:
        {
            src.stamina = 0.5f * (float)MAX_STAMINA_PLAYER;
            src.ready_to_act = false;
            set_anim(5, req.source_id, true, req.source_id, true, 0);
            log_action("P%d skips turn.", req.source_id + 1);
            break;
        }

        case ActionType::Ultimate:
        {
            if (player_has_ultimate(src))
            {
                src.stamina = 0.0f;
                if (!g_state->pvp_mode)
                {
                    g_state->phase = GamePhase::Ultimate;
                    kill(g_asp_pid, SIGSTOP);
                    alarm(CHRONO_ULTIMATE_DURATION_S);
                }
                set_anim(7, req.source_id, true, req.source_id, true, 0);
                if (g_state->pvp_mode)
                    log_action("P%d triggers ULTIMATE!", req.source_id + 1);
                else
                    log_action("P%d triggers ULTIMATE! Enemies frozen 10s!", req.source_id + 1);
            }
            break;
        }

        default:
            break;
        }

        src.ready_to_act = false;
    }
    else
    {
        EnemyCharacter &src = g_state->enemies[req.source_id];

        switch (req.type)
        {

        case ActionType::Strike:
        {
            PlayerCharacter &tgt = g_state->players[req.target_id];
            tgt.hp -= src.damage;
            if (tgt.hp < 0)
                tgt.hp = 0;
            src.stamina = 0.0f;
            set_anim(1, req.source_id, false, req.target_id, true, src.damage);
            log_action("E%d strikes P%d for %d dmg!", req.source_id + 1, req.target_id + 1, src.damage);

            if (tgt.alive && !tgt.stunned && (rand() % 5 == 0))
            {
                tgt.stunned = true;
                tgt.stun_apply_ns = now_ns();
                pid_t target_tid = g_state->player_tid[req.target_id];
                if (g_hip_pid > 0 && target_tid > 0)
                    thread_kill(g_hip_pid, target_tid, SIGUSR1);
                log_action("E%d STUNNED P%d for 3s!", req.source_id + 1, req.target_id + 1);
            }
            break;
        }

        case ActionType::Skip:
        {
            // Skip sets stamina to 50% capacity.
            src.stamina = 0.5f * (float)MAX_STAMINA_ENEMY;
            src.ready_to_act = false;
            set_anim(5, req.source_id, false, req.source_id, false, 0);
            log_action("E%d skips turn.", req.source_id + 1);
            break;
        }

        default:
            break;
        }

        src.ready_to_act = false;
    }

    g_state->turn.action_applied = true;
    g_state->turn.action_submitted = false;

    pthread_mutex_unlock(&g_state->state_lock);
}

// ─────────────────────────────────────────────
//  RELEASE ENTITY ARTIFACTS
// ─────────────────────────────────────────────
void release_entity_artifacts(int entity_idx, bool is_player)
{
    pthread_mutex_lock(&g_state->resource_table.table_lock);
    for (int a = 0; a < (int)ArtifactID::COUNT; a++)
    {
        ArtifactEntry &ae = g_state->resource_table.entries[a];

        if (!ae.is_free && ae.held_by_player == is_player &&
            ae.holder_index == entity_idx)
        {
            ae.is_free = true;
            ae.held_by_player = false;
            ae.holder_index = -1;
            log_action("%s%d defeated -> artifact released.",
                       is_player ? "P" : "E", entity_idx + 1);
        }

        if (ae.has_waiter && ae.waiter_is_player == is_player &&
            ae.waiter_index == entity_idx)
        {
            ae.has_waiter = false;
            ae.waiter_is_player = false;
            ae.waiter_index = -1;
        }
    }
    pthread_mutex_unlock(&g_state->resource_table.table_lock);
}

// ─────────────────────────────────────────────
//  HANDLE PLAYER DEATH
// ─────────────────────────────────────────────
void handle_player_death(int idx)
{
    g_state->players[idx].alive = false;
    log_action("P%d has been defeated!", idx + 1);

    release_entity_artifacts(idx, true);

    sem_post(&g_state->player_turn_sem[idx]);
}

// ─────────────────────────────────────────────
//  WEAPON DROP PROMPT
// ─────────────────────────────────────────────
static void give_drop_to_enemy_guaranteed(WeaponType dropped)
{
    int picker = -1;
    for (int i = 0; i < g_state->enemy_count; i++)
    {
        if (g_state->enemies[i].alive &&
            g_state->enemies[i].held_weapon == WeaponType::None)
        {
            picker = i;
            break;
        }
    }
    if (picker == -1)
    {
        int alive_indices[MAX_ENEMIES];
        int n_alive = 0;
        for (int i = 0; i < g_state->enemy_count; i++)
            if (g_state->enemies[i].alive)
                alive_indices[n_alive++] = i;
        if (n_alive > 0)
            picker = alive_indices[rand() % n_alive];
    }
    if (picker >= 0)
    {
        g_state->enemies[picker].held_weapon = dropped;
        log_action("E%d picked up %s!", picker + 1, WEAPON_NAMES[(int)dropped]);

        if (weapon_is_artifact(dropped))
        {
            ArtifactID aid = ArtifactID::SolarCore;
            if (dropped == WeaponType::LunarBlade)
                aid = ArtifactID::LunarBlade;
            if (dropped == WeaponType::EclipseRelic)
                aid = ArtifactID::EclipseRelic;
            pthread_mutex_lock(&g_state->resource_table.table_lock);
            ArtifactEntry &ae = g_state->resource_table.entries[(int)aid];
            ae.is_free = false;
            ae.held_by_player = false;
            ae.holder_index = picker;
            pthread_mutex_unlock(&g_state->resource_table.table_lock);
        }
    }
}

static void run_weapon_pickup_modal_with_target(int drop_target, WeaponType dropped)
{
    while (sem_trywait(&g_state->weapon_drop_sem) == 0)
        ;

    if (drop_target < 0)
    {
        give_drop_to_enemy_guaranteed(dropped);
        return;
    }

    pthread_mutex_lock(&g_state->state_lock);
    g_state->weapon_drop_pending = true;
    g_state->dropped_weapon = dropped;
    g_state->drop_for_player = drop_target;
    g_state->player_accepted_drop = false;
    g_state->drop_decision_ready = false;
    g_state->weapon_drop_deadline_ns = now_ns() + 5LL * 1000000000LL;
    pthread_mutex_unlock(&g_state->state_lock);

    sem_post(&g_state->player_turn_sem[drop_target]);

    struct timespec ts_drop;
    clock_gettime(CLOCK_REALTIME, &ts_drop);
    ts_drop.tv_sec += 5;
    int r;
    do
    {
        r = sem_timedwait(&g_state->weapon_drop_sem, &ts_drop);
    } while (r == -1 && errno == EINTR);

    if (g_state->player_accepted_drop && drop_target >= 0)
    {
        inventory_place_weapon(g_state->players[drop_target], dropped);
        log_action("P%d picked up %s!", drop_target + 1, WEAPON_NAMES[(int)dropped]);

        if (weapon_is_artifact(dropped))
        {
            ArtifactID aid = ArtifactID::SolarCore;
            if (dropped == WeaponType::LunarBlade)
                aid = ArtifactID::LunarBlade;
            if (dropped == WeaponType::EclipseRelic)
                aid = ArtifactID::EclipseRelic;
            pthread_mutex_lock(&g_state->resource_table.table_lock);
            ArtifactEntry &ae = g_state->resource_table.entries[(int)aid];
            ae.is_free = false;
            ae.held_by_player = true;
            ae.holder_index = drop_target;
            pthread_mutex_unlock(&g_state->resource_table.table_lock);
        }
    }
    else
        give_drop_to_enemy_guaranteed(dropped);

    pthread_mutex_lock(&g_state->state_lock);
    g_state->weapon_drop_pending = false;
    g_state->drop_decision_ready = false;
    g_state->gui_pressed_key = -1;
    g_state->gui_prompt_mode = 0;
    g_state->gui_prompt_max = 0;
    g_state->gui_prompt_player = -1;
    g_state->gui_prompt_text[0] = '\0';
    pthread_mutex_unlock(&g_state->state_lock);

    while (sem_trywait(&g_state->gui_input_sem) == 0)
        ;
}

static void perform_weapon_drop_prompt(int defeated_enemy_1based, WeaponType dropped)
{
    int drop_target = -1;
    if (g_state->turn.owner == TurnOwner::Player &&
        g_state->turn.entity_index >= 0 &&
        g_state->turn.entity_index < g_state->player_count &&
        g_state->players[g_state->turn.entity_index].alive)
    {
        drop_target = g_state->turn.entity_index;
    }
    else
    {
        for (int p = 0; p < g_state->player_count; p++)
            if (g_state->players[p].alive) { drop_target = p; break; }
    }

    log_action("E%d dropped %s! Pick up? (HIP prompt)", defeated_enemy_1based,
               WEAPON_NAMES[(int)dropped]);

    run_weapon_pickup_modal_with_target(drop_target, dropped);
}

// ─────────────────────────────────────────────
//  HANDLE ENEMY DEATH
// ─────────────────────────────────────────────
void handle_enemy_death(int idx)
{
    EnemyCharacter &e = g_state->enemies[idx];
    const WeaponType carrier_art =
        (e.held_weapon == WeaponType::SolarCore || e.held_weapon == WeaponType::LunarBlade)
            ? e.held_weapon
            : WeaponType::None;

    const int defeated_label = idx + 1;

    WeaponType random_drop_weapon = WeaponType::None;
    if (carrier_art == WeaponType::None && e.held_weapon == WeaponType::None)
    {
        if (rand() % 2 == 0)
        {
            WeaponType drops[10];
            int drop_count = 0;
            drops[drop_count++] = WeaponType::IronHalberd;
            drops[drop_count++] = WeaponType::VenomDagger;
            drops[drop_count++] = WeaponType::Thunderstaff;
            drops[drop_count++] = WeaponType::ObsidianAxe;
            drops[drop_count++] = WeaponType::Frostbow;
            drops[drop_count++] = WeaponType::SplinterStick;

            pthread_mutex_lock(&g_state->resource_table.table_lock);
            if (g_state->resource_table.entries[(int)ArtifactID::SolarCore].is_free)
                drops[drop_count++] = WeaponType::SolarCore;
            if (g_state->resource_table.entries[(int)ArtifactID::LunarBlade].is_free)
                drops[drop_count++] = WeaponType::LunarBlade;
            pthread_mutex_unlock(&g_state->resource_table.table_lock);

            random_drop_weapon = drops[rand() % drop_count];
        }
    }

    e.alive = false;
    g_state->total_kills++;

    release_entity_artifacts(idx, false);

    if (carrier_art != WeaponType::None)
        e.held_weapon = WeaponType::None;

    sem_post(&g_state->enemy_turn_sem[idx]);

    log_action("E%d defeated! Total kills: %d", defeated_label, g_state->total_kills);

    if (g_state->total_kills < TOTAL_KILLS_TO_WIN)
    {
        init_enemy_slot_from_spec(g_state->enemies[idx]);
        log_action("New enemy E%d appeared!", idx + 1);
    }

    if (carrier_art != WeaponType::None)
        perform_weapon_drop_prompt(defeated_label, carrier_art);
    else if (random_drop_weapon != WeaponType::None)
        perform_weapon_drop_prompt(defeated_label, random_drop_weapon);

    if (g_state->total_kills == g_eclipse_spawn_kill)
    {
        pthread_mutex_lock(&g_state->resource_table.table_lock);
        g_state->resource_table.entries[(int)ArtifactID::EclipseRelic].exists = true;
        g_state->resource_table.entries[(int)ArtifactID::EclipseRelic].is_free = true;
        pthread_mutex_unlock(&g_state->resource_table.table_lock);

        log_action("Eclipse Relic appeared! Player prompted to pick up.");

        int relic_target = -1;
        if (g_state->turn.owner == TurnOwner::Player &&
            g_state->turn.entity_index >= 0 &&
            g_state->turn.entity_index < g_state->player_count &&
            g_state->players[g_state->turn.entity_index].alive)
        {
            relic_target = g_state->turn.entity_index;
        }
        else
        {
            for (int p = 0; p < g_state->player_count; p++)
                if (g_state->players[p].alive) { relic_target = p; break; }
        }

        run_weapon_pickup_modal_with_target(relic_target, WeaponType::EclipseRelic);
    }
}

// ─────────────────────────────────────────────
//  CHECK WIN / LOSE
//  Returns true if game is over
// ─────────────────────────────────────────────
bool check_win_lose()
{
    if (g_quit_requested)
    {
        g_state->result = GameResult::Quit;
        g_state->phase = GamePhase::GameOver;
        return true;
    }
    if (g_state->pvp_mode)
    {
        bool p1_alive = (g_state->player_count > 0) ? g_state->players[0].alive : false;
        bool p2_alive = (g_state->player_count > 1) ? g_state->players[1].alive : false;
        if (p1_alive != p2_alive)
        {
            g_state->result = p1_alive ? GameResult::Win : GameResult::Lose;
            g_state->phase = GamePhase::GameOver;
            log_action("PVP OVER! %s wins the duel.", p1_alive ? "P1" : "P2");
            return true;
        }
        if (!p1_alive && !p2_alive)
        {
            g_state->result = GameResult::Lose;
            g_state->phase = GamePhase::GameOver;
            log_action("PVP OVER! Both duelists fell.");
            return true;
        }
    }
    if (!g_state->pvp_mode && g_state->total_kills >= TOTAL_KILLS_TO_WIN)
    {
        g_state->result = GameResult::Win;
        g_state->phase = GamePhase::GameOver;
        log_action("VICTORY! All enemies defeated!");
        return true;
    }
    bool any_alive = false;
    for (int i = 0; i < g_state->player_count; i++)
    {
        if (g_state->players[i].alive)
        {
            any_alive = true;
            break;
        }
    }
    if (!any_alive)
    {
        g_state->result = GameResult::Lose;
        g_state->phase = GamePhase::GameOver;
        log_action("DEFEAT! All players have fallen!");
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────
//  SCHEDULER THREAD
//  Stamina tick every second + turn logic
// ─────────────────────────────────────────────
void *scheduler_thread(void *)
{
    while (true)
    {
        sleep(1); // 1-second tick

        pthread_mutex_lock(&g_state->state_lock);
        bool over = (g_state->phase == GamePhase::GameOver);
        pthread_mutex_unlock(&g_state->state_lock);
        if (over)
            break;

        if (g_quit_requested)
            break;

        if (g_hip_died || g_asp_died)
        {
            log_action("%s exited unexpectedly — shutting down.",
                       g_hip_died ? "HIP" : "ASP");
            for (int i = 0; i < MAX_PLAYERS; i++)
                sem_post(&g_state->player_turn_sem[i]);
            for (int i = 0; i < MAX_ENEMIES; i++)
                sem_post(&g_state->enemy_turn_sem[i]);
            sem_post(&g_state->action_ready_sem);
            g_quit_requested = 1;
            break;
        }

        if (g_ultimate_fired)
        {
            g_ultimate_fired = 0;
            g_state->phase = GamePhase::Running;
            log_action("Ultimate window ended — enemies resume!");
        }

        pthread_mutex_lock(&g_state->state_lock);

        g_scheduler_tick++;

        long long t_now = now_ns();
        for (int i = 0; i < g_state->player_count; i++)
        {
            PlayerCharacter &p = g_state->players[i];
            if (p.stunned && (t_now - p.stun_apply_ns >= CHRONO_STUN_DURATION_NS))
            {
                p.stunned = false;
                log_action("P%d recovered from stun!", i + 1);
            }
        }
        for (int i = 0; i < g_state->enemy_count; i++)
        {
            EnemyCharacter &e = g_state->enemies[i];
            if (e.stunned && (t_now - e.stun_apply_ns >= CHRONO_STUN_DURATION_NS))
            {
                e.stunned = false;
                log_action("E%d recovered from stun!", i + 1);
            }
        }

        for (int i = 0; i < g_state->player_count; i++)
        {
            PlayerCharacter &p = g_state->players[i];
            if (!p.alive || p.stunned)
                continue;
            p.stamina += p.speed;
            if (p.stamina >= MAX_STAMINA_PLAYER)
            {
                p.stamina = MAX_STAMINA_PLAYER;
                if (!p.ready_to_act)
                {
                    p.ready_to_act = true;
                    p.fill_tick = g_scheduler_tick;
                }
            }
        }
        for (int i = 0; i < g_state->enemy_count; i++)
        {
            EnemyCharacter &e = g_state->enemies[i];
            if (!e.alive || e.stunned)
                continue;
            if (g_state->phase == GamePhase::Ultimate)
                continue;
            e.stamina += e.speed;
            if (e.stamina >= MAX_STAMINA_ENEMY)
            {
                e.stamina = MAX_STAMINA_ENEMY;
                if (!e.ready_to_act)
                {
                    e.ready_to_act = true;
                    e.fill_tick = g_scheduler_tick;
                }
            }
        }

        int acting_player = -1;
        int acting_enemy = -1;
        int best_p_tick = INT_MAX;
        int best_e_tick = INT_MAX;

        for (int i = 0; i < g_state->player_count; i++)
        {
            PlayerCharacter &p = g_state->players[i];
            if (p.alive && p.stunned && p.ready_to_act)
            {
                p.ready_to_act = false;
                continue;
            }
            if (p.alive && p.ready_to_act && !p.stunned)
            {
                if (p.fill_tick < best_p_tick)
                {
                    best_p_tick = p.fill_tick;
                    acting_player = i;
                }
            }
        }
        for (int i = 0; i < g_state->enemy_count; i++)
        {
            EnemyCharacter &e = g_state->enemies[i];
            if (e.alive && e.stunned && e.ready_to_act)
            {
                e.ready_to_act = false;
                continue;
            }
            if (e.alive && e.ready_to_act && !e.stunned &&
                g_state->phase != GamePhase::Ultimate)
            {
                if (e.fill_tick < best_e_tick)
                {
                    best_e_tick = e.fill_tick;
                    acting_enemy = i;
                }
            }
        }

        if (acting_player != -1 && acting_enemy != -1)
        {
            if (best_e_tick < best_p_tick)
                acting_player = -1;
        }

        pthread_mutex_unlock(&g_state->state_lock);

        // ── Process turn: player takes priority if both ready ──
        if (acting_player != -1)
        {
            pthread_mutex_lock(&g_state->state_lock);
            // Clear just_swapped_in at turn start — weapon usable from this turn
            g_state->players[acting_player].just_swapped_in = false;
            g_state->turn.owner = TurnOwner::Player;
            g_state->turn.entity_index = acting_player;
            g_state->turn.action_submitted = false;
            g_state->turn.action_applied = false;
            g_state->gui_action_label[0] = '\0';
            sem_post(&g_state->player_turn_sem[acting_player]);
            pthread_mutex_unlock(&g_state->state_lock);

            // Block until HIP submits action — no timeout for human players.
            // If quit is requested while blocked (SIGTERM), do not re-enter an
            // endless EINTR retry loop; let the scheduler exit cleanly.
            for (;;)
            {
                if (g_quit_requested)
                    break;
                if (sem_wait(&g_state->action_ready_sem) == 0)
                    break;
                if (errno == EINTR)
                    continue;
                break;
            }

            if (g_quit_requested)
                break;

            // Apply action to game state
            apply_action(g_state->pending_action);

            // Check if any enemy died after this action
            for (int i = 0; i < g_state->enemy_count; i++)
            {
                if (g_state->enemies[i].alive && g_state->enemies[i].hp <= 0)
                    handle_enemy_death(i);
            }
            // Check if any player died — Section 2 Lifecycle Management:
            // release artifacts and wake the player thread.
            for (int i = 0; i < g_state->player_count; i++)
            {
                if (g_state->players[i].alive && g_state->players[i].hp <= 0)
                    handle_player_death(i);
            }
        }
        else if (acting_enemy != -1)
        {
            pthread_mutex_lock(&g_state->state_lock);
            g_state->turn.owner = TurnOwner::Enemy;
            g_state->turn.entity_index = acting_enemy;
            g_state->turn.action_submitted = false;
            g_state->turn.action_applied = false;
            g_state->gui_action_label[0] = '\0';
            sem_post(&g_state->enemy_turn_sem[acting_enemy]);
            pthread_mutex_unlock(&g_state->state_lock);

            // Wait max 3 seconds — NPC timeout rule (Section 8)
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 3;

            int rc;
            do {
                rc = sem_timedwait(&g_state->action_ready_sem, &ts);
            } while (rc == -1 && errno == EINTR);
            if (rc < 0 && errno == ETIMEDOUT)
            {
                // Timeout — Arbiter auto-applies Skip for this enemy.
                // Section 3: committed action (auto-Skip) depletes stamina to zero.
                pthread_mutex_lock(&g_state->state_lock);
                g_state->enemies[acting_enemy].stamina = 0.0f;
                g_state->enemies[acting_enemy].ready_to_act = false;
                pthread_mutex_unlock(&g_state->state_lock);
                log_action("E%d timed out — auto Skip.", acting_enemy + 1);
            }
            else
            {
                apply_action(g_state->pending_action);
                // Check player deaths — Section 2 Lifecycle Management
                for (int i = 0; i < g_state->player_count; i++)
                {
                    if (g_state->players[i].alive && g_state->players[i].hp <= 0)
                        handle_player_death(i);
                }
            }
        }

        // ── Check win/lose after every action ──
        if (check_win_lose())
            break;
    }

    return nullptr;
}

// ─────────────────────────────────────────────
//  Section 7 — strip weapon run; caller holds state_lock.
// ─────────────────────────────────────────────
static void strip_weapon_slots(PlayerCharacter &p, WeaponType w)
{
    int s = find_weapon_in_inventory(p, w);
    if (s < 0)
        return;
    int n = WEAPON_SLOTS[(int)w];
    for (int i = 0; i < n && s + i < INVENTORY_SLOTS; i++)
    {
        p.inventory[s + i].weapon = WeaponType::None;
        p.inventory[s + i].is_start = false;
    }
}

// Caller holds state_lock and resource_table.table_lock (in that order).
static void apply_interactive_solar_lunar_resolution(int choice, int idx_solar, int idx_lunar)
{
    if (!g_state || idx_solar < 0 || idx_lunar < 0 ||
        idx_solar >= g_state->player_count || idx_lunar >= g_state->player_count)
        return;

    PlayerCharacter &ps = g_state->players[idx_solar];
    PlayerCharacter &pl = g_state->players[idx_lunar];
    ArtifactEntry &es = g_state->resource_table.entries[(int)ArtifactID::SolarCore];
    ArtifactEntry &el = g_state->resource_table.entries[(int)ArtifactID::LunarBlade];

    if (choice == 1)
    {
        int ev = find_weapon_in_inventory(ps, WeaponType::SolarCore);
        int sz = WEAPON_SLOTS[(int)WeaponType::SolarCore];
        strip_weapon_slots(ps, WeaponType::SolarCore);
        if (ev >= 0)
            set_inv_anim_evict(idx_solar, ev, sz);

        if (inventory_place_weapon(pl, WeaponType::SolarCore) < 0)
            log_action("Deadlock resolve: could not place Solar Core for P%d.", idx_lunar + 1);

        es.is_free = false;
        es.held_by_player = true;
        es.holder_index = idx_lunar;
        es.has_waiter = false;
        es.waiter_is_player = false;
        es.waiter_index = -1;

        el.has_waiter = false;
        el.waiter_is_player = false;
        el.waiter_index = -1;

        log_action("Deadlock resolved: P%d released Solar Core — P%d has both for Ultimate.",
                   idx_solar + 1, idx_lunar + 1);
    }
    else
    {
        int ev = find_weapon_in_inventory(pl, WeaponType::LunarBlade);
        int sz = WEAPON_SLOTS[(int)WeaponType::LunarBlade];
        strip_weapon_slots(pl, WeaponType::LunarBlade);
        if (ev >= 0)
            set_inv_anim_evict(idx_lunar, ev, sz);

        if (inventory_place_weapon(ps, WeaponType::LunarBlade) < 0)
            log_action("Deadlock resolve: could not place Lunar Blade for P%d.", idx_solar + 1);

        el.is_free = false;
        el.held_by_player = true;
        el.holder_index = idx_solar;
        el.has_waiter = false;
        el.waiter_is_player = false;
        el.waiter_index = -1;

        es.has_waiter = false;
        es.waiter_is_player = false;
        es.waiter_index = -1;

        log_action("Deadlock resolved: P%d released Lunar Blade — P%d has both for Ultimate.",
                   idx_lunar + 1, idx_solar + 1);
    }
}

// ─────────────────────────────────────────────
//  DEADLOCK MONITOR THREAD
//  Runs in background, detects circular waits
//  on artifacts and forces resolution
// ─────────────────────────────────────────────
void *deadlock_monitor_thread(void *)
{
    while (true)
    {
        sleep(2); // Check every 2 seconds

        // Section 4 — read shared phase under the lock for data consistency
        pthread_mutex_lock(&g_state->state_lock);
        bool over = (g_state->phase == GamePhase::GameOver);
        pthread_mutex_unlock(&g_state->state_lock);
        if (over)
            break;

        pthread_mutex_lock(&g_state->resource_table.table_lock);

        ResourceTable &rt = g_state->resource_table;
        // Simple circular wait detection:
        // For each pair of artifacts (A, B):
        //   Entity X holds A and waits for B
        //   Entity Y holds B and waits for A
        //   → Deadlock

        bool deadlock = false;
        int victim_artifact = -1;
        int other_artifact  = -1;

        for (int a = 0; a < (int)ArtifactID::COUNT && !deadlock; a++)
        {
            if (!rt.entries[a].exists || rt.entries[a].is_free)
                continue;
            for (int b = 0; b < (int)ArtifactID::COUNT && !deadlock; b++)
            {
                if (a == b)
                    continue;
                if (!rt.entries[b].exists || rt.entries[b].is_free)
                    continue;

                ArtifactEntry &ea = rt.entries[a];
                ArtifactEntry &eb = rt.entries[b];

                // Section 7: circular wait = X holds A and waits on B, Y holds B
                // and waits on A (holder/waiter identity includes player vs enemy).
                bool cross_wait =
                    (!ea.is_free && !eb.is_free) && ea.has_waiter && eb.has_waiter &&
                    (ea.waiter_index == eb.holder_index) &&
                    (ea.waiter_is_player == eb.held_by_player) &&
                    (eb.waiter_index == ea.holder_index) &&
                    (eb.waiter_is_player == ea.held_by_player);

                if (cross_wait)
                {
                    deadlock = true;
                    victim_artifact = a;
                    other_artifact  = b;
                }
            }
        }

        // PvP Solar/Lunar stalemate: each artifact held by a *different* player.

        ArtifactEntry &es = rt.entries[(int)ArtifactID::SolarCore];
        ArtifactEntry &el = rt.entries[(int)ArtifactID::LunarBlade];
        bool pvp_solar_lunar = false;
        int idx_solar = -1;
        int idx_lunar = -1;
        if (es.exists && el.exists && !es.is_free && !el.is_free &&
            es.held_by_player && el.held_by_player &&
            es.holder_index >= 0 && el.holder_index >= 0 &&
            es.holder_index != el.holder_index)
        {
            pthread_mutex_lock(&g_state->state_lock);
            int pc = g_state->player_count;
            pthread_mutex_unlock(&g_state->state_lock);
            if (pc >= 2)
            {
                pvp_solar_lunar = true;
                idx_solar = es.holder_index;
                idx_lunar = el.holder_index;
            }
        }

        if (pvp_solar_lunar)
        {
            pthread_mutex_lock(&g_state->state_lock);
            bool busy = g_state->deadlock_modal_pending;
            bool drop_p = g_state->weapon_drop_pending;

            bool solar_in_subprompt =
                (g_state->gui_prompt_mode == 1 &&
                 g_state->gui_prompt_player == idx_solar &&
                 g_state->gui_prompt_max > 2);
            pthread_mutex_unlock(&g_state->state_lock);
            pthread_mutex_unlock(&g_state->resource_table.table_lock);
            // Never stack deadlock UI on top of a weapon / relic pickup modal
            // (scheduler may still be in run_weapon_pickup_modal).
            if (busy || drop_p || solar_in_subprompt)
                continue;

            pthread_mutex_lock(&g_state->state_lock);
            snprintf(g_state->deadlock_line_main, sizeof(g_state->deadlock_line_main),
                     "Deadlock: P%d has Solar Core and P%d has Lunar Blade.",
                     idx_solar + 1, idx_lunar + 1);
            snprintf(g_state->deadlock_line_opt1, sizeof(g_state->deadlock_line_opt1),
                     "1 - P%d releases Solar Core -> P%d gets both (Ultimate)",
                     idx_solar + 1, idx_lunar + 1);
            snprintf(g_state->deadlock_line_opt2, sizeof(g_state->deadlock_line_opt2),
                     "2 - P%d releases Lunar Blade -> P%d gets both (Ultimate)",
                     idx_lunar + 1, idx_solar + 1);
            g_state->deadlock_modal_pending = true;
            g_state->gui_pressed_key = -1;
            // Prime routing immediately so SFML accepts 1/2 before the solar

            g_state->gui_prompt_mode     = 1;
            g_state->gui_prompt_max      = 2;
            g_state->gui_prompt_player   = idx_solar;
            g_state->gui_prompt_text[0]  = '\0';
            g_state->deadlock_user_choice = 0;
            g_state->deadlock_idx_solar = idx_solar;
            g_state->deadlock_idx_lunar = idx_lunar;
            g_state->deadlock_prompt_player = idx_solar;
            g_state->deadlock_modal_deadline_ns = now_ns() + 60LL * 1000000000LL;
            pthread_mutex_unlock(&g_state->state_lock);

            log_action("DEADLOCK (Solar/Lunar): P%d vs P%d — choose resolution (GUI).",
                       idx_solar + 1, idx_lunar + 1);

            sem_post(&g_state->player_turn_sem[idx_solar]);

            struct timespec ts_dl;
            clock_gettime(CLOCK_REALTIME, &ts_dl);
            ts_dl.tv_sec += 60;
            int r;
            do
            {
                r = sem_timedwait(&g_state->deadlock_choice_sem, &ts_dl);
            } while (r == -1 && errno == EINTR);

            pthread_mutex_lock(&g_state->state_lock);
            int choice = g_state->deadlock_user_choice;
            if (choice < 1 || choice > 2)
                choice = 1;
            g_state->deadlock_modal_pending = false;
            g_state->deadlock_prompt_player = -1;
            g_state->deadlock_user_choice = 0;
            g_state->gui_prompt_mode = 0;
            g_state->gui_prompt_max = 0;
            g_state->gui_prompt_player = -1;
            g_state->gui_prompt_text[0] = '\0';
            g_state->gui_pressed_key = -1;
            g_state->gui_action_label[0] = '\0';
            pthread_mutex_unlock(&g_state->state_lock);

            // Drop stray SFML posts (double key / overlap with weapon-drop teardown).
            while (sem_trywait(&g_state->gui_input_sem) == 0)
                ;

            pthread_mutex_lock(&g_state->state_lock);
            pthread_mutex_lock(&g_state->resource_table.table_lock);
            apply_interactive_solar_lunar_resolution(choice, idx_solar, idx_lunar);
            pthread_mutex_unlock(&g_state->resource_table.table_lock);
            pthread_mutex_unlock(&g_state->state_lock);

            pthread_mutex_lock(&g_state->state_lock);
            g_state->resource_hud_resolve_ttl = 105;
            g_state->resource_hud_last_victim_artifact =
                deadlock ? victim_artifact : (int)ArtifactID::SolarCore;
            pthread_mutex_unlock(&g_state->state_lock);
            continue;
        }

        if (deadlock)
        {
            log_action("DEADLOCK detected! Forcing artifact release...");

            // Map artifact ID to weapon type
            WeaponType w = WeaponType::None;
            switch ((ArtifactID)victim_artifact)
            {
            case ArtifactID::SolarCore:
                w = WeaponType::SolarCore;
                break;
            case ArtifactID::LunarBlade:
                w = WeaponType::LunarBlade;
                break;
            case ArtifactID::EclipseRelic:
                w = WeaponType::EclipseRelic;
                break;
            default:
                break;
            }

            ArtifactEntry &ea = rt.entries[victim_artifact];
            int vh = ea.holder_index;
            bool vp = ea.held_by_player;
            ea.is_free = true;
            ea.held_by_player = false;
            ea.holder_index = -1;
            ea.has_waiter = false;
            ea.waiter_is_player = false;
            ea.waiter_index = -1;

            if (other_artifact >= 0 && other_artifact < (int)ArtifactID::COUNT)
            {
                ArtifactEntry &ob = rt.entries[other_artifact];
                ob.has_waiter = false;
                ob.waiter_is_player = false;
                ob.waiter_index = -1;
            }

            if (vp && vh >= 0)
            {
                PlayerCharacter &p = g_state->players[vh];
                for (int s = 0; s < INVENTORY_SLOTS; s++)
                {
                    if (p.inventory[s].weapon == w)
                    {
                        p.inventory[s].weapon = WeaponType::None;
                        p.inventory[s].is_start = false;
                    }
                }
                log_action("Deadlock resolved: P%d released %s.", vh + 1, WEAPON_NAMES[(int)w]);
            }
            else if (!vp && vh >= 0)
            {
                g_state->enemies[vh].held_weapon = WeaponType::None;
                log_action("Deadlock resolved: E%d released %s.", vh + 1, WEAPON_NAMES[(int)w]);
            }
        }

        pthread_mutex_unlock(&g_state->resource_table.table_lock);

        if (deadlock)
        {
            pthread_mutex_lock(&g_state->state_lock);
            g_state->resource_hud_resolve_ttl       = 105;
            g_state->resource_hud_last_victim_artifact = victim_artifact;
            pthread_mutex_unlock(&g_state->state_lock);
        }
    }
    return nullptr;
}

// ─────────────────────────────────────────────
//  RENDERING THREAD 
// ─────────────────────────────────────────────
static const sf::Color COL_BG = sf::Color(20, 20, 30);
static const sf::Color COL_PANEL = sf::Color(30, 30, 50, 220);
static const sf::Color COL_PANEL_DARK = sf::Color(15, 15, 25, 240);
static const sf::Color COL_BORDER = sf::Color(100, 100, 150);
static const sf::Color COL_HP_FULL = sf::Color(50, 200, 50);
static const sf::Color COL_HP_LOW = sf::Color(220, 50, 50);
static const sf::Color COL_STM = sf::Color(50, 150, 220);
static const sf::Color COL_TEXT = sf::Color(230, 230, 210);
static const sf::Color COL_GOLD = sf::Color(255, 215, 0);
static const sf::Color COL_RED = sf::Color(220, 60, 60);
static const sf::Color COL_GREEN = sf::Color(60, 200, 60);
static const sf::Color COL_ACTIVE = sf::Color(255, 255, 100);
static const sf::Color COL_STUN = sf::Color(180, 100, 255);

// Helper: draw a rounded panel
void draw_panel(sf::RenderWindow &win, float x, float y, float w, float h,
                sf::Color fill = sf::Color(30, 30, 50, 220),
                sf::Color border = sf::Color(100, 100, 150))
{
    sf::RectangleShape bg({w, h});
    bg.setPosition(x, y);
    bg.setFillColor(fill);
    bg.setOutlineColor(border);
    bg.setOutlineThickness(2.f);
    win.draw(bg);
}

static inline float ease_in_out_cubic(float t)
{
    if (t < 0.5f) return 4.f * t * t * t;
    float f = (-2.f * t + 2.f);
    return 1.f - f * f * f / 2.f;
}

// ease_out_back: overshoots slightly then settles (impactful feel)
static inline float ease_out_back(float t)
{
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.f;
    float p = t - 1.f;
    return 1.f + c3 * p * p * p + c1 * p * p;
}

// Helper: draw a bar (HP or Stamina)
void draw_bar(sf::RenderWindow &win, float x, float y, float w, float h,
              float ratio, sf::Color col)
{
    // Background
    sf::RectangleShape bg({w, h});
    bg.setPosition(x, y);
    bg.setFillColor(sf::Color(40, 40, 40));
    bg.setOutlineColor(sf::Color(80, 80, 80));
    bg.setOutlineThickness(1.f);
    win.draw(bg);
    // Fill
    if (ratio > 0.f)
    {
        sf::RectangleShape fill({w * ratio, h});
        fill.setPosition(x, y);
        fill.setFillColor(col);
        win.draw(fill);
    }
}

// ─────────────────────────────────────────────
// draw_weapon_icon — Section 6 visualisation

// ─────────────────────────────────────────────
static void draw_weapon_icon(sf::RenderWindow &win, float x, float y,
                             float size, WeaponType w)
{
    float cx = x + size * 0.5f;
    float cy = y + size * 0.5f;
    float r  = size * 0.42f;

    switch (w)
    {
    case WeaponType::SolarCore: {
        // Bright sun: golden disc with 4 outer rays
        sf::CircleShape disc(r);
        disc.setOrigin(r, r);
        disc.setPosition(cx, cy);
        disc.setFillColor(sf::Color(255, 200, 60));
        disc.setOutlineColor(sf::Color(255, 240, 140));
        disc.setOutlineThickness(1.f);
        win.draw(disc);
        for (int i = 0; i < 4; i++) {
            float a = (float)i * 1.5707963f;
            sf::RectangleShape ray({size * 0.20f, 1.5f});
            ray.setOrigin(0.f, 0.75f);
            ray.setPosition(cx + cosf(a) * r, cy + sinf(a) * r);
            ray.setRotation(a * 57.2958f);
            ray.setFillColor(sf::Color(255, 220, 100));
            win.draw(ray);
        }
        break;
    }
    case WeaponType::LunarBlade: {
        // Silver crescent moon
        sf::CircleShape outer(r);
        outer.setOrigin(r, r);
        outer.setPosition(cx, cy);
        outer.setFillColor(sf::Color(220, 230, 240));
        win.draw(outer);
        sf::CircleShape inner(r * 0.85f);
        inner.setOrigin(r * 0.85f, r * 0.85f);
        inner.setPosition(cx + size * 0.18f, cy);
        inner.setFillColor(sf::Color(30, 30, 50, 240));
        win.draw(inner);
        break;
    }
    case WeaponType::IronHalberd: {
        // Long grey shaft + triangular blade head
        sf::RectangleShape shaft({size * 0.10f, size * 0.80f});
        shaft.setOrigin(size * 0.05f, size * 0.40f);
        shaft.setPosition(cx, cy + size * 0.05f);
        shaft.setFillColor(sf::Color(160, 160, 170));
        win.draw(shaft);
        sf::ConvexShape head;
        head.setPointCount(3);
        head.setPoint(0, sf::Vector2f(cx,                  y + size * 0.08f));
        head.setPoint(1, sf::Vector2f(cx + size * 0.30f,   y + size * 0.32f));
        head.setPoint(2, sf::Vector2f(cx - size * 0.30f,   y + size * 0.32f));
        head.setFillColor(sf::Color(200, 200, 210));
        head.setOutlineColor(sf::Color(120, 120, 130));
        head.setOutlineThickness(1.f);
        win.draw(head);
        break;
    }
    case WeaponType::VenomDagger: {
        // Short green blade with hilt
        sf::ConvexShape blade;
        blade.setPointCount(4);
        blade.setPoint(0, sf::Vector2f(cx,                y + size * 0.10f));
        blade.setPoint(1, sf::Vector2f(cx + size * 0.14f, y + size * 0.55f));
        blade.setPoint(2, sf::Vector2f(cx,                y + size * 0.65f));
        blade.setPoint(3, sf::Vector2f(cx - size * 0.14f, y + size * 0.55f));
        blade.setFillColor(sf::Color(80, 200, 90));
        blade.setOutlineColor(sf::Color(40, 120, 50));
        blade.setOutlineThickness(1.f);
        win.draw(blade);
        sf::RectangleShape hilt({size * 0.30f, size * 0.08f});
        hilt.setOrigin(size * 0.15f, size * 0.04f);
        hilt.setPosition(cx, y + size * 0.62f);
        hilt.setFillColor(sf::Color(120, 80, 40));
        win.draw(hilt);
        break;
    }
    case WeaponType::Thunderstaff: {
        // Yellow zig-zag bolt
        sf::ConvexShape bolt;
        bolt.setPointCount(7);
        bolt.setPoint(0, sf::Vector2f(cx + size * 0.10f, y + size * 0.05f));
        bolt.setPoint(1, sf::Vector2f(cx - size * 0.20f, y + size * 0.45f));
        bolt.setPoint(2, sf::Vector2f(cx - size * 0.02f, y + size * 0.45f));
        bolt.setPoint(3, sf::Vector2f(cx - size * 0.18f, y + size * 0.85f));
        bolt.setPoint(4, sf::Vector2f(cx + size * 0.20f, y + size * 0.40f));
        bolt.setPoint(5, sf::Vector2f(cx + size * 0.02f, y + size * 0.40f));
        bolt.setPoint(6, sf::Vector2f(cx + size * 0.20f, y + size * 0.05f));
        bolt.setFillColor(sf::Color(250, 220, 60));
        bolt.setOutlineColor(sf::Color(180, 120, 0));
        bolt.setOutlineThickness(1.f);
        win.draw(bolt);
        break;
    }
    case WeaponType::ObsidianAxe: {
        // Dark axe: shaft + curved head
        sf::RectangleShape shaft({size * 0.10f, size * 0.70f});
        shaft.setOrigin(size * 0.05f, size * 0.35f);
        shaft.setPosition(cx, cy + size * 0.05f);
        shaft.setFillColor(sf::Color(110, 80, 50));
        win.draw(shaft);
        sf::ConvexShape head;
        head.setPointCount(4);
        head.setPoint(0, sf::Vector2f(cx + size * 0.05f, y + size * 0.15f));
        head.setPoint(1, sf::Vector2f(cx + size * 0.40f, y + size * 0.25f));
        head.setPoint(2, sf::Vector2f(cx + size * 0.40f, y + size * 0.55f));
        head.setPoint(3, sf::Vector2f(cx + size * 0.05f, y + size * 0.45f));
        head.setFillColor(sf::Color(40, 40, 60));
        head.setOutlineColor(sf::Color(120, 120, 160));
        head.setOutlineThickness(1.f);
        win.draw(head);
        break;
    }
    case WeaponType::Frostbow: {
        // Cyan curved bow + arrow
        sf::CircleShape arc(r * 0.95f);
        arc.setOrigin(r * 0.95f, r * 0.95f);
        arc.setPosition(cx + size * 0.18f, cy);
        arc.setFillColor(sf::Color(80, 200, 220, 220));
        arc.setOutlineColor(sf::Color(140, 220, 240));
        arc.setOutlineThickness(1.f);
        win.draw(arc);
        sf::CircleShape mask(r * 0.80f);
        mask.setOrigin(r * 0.80f, r * 0.80f);
        mask.setPosition(cx + size * 0.30f, cy);
        mask.setFillColor(sf::Color(30, 30, 50, 255));
        win.draw(mask);
        sf::RectangleShape arrow({size * 0.55f, 1.5f});
        arrow.setOrigin(0.f, 0.75f);
        arrow.setPosition(x + size * 0.10f, cy);
        arrow.setFillColor(sf::Color(220, 240, 255));
        win.draw(arrow);
        break;
    }
    case WeaponType::SplinterStick: {
        // Tiny brown stick
        sf::RectangleShape stick({size * 0.15f, size * 0.70f});
        stick.setOrigin(size * 0.075f, size * 0.35f);
        stick.setPosition(cx, cy);
        stick.setRotation(35.f);
        stick.setFillColor(sf::Color(160, 110, 60));
        stick.setOutlineColor(sf::Color(80, 50, 25));
        stick.setOutlineThickness(1.f);
        win.draw(stick);
        break;
    }
    case WeaponType::EclipseRelic: {
        // Purple ringed orb
        sf::CircleShape ring(r);
        ring.setOrigin(r, r);
        ring.setPosition(cx, cy);
        ring.setFillColor(sf::Color(60, 20, 80));
        ring.setOutlineColor(sf::Color(220, 120, 240));
        ring.setOutlineThickness(2.f);
        win.draw(ring);
        sf::CircleShape core(r * 0.45f);
        core.setOrigin(r * 0.45f, r * 0.45f);
        core.setPosition(cx, cy);
        core.setFillColor(sf::Color(220, 140, 250));
        win.draw(core);
        break;
    }
    default: {
        sf::RectangleShape q({size * 0.5f, size * 0.5f});
        q.setOrigin(size * 0.25f, size * 0.25f);
        q.setPosition(cx, cy);
        q.setFillColor(sf::Color(80, 80, 80));
        win.draw(q);
        break;
    }
    }
}

void *rendering_thread(void *)
{
    // ── Load font ──
    sf::Font font;
    // Try to load a system font; fallback gracefully
    bool font_loaded = font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    if (!font_loaded)
        font_loaded = font.loadFromFile("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");

    // ── Load sprites ──
    // Players: ryu, lee, mike, retsu
    sf::Texture player_tex[MAX_PLAYERS];
    sf::Sprite player_spr[MAX_PLAYERS];
    const char *player_files[MAX_PLAYERS] = {
        "assets/players/ryu_idle.png",
        "assets/players/lee_idle.png",
        "assets/players/mike_idle.png",
        "assets/players/retsu_idle.png"};
    sf::Texture player_atk_tex[MAX_PLAYERS];
    sf::Sprite player_atk_spr[MAX_PLAYERS];
    const char *player_atk_files[MAX_PLAYERS] = {
        "assets/players/ryu_attack.png",
        "assets/players/lee_attack.png",
        "assets/players/mike_attack.png",
        "assets/players/retsu_attack.png"};
    sf::Texture player_dead_tex[MAX_PLAYERS];
    sf::Sprite player_dead_spr[MAX_PLAYERS];
    const char *player_dead_files[MAX_PLAYERS] = {
        "assets/players/ryu_dead.png",
        "assets/players/lee_dead.png",
        "assets/players/mike_dead.png",
        "assets/players/retsu_dead.png"};

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        player_tex[i].loadFromFile(player_files[i]);
        player_spr[i].setTexture(player_tex[i]);
        player_spr[i].setScale(1.f, 1.f);

        player_atk_tex[i].loadFromFile(player_atk_files[i]);
        player_atk_spr[i].setTexture(player_atk_tex[i]);
        player_atk_spr[i].setScale(1.f, 1.f);

        player_dead_tex[i].loadFromFile(player_dead_files[i]);
        player_dead_spr[i].setTexture(player_dead_tex[i]);
        player_dead_spr[i].setScale(1.f, 1.f);
    }

    // Enemies: 3 types cycling
    sf::Texture enemy_tex[3];
    sf::Sprite enemy_spr[3];
    const char *enemy_files[3] = {
        "assets/enemies/imp_idle.png",
        "assets/enemies/andromalius_idle.png",
        "assets/enemies/mage_idle.png"};
    sf::Texture enemy_atk_tex[3];
    sf::Sprite enemy_atk_spr[3];
    const char *enemy_atk_files[3] = {
        "assets/enemies/imp_attack.png",
        "assets/enemies/andromalius_attack.png",
        "assets/enemies/mage_attack.png"};
    sf::Texture enemy_dead_tex[3];
    sf::Sprite enemy_dead_spr[3];
    const char *enemy_dead_files[3] = {
        "assets/enemies/imp_dead.png",
        "assets/enemies/andromalius_dead.png",
        "assets/enemies/mage_dead.png"};
    for (int i = 0; i < 3; i++)
    {
        enemy_tex[i].loadFromFile(enemy_files[i]);
        enemy_spr[i].setTexture(enemy_tex[i]);
        enemy_spr[i].setScale(2.5f, 2.5f);

        enemy_atk_tex[i].loadFromFile(enemy_atk_files[i]);
        enemy_atk_spr[i].setTexture(enemy_atk_tex[i]);
        enemy_atk_spr[i].setScale(2.5f, 2.5f);

        enemy_dead_tex[i].loadFromFile(enemy_dead_files[i]);
        enemy_dead_spr[i].setTexture(enemy_dead_tex[i]);
        enemy_dead_spr[i].setScale(2.5f, 2.5f);
    }
    sf::Texture bg_tex;
    bg_tex.loadFromFile("assets/background/battle_bg.png");
    bg_tex.setSmooth(true);
    sf::IntRect bg_crop(10, 18, 820, 132); // (x, y, w, h) — strips watermarks
    sf::Sprite bg_spr(bg_tex, bg_crop);
    // Stretch cropped scene over full window width and battle-area height
    // (battle area is top 55% of window).
    bg_spr.setScale((float)WIN_W / (float)bg_crop.width,
                    (float)(WIN_H * 0.58f) / (float)bg_crop.height);

    // Subtle dark vignette overlays for the battle area edges
    sf::RectangleShape vignette_top({(float)WIN_W, 18.f});
    vignette_top.setFillColor(sf::Color(0, 0, 0, 110));
    vignette_top.setPosition(0.f, 0.f);
    sf::RectangleShape vignette_bot({(float)WIN_W, 22.f});
    vignette_bot.setFillColor(sf::Color(0, 0, 0, 140));
    vignette_bot.setPosition(0.f, WIN_H * 0.58f - 22.f);

    // ── Create window ──
    sf::RenderWindow window(sf::VideoMode(WIN_W, WIN_H),
                            "Chrono Rift",
                            sf::Style::Titlebar | sf::Style::Close);
    window.setFramerateLimit(30);

    // Layout constants
    const float BATTLE_H = WIN_H * 0.58f; // top 58% = battle area
    const float UI_Y = BATTLE_H;          // bottom 42% = UI panels
    const float UI_H = WIN_H - UI_Y;
    const float PANEL_W = WIN_W / 2.f; // left = menu, right = stats
    const float MENU_X = 0.f;
    const float STATS_X = PANEL_W;

    // Action flash timer 
    int flash_player = -1;
    int flash_enemy = -1;
    int flash_timer = 0;

    // ── Section 3 visualisation: interpolated stamina display ──

    float disp_stm_p[MAX_PLAYERS] = {0};
    float disp_stm_e[MAX_ENEMIES] = {0};
    int   ready_pulse_t = 0;       // shared pulsing clock for READY badges
    int   committed_flash_p[MAX_PLAYERS] = {0};
    int   committed_flash_e[MAX_ENEMIES] = {0};
    bool  was_ready_p[MAX_PLAYERS] = {false, false, false, false};
    bool  was_ready_e[MAX_ENEMIES] = {false, false, false, false, false, false, false, false, false};

    // ── Section 5 visualisation: stun applied / recovered transient flashes ──
    bool  was_stunned_p[MAX_PLAYERS] = {false, false, false, false};
    bool  was_stunned_e[MAX_ENEMIES] = {false, false, false, false, false, false, false, false, false};
    int   stun_applied_flash_p[MAX_PLAYERS] = {0};
    int   stun_applied_flash_e[MAX_ENEMIES] = {0};
    int   stun_recover_flash_p[MAX_PLAYERS] = {0};
    int   stun_recover_flash_e[MAX_ENEMIES] = {0};

    // Window must have keyboard focus to receive input
    window.requestFocus();

    while (window.isOpen())
    {
        // ── Event handling ──
        sf::Event event;
        while (window.pollEvent(event))
        {
            if (event.type == sf::Event::Closed)
            {
                window.close();
                g_quit_requested = 1;
            }
            else if (event.type == sf::Event::KeyPressed)
            {
                int digit = -1;
                switch (event.key.code)
                {
                case sf::Keyboard::Num0: case sf::Keyboard::Numpad0: digit = 0; break;
                case sf::Keyboard::Num1: case sf::Keyboard::Numpad1: digit = 1; break;
                case sf::Keyboard::Num2: case sf::Keyboard::Numpad2: digit = 2; break;
                case sf::Keyboard::Num3: case sf::Keyboard::Numpad3: digit = 3; break;
                case sf::Keyboard::Num4: case sf::Keyboard::Numpad4: digit = 4; break;
                case sf::Keyboard::Num5: case sf::Keyboard::Numpad5: digit = 5; break;
                case sf::Keyboard::Num6: case sf::Keyboard::Numpad6: digit = 6; break;
                case sf::Keyboard::Num7: case sf::Keyboard::Numpad7: digit = 7; break;
                case sf::Keyboard::Num8: case sf::Keyboard::Numpad8: digit = 8; break;
                case sf::Keyboard::Num9: case sf::Keyboard::Numpad9: digit = 9; break;
                case sf::Keyboard::Escape:
                    window.close();
                    g_quit_requested = 1;
                    break;
                default: break;
                }
                if (digit >= 0 && g_state)
                {
                    pthread_mutex_lock(&g_state->state_lock);
                    bool waiting = (g_state->gui_prompt_mode == 1);
                    int max_d = g_state->gui_prompt_max;
                    bool dmodal = g_state->deadlock_modal_pending;
                    int d_for = g_state->deadlock_prompt_player;
                    int gui_for = g_state->gui_prompt_player;
                    bool drop_p = g_state->weapon_drop_pending;
                    int d_ch = g_state->deadlock_user_choice;
                    bool dlock_keys = dmodal && !drop_p && (d_ch == 0) && (d_for >= 0) &&
                                      (max_d == 2) &&
                                      (digit == 1 || digit == 2) && (gui_for == d_for);
                    bool take = false;
                    if (dlock_keys)
                        take = true;
                    else if (waiting && digit <= max_d)
                    {
                        // While Solar/Lunar deadlock is up, another player may still own
                        
                        if (dmodal && !drop_p && max_d > 2 && gui_for >= 0 && d_for >= 0 &&
                            gui_for != d_for)
                            take = false;
                        else if (!dmodal || gui_for != d_for || drop_p || max_d > 2)
                            take = true;
                    }
                    if (take)
                    {
                        g_state->gui_pressed_key = digit;
                        g_state->gui_prompt_mode = 0; // consumed
                        pthread_mutex_unlock(&g_state->state_lock);
                        sem_post(&g_state->gui_input_sem);
                    }
                    else
                    {
                        pthread_mutex_unlock(&g_state->state_lock);
                    }
                }
            }
        }

        if (!g_state)
        {
            window.display();
            continue;
        }

        // ══ Section 4 — synchronized per-frame snapshot ═════════════════
        // The renderer must not race with scheduler / HIP / ASP writes.
       
        SharedState snap_state;
        pthread_mutex_lock(&g_state->state_lock);

        // Animation lifecycle 
        if (g_state->anim_frames_left > 0)
            g_state->anim_frames_left--;
        else if (g_state->anim_kind != 0)
        {
            g_state->anim_kind = 0;
            g_state->anim_attacker = -1;
            g_state->anim_target = -1;
        }

        
        if (g_state->inv_anim_place_ttl  > 0) g_state->inv_anim_place_ttl--;
        if (g_state->inv_anim_evict_ttl  > 0) g_state->inv_anim_evict_ttl--;
        if (g_state->inv_anim_swapin_ttl > 0) g_state->inv_anim_swapin_ttl--;

        if (g_state->resource_hud_resolve_ttl > 0)
            g_state->resource_hud_resolve_ttl--;


        long long t_now_render = now_ns();
        for (int i = 0; i < g_state->player_count; i++)
        {
            PlayerCharacter &p = g_state->players[i];
            if (p.stunned && (t_now_render - p.stun_apply_ns >= CHRONO_STUN_DURATION_NS))
                p.stunned = false;
        }
        for (int i = 0; i < g_state->enemy_count; i++)
        {
            EnemyCharacter &e = g_state->enemies[i];
            if (e.stunned && (t_now_render - e.stun_apply_ns >= CHRONO_STUN_DURATION_NS))
                e.stunned = false;
        }

        memcpy(&snap_state, g_state, sizeof(SharedState));
        pthread_mutex_unlock(&g_state->state_lock);
        SharedState *s = &snap_state;

        {
            static TurnOwner s_last_owner = TurnOwner::Player;
            static int s_last_entity = -999;
            if (s->turn.owner != s_last_owner || s->turn.entity_index != s_last_entity)
            {
                for (int i = 0; i < s->player_count; i++)
                {
                    PlayerCharacter &pp = s->players[i];
                    was_ready_p[i] = pp.alive && pp.ready_to_act && !pp.stunned;
                }
                for (int i = 0; i < s->enemy_count; i++)
                {
                    EnemyCharacter &ee = s->enemies[i];
                    was_ready_e[i] = ee.alive && ee.ready_to_act && !ee.stunned &&
                                     s->phase != GamePhase::Ultimate;
                }
                s_last_owner = s->turn.owner;
                s_last_entity = s->turn.entity_index;
            }
        }

        ArtifactEntry art_hud[(int)ArtifactID::COUNT];
        if (!s->pvp_mode)
        {
            pthread_mutex_lock(&g_state->resource_table.table_lock);
            for (int ai = 0; ai < (int)ArtifactID::COUNT; ai++)
                art_hud[ai] = g_state->resource_table.entries[ai];
            pthread_mutex_unlock(&g_state->resource_table.table_lock);
        }
        else
        {
            memset(art_hud, 0, sizeof(art_hud));
        }

        // ── Section 3 visualisation: smooth stamina interpolation ──
        // Reads come from the synchronized snapshot above (Section 4).
        ready_pulse_t = (ready_pulse_t + 1) % 600;
        {
            const float alpha_lerp = 0.18f;
            for (int i = 0; i < s->player_count; i++)
            {
                PlayerCharacter &pp = s->players[i];
                float target = pp.alive ? pp.stamina : 0.f;
                disp_stm_p[i] += (target - disp_stm_p[i]) * alpha_lerp;
                bool now_ready = pp.alive && pp.ready_to_act && !pp.stunned;
                if (was_ready_p[i] && !now_ready && pp.alive)
                    committed_flash_p[i] = 18;
                was_ready_p[i] = now_ready;
                if (committed_flash_p[i] > 0) committed_flash_p[i]--;

                // Section 5 — stun applied / recovered transitions
                if (pp.stunned && !was_stunned_p[i])
                    stun_applied_flash_p[i] = 22;     // ~0.7s purple burst
                if (!pp.stunned && was_stunned_p[i] && pp.alive)
                    stun_recover_flash_p[i] = 22;     // ~0.7s green-cyan recover pulse
                was_stunned_p[i] = pp.stunned;
                if (stun_applied_flash_p[i] > 0) stun_applied_flash_p[i]--;
                if (stun_recover_flash_p[i] > 0) stun_recover_flash_p[i]--;
            }
            for (int i = 0; i < s->enemy_count; i++)
            {
                EnemyCharacter &ee = s->enemies[i];
                float target = ee.alive ? ee.stamina : 0.f;
                disp_stm_e[i] += (target - disp_stm_e[i]) * alpha_lerp;
                bool now_ready = ee.alive && ee.ready_to_act && !ee.stunned;
                if (was_ready_e[i] && !now_ready && ee.alive)
                    committed_flash_e[i] = 18;
                was_ready_e[i] = now_ready;
                if (committed_flash_e[i] > 0) committed_flash_e[i]--;

                // Section 5 — stun applied / recovered transitions
                if (ee.stunned && !was_stunned_e[i])
                    stun_applied_flash_e[i] = 22;
                if (!ee.stunned && was_stunned_e[i] && ee.alive)
                    stun_recover_flash_e[i] = 22;
                was_stunned_e[i] = ee.stunned;
                if (stun_applied_flash_e[i] > 0) stun_applied_flash_e[i]--;
                if (stun_recover_flash_e[i] > 0) stun_recover_flash_e[i]--;
            }
        }

        // Track action flash (legacy support) — Section 4: read from snapshot
        if (s->turn.action_submitted)
        {
            if (s->turn.owner == TurnOwner::Player)
                flash_player = s->turn.entity_index;
            else
                flash_enemy = s->turn.entity_index;
            flash_timer = 8;
        }
        if (flash_timer > 0)
            flash_timer--;
        else
        {
            flash_player = -1;
            flash_enemy = -1;
        }

        // Animation-driven flash override
        if (s->anim_frames_left > 0)
        {
            if (s->anim_attacker_is_player)
                flash_player = s->anim_attacker;
            else
                flash_enemy = s->anim_attacker;
        }

        window.clear(s->pvp_mode ? sf::Color(50, 20, 70) : COL_BG);

        // ══ BATTLE AREA ════════════════════════════════════════════

        if (!s->pvp_mode)
        {
            // Single mode: cropped battle scene + vignettes
            window.draw(bg_spr);
            window.draw(vignette_top);
            window.draw(vignette_bot);
        }
        else
        {
            // Bonus PvP 
            sf::RectangleShape pvp_base({(float)WIN_W, BATTLE_H});
            pvp_base.setFillColor(sf::Color(35, 8, 55));
            window.draw(pvp_base);
        }

        // Ultimate tint (single mode) or persistent purple duel tint (bonus PvP)
        if (s->pvp_mode || s->phase == GamePhase::Ultimate)
        {
            sf::RectangleShape overlay({(float)WIN_W, BATTLE_H});
            overlay.setFillColor(sf::Color(100, 0, 150, 80));
            window.draw(overlay);
        }

        bool target_select_mode = false;
        {
            if (s->gui_prompt_mode == 1 &&
                (strstr(s->gui_prompt_text, "Target") ||
                 strstr(s->gui_prompt_text, "target")))
                target_select_mode = true;
        }

        // ── Draw Enemies (top of battle area) ── dynamic scaling
        int ec = s->enemy_count;
        float enemy_area_w = WIN_W * 0.92f;
        float enemy_area_x = WIN_W * 0.04f;
        float enemy_y = BATTLE_H * 0.05f;
        float enemy_slot_w = (ec > 0) ? enemy_area_w / (float)ec : enemy_area_w;
        float enemy_centers_x[MAX_ENEMIES] = {0};
        float enemy_centers_y[MAX_ENEMIES] = {0};

        for (int i = 0; i < ec; i++)
        {
            EnemyCharacter &e = s->enemies[i];
            int etype = i % 3;

            sf::Sprite *spr;
            if (!e.alive)
                spr = &enemy_dead_spr[etype];
            else if (flash_enemy == i && flash_timer > 0)
                spr = &enemy_atk_spr[etype];
            else
                spr = &enemy_spr[etype];

            // Dynamic scale: prevent overlap — fit within slot with gap
            float tex_w = (float)spr->getTexture()->getSize().x;
            float tex_h = (float)spr->getTexture()->getSize().y;
            float escale = (enemy_slot_w - 14.f) / tex_w;
            if (escale > 1.8f) escale = 1.8f;
            if (escale < 0.6f) escale = 0.6f;
            spr->setScale(escale, escale);

            // Smooth slide-attack animation for enemy attackers (downward lunge)
            float dy_offset = 0.f;
            float scale_boost = 1.f;
            if (s->anim_kind != 0 && !s->anim_attacker_is_player &&
                s->anim_attacker == i && s->anim_frames_left > 0)
            {
                float t = (30.f - (float)s->anim_frames_left) / 30.f; // 0..1
                if (t < 0.5f)
                {
                    float p = ease_in_out_cubic(t * 2.f);
                    dy_offset = p * 50.f;          // approach (down 50px)
                    scale_boost = 1.f + p * 0.10f; // slight grow
                }
                else
                {
                    float p = ease_in_out_cubic((1.f - t) * 2.f);
                    dy_offset = p * 50.f;
                    scale_boost = 1.f + p * 0.10f;
                }
            }
            // Hit recoil for enemy targets (smooth shake)
            float dx_shake = 0.f;
            int impact_frames = 18;
            if (s->anim_kind != 0 && !s->anim_target_is_player &&
                s->anim_target == i && s->anim_frames_left > impact_frames)
            {
                int kf = s->anim_frames_left - impact_frames;
                dx_shake = (kf % 2 ? -4.f : 4.f) * (float)kf / 12.f;
            }

            spr->setScale(escale * scale_boost, escale * scale_boost);
            // Section 5 viz: small horizontal jitter while stunned for "shaky" feel.
            float stun_jitter = 0.f;
            if (e.stunned)
                stun_jitter = (sinf((float)ready_pulse_t * 1.6f + (float)i) * 1.6f);
            float ex = enemy_area_x + (i * enemy_slot_w) +
                       (enemy_slot_w - tex_w * escale * scale_boost) * 0.5f + dx_shake + stun_jitter;
            float ey = enemy_y + 16 + dy_offset;
            spr->setPosition(ex, ey);
            if (e.stunned)
            {
                // Pulse between purple stun tint and a brighter "applied" flash
                // for the first ~0.7s of the stun.
                if (stun_applied_flash_e[i] > 0)
                {
                    int b = 80 + stun_applied_flash_e[i] * 6;
                    if (b > 255) b = 255;
                    spr->setColor(sf::Color((sf::Uint8)b, (sf::Uint8)(b/2), 255));
                }
                else
                {
                    spr->setColor(COL_STUN);
                }
            }
            else if (stun_recover_flash_e[i] > 0)
            {
                // Brief green recovery glow when stun expires
                int a = 100 + stun_recover_flash_e[i] * 7;
                if (a > 255) a = 255;
                spr->setColor(sf::Color((sf::Uint8)(255 - a/2), 255, (sf::Uint8)(255 - a/3)));
            }
            else if (s->anim_target == i && !s->anim_target_is_player &&
                     s->anim_frames_left > impact_frames)
            {
                int kf = s->anim_frames_left - impact_frames;
                int flash = 200 + (kf * 5);
                if (flash > 255) flash = 255;
                spr->setColor(sf::Color(255, (sf::Uint8)flash, (sf::Uint8)flash));
            }
            else
                spr->setColor(sf::Color::White);
            window.draw(*spr);
            // restore base scale for next frame
            spr->setScale(escale, escale);

            float sprite_h = tex_h * escale;
            enemy_centers_x[i] = ex + tex_w * escale * 0.5f;
            enemy_centers_y[i] = ey + sprite_h * 0.5f;

            if (!font_loaded)
                continue;

            sf::Text name_txt("E" + std::to_string(i + 1), font, 11);
            name_txt.setFillColor(COL_GOLD);
            name_txt.setPosition(ex, enemy_y);
            window.draw(name_txt);

            float bar_w = tex_w * escale;
            if (bar_w > enemy_slot_w - 8.f) bar_w = enemy_slot_w - 8.f;
            if (bar_w < 40.f) bar_w = 40.f;
            float bar_x = ex;
            float bar_y = enemy_y + 16 + sprite_h + 4.f;
            float hp_ratio = e.alive ? (float)e.hp / e.max_hp : 0.f;
            sf::Color hp_col = hp_ratio > 0.4f ? COL_HP_FULL : COL_HP_LOW;
            draw_bar(window, bar_x, bar_y, bar_w, 6.f, hp_ratio, hp_col);

            
            float stm_ratio = e.alive ? disp_stm_e[i] / MAX_STAMINA_ENEMY : 0.f;
            if (stm_ratio < 0.f) stm_ratio = 0.f;
            if (stm_ratio > 1.f) stm_ratio = 1.f;
            sf::Color stm_col = COL_STM;
            bool e_ready = e.alive && e.ready_to_act && !e.stunned;
            if (e_ready)
            {
                float pulse = 0.5f + 0.5f * sinf((float)ready_pulse_t * 0.25f);
                stm_col = sf::Color(
                    (sf::Uint8)(200 + (int)(55.f * pulse)),
                    (sf::Uint8)(170 + (int)(60.f * pulse)),
                    (sf::Uint8)(40),
                    255);
            }
            else if (committed_flash_e[i] > 0)
            {
                int a = 200 + committed_flash_e[i] * 3;
                if (a > 255) a = 255;
                stm_col = sf::Color((sf::Uint8)a, (sf::Uint8)(a / 2), (sf::Uint8)(a / 2), 255);
            }
            draw_bar(window, bar_x, bar_y + 8.f, bar_w, 4.f, stm_ratio, stm_col);

            sf::Text hp_txt(std::to_string(e.hp) + "/" + std::to_string(e.max_hp), font, 9);
            hp_txt.setFillColor(COL_TEXT);
            hp_txt.setPosition(bar_x, bar_y + 14.f);
            window.draw(hp_txt);

            if (e.stunned)
            {
                // Section 5 viz: live wall-clock countdown of the 3-second stun.
                long long t_now_e = now_ns();
                float remain_s = (float)(CHRONO_STUN_DURATION_NS - (t_now_e - e.stun_apply_ns)) / 1e9f;
                if (remain_s < 0.f) remain_s = 0.f;
                if (remain_s > 3.f) remain_s = 3.f;
                float ratio = remain_s / 3.f;

                // Pulsing label "STUN 2.7s"
                int pulse = 200 + (int)(55.f * sinf((float)ready_pulse_t * 0.45f));
                if (pulse > 255) pulse = 255;
                if (pulse < 140) pulse = 140;
                char buf_st[24];
                snprintf(buf_st, sizeof(buf_st), "STUN %.1fs", remain_s);
                sf::Text stun_txt(buf_st, font, 10);
                stun_txt.setStyle(sf::Text::Bold);
                stun_txt.setFillColor(sf::Color(190, 110, 255, (sf::Uint8)pulse));
                stun_txt.setOutlineColor(sf::Color(20, 0, 40, (sf::Uint8)pulse));
                stun_txt.setOutlineThickness(1.f);
                stun_txt.setPosition(ex, bar_y + 26.f);
                window.draw(stun_txt);

                // Skinny purple countdown bar that drains over exactly 3 seconds.
                draw_bar(window, bar_x, bar_y + 38.f, bar_w, 3.f, ratio, COL_STUN);
            }
            else if (stun_recover_flash_e[i] > 0)
            {
                // Recovery badge fades out over ~0.7s after stun ends.
                int alpha = stun_recover_flash_e[i] * 11;
                if (alpha > 255) alpha = 255;
                sf::Text rec("RECOVERED", font, 9);
                rec.setStyle(sf::Text::Bold);
                rec.setFillColor(sf::Color(120, 255, 180, (sf::Uint8)alpha));
                rec.setOutlineColor(sf::Color(0, 60, 30, (sf::Uint8)alpha));
                rec.setOutlineThickness(1.f);
                rec.setPosition(ex, bar_y + 26.f);
                window.draw(rec);
            }
            else if (e_ready)
            {
                // "READY" badge — visible cue for Section 3 first-to-fill scheduling
                int alpha = 200 + (int)(55.f * sinf((float)ready_pulse_t * 0.25f));
                if (alpha > 255) alpha = 255;
                if (alpha < 120) alpha = 120;
                sf::Text rb("READY", font, 9);
                rb.setStyle(sf::Text::Bold);
                rb.setFillColor(sf::Color(255, 220, 60, (sf::Uint8)alpha));
                rb.setOutlineColor(sf::Color(0, 0, 0, (sf::Uint8)alpha));
                rb.setOutlineThickness(1.f);
                rb.setPosition(ex, bar_y + 26.f);
                window.draw(rb);
            }

            // Target-select mode: draw big yellow target number
            if (target_select_mode && e.alive)
            {
                sf::CircleShape circ(16.f);
                circ.setFillColor(sf::Color(255, 220, 60, 220));
                circ.setOutlineColor(sf::Color::Black);
                circ.setOutlineThickness(2.f);
                circ.setPosition(ex + tex_w * escale * 0.5f - 16.f, ey - 36.f);
                window.draw(circ);

                sf::Text num(std::to_string(i + 1), font, 18);
                num.setStyle(sf::Text::Bold);
                num.setFillColor(sf::Color::Black);
                float nx = ex + tex_w * escale * 0.5f - (i + 1 < 10 ? 6.f : 12.f);
                num.setPosition(nx, ey - 36.f);
                window.draw(num);
            }
        }

        // ── Draw Players (lower-left of battle area) ── compact scaling
        int pc = s->player_count;
        // Players occupy lower-left area, smaller and tighter (Chrono Trigger feel)
        float player_area_w = WIN_W * 0.45f;
        float player_area_x = WIN_W * 0.04f;
        float player_y = BATTLE_H * 0.65f;
        float player_slot_w = (pc > 0) ? player_area_w / (float)pc : player_area_w;
        float player_centers_x[MAX_PLAYERS] = {0};
        float player_centers_y[MAX_PLAYERS] = {0};

        for (int i = 0; i < pc; i++)
        {
            PlayerCharacter &p = s->players[i];

            sf::Sprite *spr;
            if (!p.alive)
                spr = &player_dead_spr[i];
            else if (flash_player == i && flash_timer > 0)
                spr = &player_atk_spr[i];
            else
                spr = &player_spr[i];

            float tex_w = (float)spr->getTexture()->getSize().x;
            float tex_h = (float)spr->getTexture()->getSize().y;

            // Players are much smaller now — clean Chrono-Trigger silhouettes
            float pscale = (player_slot_w - 16.f) / tex_w;
            if (pscale > 0.65f) pscale = 0.65f;
            if (pscale < 0.40f) pscale = 0.40f;

            // Smooth slide-attack with easing + scale-up for impact
            float dy_offset = 0.f;
            float scale_boost = 1.f;
            if (s->anim_kind != 0 && s->anim_attacker_is_player &&
                s->anim_attacker == i && s->anim_frames_left > 0)
            {
                float t = (30.f - (float)s->anim_frames_left) / 30.f; // 0..1
                if (t < 0.5f)
                {
                    float p_ease = ease_in_out_cubic(t * 2.f);
                    dy_offset = -p_ease * 60.f;       // approach (up 60px)
                    scale_boost = 1.f + p_ease * 0.18f;
                }
                else
                {
                    float p_ease = ease_in_out_cubic((1.f - t) * 2.f);
                    dy_offset = -p_ease * 60.f;
                    scale_boost = 1.f + p_ease * 0.18f;
                }
            }

            // Smooth hit recoil
            float dx_shake = 0.f;
            int impact_frames = 18;
            if (s->anim_kind != 0 && s->anim_target_is_player &&
                s->anim_target == i && s->anim_frames_left > impact_frames)
            {
                int kf = s->anim_frames_left - impact_frames;
                dx_shake = (kf % 2 ? -4.f : 4.f) * (float)kf / 12.f;
            }

            float final_scale = pscale * scale_boost;
            spr->setScale(final_scale, final_scale);

            // Section 5 viz: stun-induced jitter so the player visibly looks "frozen-shaking".
            float p_stun_jitter = 0.f;
            if (p.stunned)
                p_stun_jitter = (sinf((float)ready_pulse_t * 1.6f + (float)(i + 0.5f)) * 1.6f);
            float px = player_area_x + (i * player_slot_w) +
                       (player_slot_w - tex_w * final_scale) * 0.5f + dx_shake + p_stun_jitter;
            float py = player_y + dy_offset;
            spr->setPosition(px, py);
            if (p.stunned)
            {
                if (stun_applied_flash_p[i] > 0)
                {
                    int b = 80 + stun_applied_flash_p[i] * 6;
                    if (b > 255) b = 255;
                    spr->setColor(sf::Color((sf::Uint8)b, (sf::Uint8)(b/2), 255));
                }
                else
                {
                    spr->setColor(COL_STUN);
                }
            }
            else if (stun_recover_flash_p[i] > 0)
            {
                int a = 100 + stun_recover_flash_p[i] * 7;
                if (a > 255) a = 255;
                spr->setColor(sf::Color((sf::Uint8)(255 - a/2), 255, (sf::Uint8)(255 - a/3)));
            }
            else if (s->anim_target_is_player && s->anim_target == i &&
                     s->anim_frames_left > impact_frames)
            {
                int kf = s->anim_frames_left - impact_frames;
                int flash = 200 + (kf * 5);
                if (flash > 255) flash = 255;
                spr->setColor(sf::Color(255, (sf::Uint8)flash, (sf::Uint8)flash));
            }
            else
                spr->setColor(sf::Color::White);
            window.draw(*spr);

            // Section 5 viz: under-sprite stun countdown bar so the player can
            // see exactly how long is left in real time (3.0s → 0.0s).
            if (p.stunned)
            {
                long long t_now_p = now_ns();
                float remain_s = (float)(CHRONO_STUN_DURATION_NS - (t_now_p - p.stun_apply_ns)) / 1e9f;
                if (remain_s < 0.f) remain_s = 0.f;
                if (remain_s > 3.f) remain_s = 3.f;
                float pratio = remain_s / 3.f;
                float pbar_w = tex_w * final_scale;
                if (pbar_w < 50.f) pbar_w = 50.f;
                float pbar_x = px;
                float pbar_y = py + tex_h * final_scale + 2.f;
                draw_bar(window, pbar_x, pbar_y, pbar_w, 4.f, pratio, COL_STUN);
                int pp_pulse = 200 + (int)(55.f * sinf((float)ready_pulse_t * 0.45f));
                if (pp_pulse > 255) pp_pulse = 255;
                if (pp_pulse < 140) pp_pulse = 140;
                char pbuf[24];
                snprintf(pbuf, sizeof(pbuf), "STUN %.1fs", remain_s);
                sf::Text pst(pbuf, font, 10);
                pst.setStyle(sf::Text::Bold);
                pst.setFillColor(sf::Color(190, 110, 255, (sf::Uint8)pp_pulse));
                pst.setOutlineColor(sf::Color(20, 0, 40, (sf::Uint8)pp_pulse));
                pst.setOutlineThickness(1.f);
                pst.setPosition(pbar_x, pbar_y + 6.f);
                window.draw(pst);
            }
            else if (stun_recover_flash_p[i] > 0)
            {
                int alpha = stun_recover_flash_p[i] * 11;
                if (alpha > 255) alpha = 255;
                sf::Text rec("RECOVERED", font, 9);
                rec.setStyle(sf::Text::Bold);
                rec.setFillColor(sf::Color(120, 255, 180, (sf::Uint8)alpha));
                rec.setOutlineColor(sf::Color(0, 60, 30, (sf::Uint8)alpha));
                rec.setOutlineThickness(1.f);
                rec.setPosition(px, py + tex_h * final_scale + 2.f);
                window.draw(rec);
            }

            // Heal glow ring for healing animation
            if (s->anim_kind == 4 && s->anim_target == i &&
                s->anim_target_is_player && s->anim_frames_left > 0)
            {
                float t = (30.f - (float)s->anim_frames_left) / 30.f;
                float radius = 18.f + t * 22.f;
                int alpha = (int)(180.f * (1.f - t));
                sf::CircleShape glow(radius);
                glow.setFillColor(sf::Color(0, 0, 0, 0));
                glow.setOutlineColor(sf::Color(120, 255, 120, (sf::Uint8)alpha));
                glow.setOutlineThickness(3.f);
                glow.setOrigin(radius, radius);
                glow.setPosition(px + tex_w * final_scale * 0.5f,
                                 py + tex_h * final_scale * 0.5f);
                window.draw(glow);
            }

            spr->setScale(pscale, pscale); // restore for next frame

            player_centers_x[i] = px + tex_w * final_scale * 0.5f;
            player_centers_y[i] = py + tex_h * final_scale * 0.5f;

            // Active-turn indicator: smooth pulsing yellow brackets (Chrono Trigger)
            if (s->turn.owner == TurnOwner::Player &&
                s->turn.entity_index == i && p.alive)
            {
                static int pulse_t = 0;
                pulse_t = (pulse_t + 1) % 60;
                int pulse_alpha = 180 + (int)(50.f * sinf((float)pulse_t * 0.2f));
                sf::Color hl(255, 255, 100, (sf::Uint8)pulse_alpha);

                float bw = tex_w * final_scale;
                float bh = tex_h * final_scale;
                // L-shaped corner brackets
                float bl = 6.f;
                sf::RectangleShape c1({bl, 2.f}); c1.setFillColor(hl);
                c1.setPosition(px - 2, py - 2); window.draw(c1);
                sf::RectangleShape c2({2.f, bl}); c2.setFillColor(hl);
                c2.setPosition(px - 2, py - 2); window.draw(c2);
                sf::RectangleShape c3({bl, 2.f}); c3.setFillColor(hl);
                c3.setPosition(px + bw - bl + 2, py - 2); window.draw(c3);
                sf::RectangleShape c4({2.f, bl}); c4.setFillColor(hl);
                c4.setPosition(px + bw, py - 2); window.draw(c4);
                sf::RectangleShape c5({bl, 2.f}); c5.setFillColor(hl);
                c5.setPosition(px - 2, py + bh); window.draw(c5);
                sf::RectangleShape c6({2.f, bl}); c6.setFillColor(hl);
                c6.setPosition(px - 2, py + bh - bl + 2); window.draw(c6);
                sf::RectangleShape c7({bl, 2.f}); c7.setFillColor(hl);
                c7.setPosition(px + bw - bl + 2, py + bh); window.draw(c7);
                sf::RectangleShape c8({2.f, bl}); c8.setFillColor(hl);
                c8.setPosition(px + bw, py + bh - bl + 2); window.draw(c8);

                if (font_loaded)
                {
                    sf::Text mark("P" + std::to_string(i + 1), font, 11);
                    mark.setStyle(sf::Text::Bold);
                    mark.setFillColor(COL_ACTIVE);
                    mark.setOutlineColor(sf::Color::Black);
                    mark.setOutlineThickness(1.f);
                    mark.setPosition(px, py - 16.f);
                    window.draw(mark);
                }
            }
        }

        // ── Damage / heal popup over target ──
        if (font_loaded && s->anim_kind != 0 && s->anim_frames_left > 0 &&
            s->anim_damage > 0)
        {
            float cx = 0, cy = 0;
            bool valid = false;
            if (s->anim_target_is_player &&
                s->anim_target >= 0 && s->anim_target < pc)
            {
                cx = player_centers_x[s->anim_target];
                cy = player_centers_y[s->anim_target];
                valid = true;
            }
            else if (!s->anim_target_is_player &&
                     s->anim_target >= 0 && s->anim_target < ec)
            {
                cx = enemy_centers_x[s->anim_target];
                cy = enemy_centers_y[s->anim_target];
                valid = true;
            }

            if (valid)
            {
                float t = (30.f - (float)s->anim_frames_left) / 30.f; // 0..1
                // Eased upward float — fast initial, slows
                float ease = 1.f - (1.f - t) * (1.f - t);
                float fy = cy - 28.f - ease * 50.f;
                // Fade out in last third
                int alpha = 255;
                if (t > 0.66f) alpha = (int)(255.f * (1.f - (t - 0.66f) / 0.34f));
                if (alpha < 0) alpha = 0;

                sf::Color col = (s->anim_kind == 4) ? COL_GREEN : COL_RED;
                col.a = (sf::Uint8)alpha;

                // Pop-in scale: starts small, grows to full
                float popscale = 0.6f + (t < 0.2f ? t * 2.f : 1.f) * 0.4f;
                if (popscale > 1.f) popscale = 1.f;

                sf::Text dmg_txt(std::to_string(s->anim_damage), font, 26);
                dmg_txt.setStyle(sf::Text::Bold);
                dmg_txt.setFillColor(col);
                dmg_txt.setOutlineColor(sf::Color(0, 0, 0, alpha));
                dmg_txt.setOutlineThickness(2.f);
                dmg_txt.setScale(popscale, popscale);
                // Center-anchor the text
                sf::FloatRect lb = dmg_txt.getLocalBounds();
                dmg_txt.setOrigin(lb.width * 0.5f, lb.height * 0.5f);
                dmg_txt.setPosition(cx, fy);
                window.draw(dmg_txt);
            }
        }

        // ── Top message box (prompt OR last action log) ──
        bool has_prompt = (s->gui_prompt_mode == 1 &&
                           s->gui_prompt_text[0] != '\0');
        draw_panel(window, 5.f, 5.f, WIN_W - 10.f, 32.f,
                   has_prompt ? sf::Color(60, 30, 30, 240)
                              : sf::Color(20, 20, 40, 230),
                   has_prompt ? COL_GOLD : COL_BORDER);
        if (font_loaded)
        {
            if (has_prompt)
            {
                sf::Text msg(s->gui_prompt_text, font, 14);
                msg.setStyle(sf::Text::Bold);
                msg.setFillColor(COL_GOLD);
                msg.setPosition(12.f, 11.f);
                window.draw(msg);
            }
            else if (s->log.count > 0)
            {
                int last = ((s->log.head - 1) + ACTION_LOG_LINES) % ACTION_LOG_LINES;
                sf::Text msg(s->log.lines[last], font, 13);
                msg.setFillColor(COL_TEXT);
                msg.setPosition(12.f, 12.f);
                window.draw(msg);
            }
        }

        // ══ UI PANELS (bottom 45%) ════════════════════════════════

        // Left panel: Action menu (for active player)
        draw_panel(window, MENU_X, UI_Y, PANEL_W, UI_H,
                   COL_PANEL_DARK, COL_BORDER);

        if (font_loaded)
        {
            // Tighter line spacing leaves room for artifact table under actions
            const float act_dy     =
                17.f; // fits artifact table + quit in left panel
            const float act_y0     = UI_Y + 30.f;
            const float art_main_h = 70.f;

            // Single-player: always list 8. Ultimate (key 8; arbiter rejects if not ready).
            // PvP: no Ultimate row — options stop at 7. Skip.
            int n_act_rows = 7;
            if (s->turn.owner == TurnOwner::Player && !s->pvp_mode)
                n_act_rows = 8;

            // Title
            std::string menu_title = "ACTION";
            if (s->turn.owner == TurnOwner::Player)
            {
                menu_title = "P" + std::to_string(s->turn.entity_index + 1) + " ACTION";
            }
            else
            {
                menu_title = "ENEMY TURN...";
            }
            sf::Text menu_hdr(menu_title, font, 13);
            menu_hdr.setFillColor(COL_GOLD);
            menu_hdr.setPosition(MENU_X + 10.f, UI_Y + 8.f);
            window.draw(menu_hdr);

            // Actions list — numbered for SFML keyboard input
            const char *actions[] = {
                "1. Strike", "2. Exhaust", "3. Use Weapon",
                "4. Swap In", "5. Move to Long Term", "6. Heal", "7. Skip",
                "8. Ultimate"};

            // Are we waiting for an "action" (1–8) choice?
            bool action_select_mode = (s->gui_prompt_mode == 1 &&
                                       strstr(s->gui_prompt_text, "Action"));

            for (int i = 0; i < n_act_rows; i++)
            {
                float ay = act_y0 + i * act_dy;
                sf::Text act(actions[i], font, 13);

                // Currently chosen action (label persists during animation)
                bool is_chosen = (s->gui_action_label[0] != '\0' &&
                                  strstr(actions[i], s->gui_action_label));

                if (action_select_mode)
                {
                    // Pulsing yellow box around all keys to show "press a number"
                    sf::RectangleShape box({PANEL_W - 24.f, 20.f});
                    box.setPosition(MENU_X + 12.f, ay - 2.f);
                    box.setFillColor(sf::Color(80, 60, 20, 120));
                    box.setOutlineColor(sf::Color(180, 140, 40, 200));
                    box.setOutlineThickness(1.f);
                    window.draw(box);
                    act.setFillColor(COL_GOLD);
                }
                else if (is_chosen)
                {
                    sf::RectangleShape box({PANEL_W - 24.f, 20.f});
                    box.setPosition(MENU_X + 12.f, ay - 2.f);
                    box.setFillColor(sf::Color(60, 100, 60, 200));
                    window.draw(box);
                    act.setFillColor(COL_ACTIVE);
                    act.setStyle(sf::Text::Bold);
                }
                else
                {
                    act.setFillColor(COL_TEXT);
                }
                act.setPosition(MENU_X + 22.f, ay);
                window.draw(act);
            }

            // ── Section 7: Resource table under action list (left panel) ──
            if (!s->pvp_mode) {
                const float art_hw     = PANEL_W - 24.f;
                const float art_hx0    = MENU_X + 12.f;
                const float art_y0     = act_y0 + (float)n_act_rows * act_dy + 5.f;
                float       hy        = art_y0 + 14.f;
                float pulse =
                    0.5f + 0.5f * sinf((float)ready_pulse_t * 0.11f);

                sf::RectangleShape hud_bg({art_hw, art_main_h});
                hud_bg.setPosition(art_hx0, art_y0);
                hud_bg.setFillColor(sf::Color(16, 28, 42, 238));
                hud_bg.setOutlineColor(sf::Color(
                    (sf::Uint8)(72 + (int)(40.f * pulse)),
                    (sf::Uint8)(168 + (int)(50.f * pulse)),
                    (sf::Uint8)(188 + (int)(35.f * pulse)),
                    220));
                hud_bg.setOutlineThickness(1.f);
                window.draw(hud_bg);

                sf::Text ht("ARTIFACTS (global)", font, 9);
                ht.setStyle(sf::Text::Bold);
                ht.setFillColor(sf::Color(245, 220, 150));
                ht.setOutlineColor(sf::Color(40, 30, 20, 180));
                ht.setOutlineThickness(1.f);
                ht.setPosition(art_hx0 + 6.f, art_y0 + 2.f);
                window.draw(ht);

                static const char *const art_short[(int)ArtifactID::COUNT] = {
                    "Solar Core", "Lunar Blade", "Eclipse"};

                for (int ai = 0; ai < (int)ArtifactID::COUNT; ai++)
                {
                    ArtifactEntry &ae = art_hud[ai];
                    char line[112];
                    if (!ae.exists)
                        snprintf(line, sizeof(line), "%s  — inactive —",
                                 art_short[ai]);
                    else if (ae.is_free)
                        snprintf(line, sizeof(line), "%s  available",
                                 art_short[ai]);
                    else
                    {
                        const char *who = ae.held_by_player ? "P" : "E";
                        snprintf(line, sizeof(line), "%s  %s%d",
                                 art_short[ai], who, ae.holder_index + 1);
                    }
                    sf::Text row(line, font, 8);
                    float row_pulse =
                        ae.has_waiter
                            ? (0.55f +
                               0.45f * sinf((float)ready_pulse_t * 0.19f +
                                            (float)ai * 0.65f))
                            : 1.f;
                    if (!ae.exists)
                        row.setFillColor(sf::Color(110, 120, 140));
                    else if (ae.is_free)
                        row.setFillColor(sf::Color(130, 230, 185));
                    else if (ae.has_waiter)
                        row.setFillColor(sf::Color(
                            (sf::Uint8)(255 * row_pulse),
                            (sf::Uint8)(155 + 50 * row_pulse),
                            (sf::Uint8)(105 + 40 * row_pulse)));
                    else
                        row.setFillColor(sf::Color(210, 225, 250));
                    row.setPosition(art_hx0 + 6.f, hy);
                    window.draw(row);
                    hy += 12.f;
                    if (ae.has_waiter && ae.waiter_index >= 0)
                    {
                        char wtxt[56];
                        snprintf(wtxt, sizeof(wtxt), "   blocked by wait %s%d",
                                 ae.waiter_is_player ? "P" : "E",
                                 ae.waiter_index + 1);
                        sf::Text wt(wtxt, font, 7);
                        wt.setFillColor(sf::Color(
                            255, 200, 140,
                            (sf::Uint8)(175 + (int)(55.f * row_pulse))));
                        wt.setPosition(art_hx0 + 6.f, hy);
                        window.draw(wt);
                        hy += 10.f;
                    }
                }

                if (s->resource_hud_resolve_ttl > 0)
                {
                    float t = (float)s->resource_hud_resolve_ttl / 105.f;
                    int alpha = (int)(55 + 200.f * t);
                    if (alpha > 255) alpha = 255;
                    WeaponType vw = WeaponType::None;
                    int vid       = s->resource_hud_last_victim_artifact;
                    if (vid == (int)ArtifactID::SolarCore)
                        vw = WeaponType::SolarCore;
                    else if (vid == (int)ArtifactID::LunarBlade)
                        vw = WeaponType::LunarBlade;
                    else if (vid == (int)ArtifactID::EclipseRelic)
                        vw = WeaponType::EclipseRelic;
                    char banner[96];
                    if (vw != WeaponType::None)
                        snprintf(banner, sizeof(banner), "Deadlock cleared: %s",
                                 WEAPON_NAMES[(int)vw]);
                    else
                        snprintf(banner, sizeof(banner), "Deadlock cleared");
                    float by = art_y0 + art_main_h + 3.f;
                    sf::RectangleShape bbg({art_hw, 22.f});
                    bbg.setPosition(art_hx0, by - 1.f);
                    bbg.setFillColor(sf::Color(22, 48, 38, (sf::Uint8)(alpha * 0.85f)));
                    bbg.setOutlineColor(
                        sf::Color(64, 220, 140, (sf::Uint8)alpha));
                    bbg.setOutlineThickness(1.f + pulse * 0.5f);
                    window.draw(bbg);
                    sf::Text ban(banner, font, 8);
                    ban.setStyle(sf::Text::Bold);
                    ban.setFillColor(sf::Color(150, 255, 195, (sf::Uint8)alpha));
                    ban.setOutlineColor(sf::Color(0, 40, 24, (sf::Uint8)alpha));
                    ban.setOutlineThickness(1.f);
                    ban.setPosition(art_hx0 + 6.f, by + 2.f);
                    window.draw(ban);
                }
            }

            // Quit entry (below artifact block)
            {
                float ay_quit =
                    act_y0 + (float)n_act_rows * act_dy + 5.f + art_main_h + 6.f +
                    (s->resource_hud_resolve_ttl > 0 ? 26.f : 0.f);
                sf::Text quit_txt("0. Quit", font, 13);
                quit_txt.setFillColor(sf::Color(230, 120, 130));
                quit_txt.setOutlineColor(sf::Color(60, 25, 30, 140));
                quit_txt.setOutlineThickness(1.f);
                quit_txt.setPosition(MENU_X + 22.f, ay_quit);
                window.draw(quit_txt);
            }

            // Ultimate availability indicator
            if (s->turn.owner == TurnOwner::Player)
            {
                int pi = s->turn.entity_index;
                if (pi < s->player_count)
                {
                    bool can_ult = player_has_ultimate(s->players[pi]);
                    sf::Text ult_txt(can_ult ? "[ULTIMATE READY]"
                                           : "[Need Solar+Lunar]",
                                     font, 10);
                    ult_txt.setFillColor(
                        can_ult ? sf::Color(255, 215, 120)
                                : sf::Color(130, 135, 150));
                    ult_txt.setOutlineColor(sf::Color(20, 18, 12, 120));
                    ult_txt.setOutlineThickness(1.f);
                    ult_txt.setPosition(MENU_X + 10.f, UI_Y + UI_H - 18.f);
                    window.draw(ult_txt);
                }
            }
        }

        // Right panel: Party stats (HP, Stamina bars)
        draw_panel(window, STATS_X, UI_Y, PANEL_W, UI_H,
                   COL_PANEL, COL_BORDER);

        if (font_loaded)
        {
            // Column headers
            sf::Text hdr("     NAME      HP              STM", font, 11);
            hdr.setFillColor(COL_GOLD);
            hdr.setPosition(STATS_X + 8.f, UI_Y + 6.f);
            window.draw(hdr);

            // HP/MP header labels (FF6 style)
            sf::Text hp_hdr("HP", font, 11);
            hp_hdr.setFillColor(COL_GOLD);
            hp_hdr.setPosition(STATS_X + 160.f, UI_Y + 6.f);
            window.draw(hp_hdr);

            sf::Text stm_hdr("STM", font, 11);
            stm_hdr.setFillColor(COL_GOLD);
            stm_hdr.setPosition(STATS_X + 280.f, UI_Y + 6.f);
            window.draw(stm_hdr);

            float row_y = UI_Y + 26.f;
            const char *pnames[4] = {"Ryu", "Lee", "Mike", "Retsu"};

            for (int i = 0; i < s->player_count; i++)
            {
                PlayerCharacter &p = s->players[i];
                float ry = row_y + i * 38.f;

                // Active turn highlight
                if (s->turn.owner == TurnOwner::Player &&
                    s->turn.entity_index == i)
                {
                    sf::RectangleShape hl({PANEL_W - 4.f, 36.f});
                    hl.setPosition(STATS_X + 2.f, ry - 2.f);
                    hl.setFillColor(sf::Color(60, 60, 100, 100));
                    window.draw(hl);

                    // Arrow cursor (FF6 style)
                    sf::Text arrow(">", font, 13);
                    arrow.setFillColor(COL_ACTIVE);
                    arrow.setPosition(STATS_X + 4.f, ry + 2.f);
                    window.draw(arrow);
                }

                // Name
                sf::Text name(pnames[i], font, 13);
                name.setFillColor(p.alive ? COL_TEXT : sf::Color(100, 100, 100));
                name.setPosition(STATS_X + 18.f, ry + 2.f);
                window.draw(name);

                // HP value (FF6: "435:46" style — we use hp/maxhp)
                std::string hp_str = std::to_string(p.hp) + ":" + std::to_string(p.max_hp);
                sf::Text hp_val(hp_str, font, 12);
                float hp_ratio = p.max_hp > 0 ? (float)p.hp / p.max_hp : 0.f;
                hp_val.setFillColor(hp_ratio > 0.3f ? COL_TEXT : COL_RED);
                hp_val.setPosition(STATS_X + 80.f, ry + 2.f);
                window.draw(hp_val);

                // HP bar
                draw_bar(window, STATS_X + 80.f, ry + 18.f, 100.f, 7.f,
                         hp_ratio,
                         hp_ratio > 0.4f ? COL_HP_FULL : COL_HP_LOW);

                // Stamina bar (right side) — Section 3 visualisation:
                
                float stm_ratio = disp_stm_p[i] / MAX_STAMINA_PLAYER;
                if (stm_ratio < 0.f) stm_ratio = 0.f;
                if (stm_ratio > 1.f) stm_ratio = 1.f;
                sf::Color stm_col = COL_STM;
                bool p_ready = p.alive && p.ready_to_act && !p.stunned;
                if (p_ready)
                {
                    float pulse = 0.5f + 0.5f * sinf((float)ready_pulse_t * 0.25f);
                    stm_col = sf::Color(
                        (sf::Uint8)(200 + (int)(55.f * pulse)),
                        (sf::Uint8)(170 + (int)(60.f * pulse)),
                        (sf::Uint8)(40),
                        255);
                }
                else if (committed_flash_p[i] > 0)
                {
                    int a = 200 + committed_flash_p[i] * 3;
                    if (a > 255) a = 255;
                    stm_col = sf::Color((sf::Uint8)a, (sf::Uint8)(a / 2), (sf::Uint8)(a / 2), 255);
                }
                draw_bar(window, STATS_X + 190.f, ry + 18.f, 90.f, 7.f,
                         stm_ratio, stm_col);

                // Show interpolated displayed value rather than raw — matches the bar
                sf::Text stm_val(std::to_string((int)disp_stm_p[i]) + "/100", font, 10);
                stm_val.setFillColor(p_ready ? COL_GOLD : COL_TEXT);
                stm_val.setPosition(STATS_X + 190.f, ry + 2.f);
                window.draw(stm_val);

                // Section 5: roster stun indicator with live countdown
                if (p.stunned)
                {
                    long long t_now_pr = now_ns();
                    float remain_s = (float)(CHRONO_STUN_DURATION_NS - (t_now_pr - p.stun_apply_ns)) / 1e9f;
                    if (remain_s < 0.f) remain_s = 0.f;
                    if (remain_s > 3.f) remain_s = 3.f;
                    char buf_st[24];
                    snprintf(buf_st, sizeof(buf_st), "STUN %.1fs", remain_s);
                    int pulse = 200 + (int)(55.f * sinf((float)ready_pulse_t * 0.45f));
                    if (pulse > 255) pulse = 255;
                    if (pulse < 140) pulse = 140;
                    sf::Text st(buf_st, font, 10);
                    st.setStyle(sf::Text::Bold);
                    st.setFillColor(sf::Color(190, 110, 255, (sf::Uint8)pulse));
                    st.setOutlineColor(sf::Color(20, 0, 40, (sf::Uint8)pulse));
                    st.setOutlineThickness(1.f);
                    st.setPosition(STATS_X + 290.f, ry + 2.f);
                    window.draw(st);
                    // skinny countdown bar under the row
                    draw_bar(window, STATS_X + 290.f, ry + 14.f, 70.f, 3.f,
                             remain_s / 3.f, COL_STUN);
                }
                else if (stun_recover_flash_p[i] > 0)
                {
                    int alpha = stun_recover_flash_p[i] * 11;
                    if (alpha > 255) alpha = 255;
                    sf::Text rec("RECOVERED", font, 9);
                    rec.setStyle(sf::Text::Bold);
                    rec.setFillColor(sf::Color(120, 255, 180, (sf::Uint8)alpha));
                    rec.setOutlineColor(sf::Color(0, 60, 30, (sf::Uint8)alpha));
                    rec.setOutlineThickness(1.f);
                    rec.setPosition(STATS_X + 290.f, ry + 2.f);
                    window.draw(rec);
                }
                else if (p_ready)
                {
                    // Section 3: READY badge — entity scheduled to act
                    int alpha = 200 + (int)(55.f * sinf((float)ready_pulse_t * 0.25f));
                    if (alpha > 255) alpha = 255;
                    if (alpha < 120) alpha = 120;
                    sf::Text rb("READY", font, 10);
                    rb.setStyle(sf::Text::Bold);
                    rb.setFillColor(sf::Color(255, 220, 60, (sf::Uint8)alpha));
                    rb.setPosition(STATS_X + 290.f, ry + 2.f);
                    window.draw(rb);
                }
                // Dead indicator
                if (!p.alive)
                {
                    sf::Text dead("KO", font, 12);
                    dead.setFillColor(COL_RED);
                    dead.setPosition(STATS_X + 290.f, ry + 2.f);
                    window.draw(dead);
                }
            }

            // ── Section 6: Inventory + Long-term storage panels ──
            
            int active_p_for_inv;
            if (s->weapon_drop_pending &&
                s->drop_for_player >= 0 &&
                s->drop_for_player < s->player_count)
            {
                
                active_p_for_inv = s->drop_for_player;
            }
            else if (s->turn.owner == TurnOwner::Player &&
                     s->turn.entity_index >= 0 &&
                     s->turn.entity_index < s->player_count)
            {
                active_p_for_inv = s->turn.entity_index;
            }
            else
            {
                active_p_for_inv = 0;
            }

            float inv_y_start = UI_Y + 26.f + s->player_count * 38.f + 6.f;

            char inv_hdr[48];
            snprintf(inv_hdr, sizeof(inv_hdr),
                     "P%d INVENTORY (20 slots)   long-term: %d/%d",
                     active_p_for_inv + 1,
                     s->players[active_p_for_inv].long_term_count,
                     MAX_LONG_TERM);
            sf::Text inv_title(inv_hdr, font, 11);
            inv_title.setFillColor(COL_GOLD);
            inv_title.setPosition(STATS_X + 8.f, inv_y_start);
            window.draw(inv_title);

            // Inventory grid: 20 cells × (PANEL_W-20)/20 wide, 18 px tall
            float inv_strip_x = STATS_X + 8.f;
            float inv_strip_y = inv_y_start + 14.f;
            float inv_strip_w = PANEL_W - 16.f;
            float cell_w = inv_strip_w / (float)INVENTORY_SLOTS;
            float cell_h = 18.f;

            const PlayerCharacter &pp_inv = s->players[active_p_for_inv];

            // Background strip behind the cells (keeps visual grouping)
            sf::RectangleShape inv_bg({inv_strip_w + 2.f, cell_h + 2.f});
            inv_bg.setPosition(inv_strip_x - 1.f, inv_strip_y - 1.f);
            inv_bg.setFillColor(sf::Color(0, 0, 0, 60));
            inv_bg.setOutlineColor(COL_BORDER);
            inv_bg.setOutlineThickness(1.f);
            window.draw(inv_bg);

            for (int sl = 0; sl < INVENTORY_SLOTS; sl++)
            {
                const InventorySlot &is_slot = pp_inv.inventory[sl];

                bool has_w = (is_slot.weapon != WeaponType::None);
                float cx = inv_strip_x + sl * cell_w;

                // Determine cell colour from animation state.
                bool in_place_anim  = (s->inv_anim_player == active_p_for_inv &&
                                       s->inv_anim_place_ttl > 0 &&
                                       sl >= s->inv_anim_place_slot &&
                                       sl <  s->inv_anim_place_slot + s->inv_anim_place_size);
                bool in_swapin_anim = (s->inv_anim_player == active_p_for_inv &&
                                       s->inv_anim_swapin_ttl > 0 &&
                                       sl >= s->inv_anim_swapin_slot &&
                                       sl <  s->inv_anim_swapin_slot + s->inv_anim_swapin_size);
                bool in_evict_anim  = (s->inv_anim_player == active_p_for_inv &&
                                       s->inv_anim_evict_ttl > 0 &&
                                       sl >= s->inv_anim_evict_slot &&
                                       sl <  s->inv_anim_evict_slot + s->inv_anim_evict_size);

                sf::Color fill;
                if (in_evict_anim)
                {
                    int a = 80 + s->inv_anim_evict_ttl * 5;
                    if (a > 220) a = 220;
                    fill = sf::Color(220, 60, 60, (sf::Uint8)a);
                }
                else if (in_swapin_anim)
                {
                    int a = 80 + s->inv_anim_swapin_ttl * 4;
                    if (a > 220) a = 220;
                    fill = sf::Color(60, 200, 220, (sf::Uint8)a);
                }
                else if (in_place_anim)
                {
                    int a = 80 + s->inv_anim_place_ttl * 4;
                    if (a > 220) a = 220;
                    fill = sf::Color(220, 200, 60, (sf::Uint8)a);
                }
                else if (has_w)
                {
                    // body cells slightly dimmer than start cell
                    if (is_slot.is_start)
                        fill = sf::Color(60, 130, 80, 220);
                    else
                        fill = sf::Color(40, 90, 55, 180);
                }
                else
                {
                    fill = sf::Color(28, 28, 32, 180);
                }

                sf::RectangleShape cell({cell_w - 1.f, cell_h});
                cell.setPosition(cx, inv_strip_y);
                cell.setFillColor(fill);
                cell.setOutlineColor(sf::Color(80, 80, 80, 160));
                cell.setOutlineThickness(0.5f);
                window.draw(cell);

                // Self-made sprite icon on the start cell of each weapon.
                // Sized to fill the cell with a 1px inset.
                if (has_w && is_slot.is_start)
                {
                    float icon = std::min(cell_w - 2.f, cell_h - 2.f);
                    float icon_x = cx + 1.f;
                    float icon_y = inv_strip_y + 1.f;
                    draw_weapon_icon(window, icon_x, icon_y, icon,
                                     is_slot.weapon);
                }
            }

            // Long-term storage strip
            float lt_y = inv_strip_y + cell_h + 4.f;
            sf::Text lt_title("LONG-TERM:", font, 10);
            lt_title.setFillColor(COL_GOLD);
            lt_title.setPosition(STATS_X + 8.f, lt_y);
            window.draw(lt_title);

            float lt_x_cur = STATS_X + 80.f;
            for (int i = 0; i < pp_inv.long_term_count && i < MAX_LONG_TERM; i++)
            {
                if (!pp_inv.long_term[i].occupied) continue;
                WeaponType lw = pp_inv.long_term[i].weapon;
                std::string label = std::string(WEAPON_NAMES[(int)lw]) +
                                    "(" + std::to_string(WEAPON_SLOTS[(int)lw]) + ")";
                sf::Text lt_t(label, font, 10);
                lt_t.setFillColor(sf::Color(180, 200, 220));
                lt_t.setPosition(lt_x_cur, lt_y);
                sf::FloatRect bb = lt_t.getLocalBounds();
                window.draw(lt_t);
                lt_x_cur += bb.width + 10.f;
                if (lt_x_cur > STATS_X + PANEL_W - 20.f) break;
            }
            if (pp_inv.long_term_count == 0)
            {
                sf::Text lt_e("(empty)", font, 10);
                lt_e.setFillColor(sf::Color(120, 120, 120));
                lt_e.setPosition(lt_x_cur, lt_y);
                window.draw(lt_e);
            }

            // ── Action log (pushed below inventory + long-term) ──
            float log_y = lt_y + 16.f;
            if (log_y < UI_Y + UI_H - 20.f)
            {
                sf::RectangleShape log_div({PANEL_W - 10.f, 1.f});
                log_div.setPosition(STATS_X + 5.f, log_y);
                log_div.setFillColor(COL_BORDER);
                window.draw(log_div);

                int lines_avail = (int)((UI_Y + UI_H - log_y - 5.f) / 13.f);
                int count = std::min(lines_avail, s->log.count);
                for (int i = 0; i < count; i++)
                {
                    int idx = ((s->log.head - count + i) + ACTION_LOG_LINES) % ACTION_LOG_LINES;
                    sf::Text lt(s->log.lines[idx], font, 11);
                    lt.setFillColor(sf::Color(180, 180, 160));
                    lt.setPosition(STATS_X + 8.f, log_y + 4.f + i * 13.f);
                    window.draw(lt);
                }
            }

            // ── Kill counter ──
            sf::Text kills("Kills: " + std::to_string(s->total_kills) + "/" +
                               std::to_string(TOTAL_KILLS_TO_WIN),
                           font, 12);
            kills.setFillColor(COL_GOLD);
            kills.setPosition(STATS_X + 8.f, UI_Y + UI_H - 18.f);
            window.draw(kills);
        }

        // ── Section 7: Solar/Lunar PvP deadlock — choose who releases ──
        if (s->deadlock_modal_pending && font_loaded)
        {
            float box_w = 520.f;
            float box_h = 228.f;
            float box_x = (WIN_W - box_w) * 0.5f;
            float box_y = BATTLE_H * 0.12f;

            sf::RectangleShape dim({(float)WIN_W, BATTLE_H});
            dim.setPosition(0.f, 0.f);
            dim.setFillColor(sf::Color(0, 0, 0, 150));
            window.draw(dim);

            sf::RectangleShape box({box_w, box_h});
            box.setPosition(box_x, box_y);
            box.setFillColor(sf::Color(40, 25, 35, 245));
            box.setOutlineColor(COL_RED);
            box.setOutlineThickness(3.f);
            window.draw(box);

            sf::Text dtitle("DEADLOCK", font, 22);
            dtitle.setStyle(sf::Text::Bold);
            dtitle.setFillColor(COL_RED);
            sf::FloatRect dtb = dtitle.getLocalBounds();
            dtitle.setPosition(box_x + (box_w - dtb.width) * 0.5f, box_y + 10.f);
            window.draw(dtitle);

            sf::Text ln1(s->deadlock_line_main, font, 14);
            ln1.setFillColor(COL_TEXT);
            ln1.setPosition(box_x + 18.f, box_y + 46.f);
            window.draw(ln1);

            sf::Text ln2(s->deadlock_line_opt1, font, 12);
            ln2.setStyle(sf::Text::Bold);
            ln2.setFillColor(sf::Color(160, 230, 160));
            ln2.setPosition(box_x + 18.f, box_y + 78.f);
            window.draw(ln2);

            sf::Text ln3(s->deadlock_line_opt2, font, 12);
            ln3.setStyle(sf::Text::Bold);
            ln3.setFillColor(sf::Color(170, 200, 245));
            ln3.setPosition(box_x + 18.f, box_y + 100.f);
            window.draw(ln3);

            sf::Text hint("Press 1 or 2  (timeout defaults to option 1)", font, 11);
            hint.setFillColor(sf::Color(200, 200, 200, 200));
            hint.setPosition(box_x + 18.f, box_y + 128.f);
            window.draw(hint);

            long long t_now_d = now_ns();
            long long remain_d = s->deadlock_modal_deadline_ns - t_now_d;
            if (remain_d < 0) remain_d = 0;
            float rem_s = (float)remain_d / 1.0e9f;
            if (rem_s > 60.f) rem_s = 60.f;
            float prog_d = rem_s / 60.f;
            char tbuf[48];
            snprintf(tbuf, sizeof(tbuf), "%.0f s", rem_s);
            sf::Text tnumd(tbuf, font, 16);
            tnumd.setStyle(sf::Text::Bold);
            tnumd.setFillColor(rem_s > 15.f ? COL_GOLD : COL_RED);
            sf::FloatRect tnb = tnumd.getLocalBounds();
            tnumd.setPosition(box_x + box_w - tnb.width - 20.f, box_y + 124.f);
            window.draw(tnumd);

            float bar_x = box_x + 18.f;
            float bar_w = box_w - 36.f;
            float bar_y = box_y + box_h - 22.f;
            draw_bar(window, bar_x, bar_y, bar_w, 6.f, prog_d,
                     rem_s > 15.f ? sf::Color(200, 160, 60) : COL_RED);
        }

        // ── Section 6: Weapon-drop pickup prompt overlay ──
        // Only shown when the Arbiter has set weapon_drop_pending and the
        
        if (s->weapon_drop_pending && font_loaded && !s->deadlock_modal_pending &&
            s->gui_prompt_mode == 1 && strstr(s->gui_prompt_text, "Pick up"))
        {
            float box_w = 480.f;
            float box_h = 180.f;
            float box_x = (WIN_W - box_w) * 0.5f;
            float box_y = BATTLE_H * 0.20f;

            // Dimmed overlay across the battle area
            sf::RectangleShape dim({(float)WIN_W, BATTLE_H});
            dim.setPosition(0.f, 0.f);
            dim.setFillColor(sf::Color(0, 0, 0, 130));
            window.draw(dim);

            // Box
            sf::RectangleShape box({box_w, box_h});
            box.setPosition(box_x, box_y);
            box.setFillColor(sf::Color(30, 30, 50, 240));
            box.setOutlineColor(COL_GOLD);
            box.setOutlineThickness(2.f);
            window.draw(box);

            // Title
            sf::Text title("WEAPON DROP!", font, 22);
            title.setStyle(sf::Text::Bold);
            title.setFillColor(COL_GOLD);
            sf::FloatRect tb = title.getLocalBounds();
            title.setPosition(box_x + (box_w - tb.width) * 0.5f, box_y + 8.f);
            window.draw(title);

            // Weapon icon (self-made sprite)
            float icon_size = 56.f;
            draw_weapon_icon(window,
                             box_x + 18.f,
                             box_y + 44.f,
                             icon_size,
                             s->dropped_weapon);

            // Weapon name + size, to the right of the icon
            char nm[80];
            snprintf(nm, sizeof(nm), "%s  (uses %d slots)",
                     WEAPON_NAMES[(int)s->dropped_weapon],
                     WEAPON_SLOTS[(int)s->dropped_weapon]);
            sf::Text wn(nm, font, 16);
            wn.setStyle(sf::Text::Bold);
            wn.setFillColor(COL_TEXT);
            wn.setPosition(box_x + 18.f + icon_size + 14.f, box_y + 52.f);
            window.draw(wn);

            
            long long t_now = now_ns();
            long long remain_ns = s->weapon_drop_deadline_ns - t_now;
            if (remain_ns < 0) remain_ns = 0;
            float remain_s = (float)remain_ns / 1.0e9f;
            if (remain_s > 5.0f) remain_s = 5.0f;
            float prog = remain_s / 5.0f;

            char timebuf[16];
            snprintf(timebuf, sizeof(timebuf), "%.1fs", remain_s);
            sf::Text tnum(timebuf, font, 18);
            tnum.setStyle(sf::Text::Bold);
            // Bar+text turn red as time runs out
            sf::Color bar_col = (remain_s > 2.0f) ? sf::Color(220, 180, 60)
                              : (remain_s > 1.0f) ? sf::Color(230, 140, 40)
                                                  : sf::Color(230, 60, 60);
            tnum.setFillColor(bar_col);
            sf::FloatRect tnb = tnum.getLocalBounds();
            tnum.setPosition(box_x + box_w - tnb.width - 16.f,
                             box_y + 52.f);
            window.draw(tnum);

            
            int pulse = 200 + (int)(55.f * sinf((float)ready_pulse_t * 0.45f));
            if (pulse > 255) pulse = 255;
            if (pulse < 140) pulse = 140;

            sf::Text pick("Press 0 = PICK UP", font, 16);
            pick.setStyle(sf::Text::Bold);
            pick.setFillColor(sf::Color(120, 230, 120, (sf::Uint8)pulse));
            pick.setPosition(box_x + 30.f, box_y + 108.f);
            window.draw(pick);

            sf::Text leave("Press 1 = LEAVE FOR ENEMY", font, 14);
            leave.setStyle(sf::Text::Bold);
            leave.setFillColor(sf::Color(220, 110, 110, 230));
            sf::FloatRect lb = leave.getLocalBounds();
            leave.setPosition(box_x + box_w - lb.width - 30.f, box_y + 110.f);
            window.draw(leave);

            // Spec note
            sf::Text spec("(if you decline OR the timer hits 0, an enemy is GUARANTEED to take it)",
                          font, 10);
            spec.setFillColor(sf::Color(200, 200, 200, 180));
            sf::FloatRect sb = spec.getLocalBounds();
            spec.setPosition(box_x + (box_w - sb.width) * 0.5f, box_y + 134.f);
            window.draw(spec);

            // Exact draining countdown bar — width shrinks linearly with
            // remaining wall-clock time.
            float bar_x = box_x + 30.f;
            float bar_w = box_w - 60.f;
            float bar_y = box_y + box_h - 18.f;
            draw_bar(window, bar_x, bar_y, bar_w, 6.f, prog, bar_col);

            // Tick markers at each second so the player can see the rate.
            for (int t = 1; t < 5; t++)
            {
                float tx = bar_x + bar_w * (t / 5.0f);
                sf::RectangleShape tick({1.f, 6.f});
                tick.setPosition(tx, bar_y);
                tick.setFillColor(sf::Color(60, 60, 60, 200));
                window.draw(tick);
            }
        }

        // ── Game Over overlay ──
        if (s->phase == GamePhase::GameOver && font_loaded)
        {
            sf::RectangleShape overlay({(float)WIN_W, (float)WIN_H});
            overlay.setFillColor(sf::Color(0, 0, 0, 180));
            window.draw(overlay);

            std::string msg;
            sf::Color msg_col;
            if (s->result == GameResult::Win)
            {
                msg = "VICTORY!";
                msg_col = COL_GOLD;
            }
            else if (s->result == GameResult::Lose)
            {
                msg = "DEFEAT!";
                msg_col = COL_RED;
            }
            else
            {
                msg = "GAME QUIT";
                msg_col = COL_TEXT;
            }
            sf::Text go(msg, font, 48);
            go.setFillColor(msg_col);
            sf::FloatRect bounds = go.getLocalBounds();
            go.setPosition((WIN_W - bounds.width) / 2.f, (WIN_H - bounds.height) / 2.f);
            window.draw(go);
        }

        // ── Ultimate overlay ──
        if (s->phase == GamePhase::Ultimate && font_loaded)
        {
            sf::Text ult("!! ULTIMATE ACTIVE !!", font, 16);
            ult.setFillColor(COL_GOLD);
            ult.setPosition(WIN_W / 2.f - 100.f, BATTLE_H - 28.f);
            window.draw(ult);
        }

        window.display();
        sf::sleep(sf::milliseconds(33)); // ~30 fps
    }

    return nullptr;
}

// ─────────────────────────────────────────────
//  CLEANUP AND EXIT
// ─────────────────────────────────────────────
void cleanup_and_exit(GameResult result)
{
    std::cout << "\n=== GAME OVER ===\n";
    if (result == GameResult::Win)
        std::cout << "VICTORY!\n";
    if (result == GameResult::Lose)
        std::cout << "DEFEAT!\n";
    if (result == GameResult::Quit)
        std::cout << "QUIT.\n";

    // Signal children to terminate
    if (g_hip_pid > 0)
        kill(g_hip_pid, SIGTERM);
    if (g_asp_pid > 0)
        kill(g_asp_pid, SIGTERM);

    sleep(1); // Give children time to clean up

    // Destroy semaphores
    for (int i = 0; i < MAX_PLAYERS; i++)
        sem_destroy(&g_state->player_turn_sem[i]);
    for (int i = 0; i < MAX_ENEMIES; i++)
        sem_destroy(&g_state->enemy_turn_sem[i]);
    sem_destroy(&g_state->action_ready_sem);
    sem_destroy(&g_state->weapon_drop_sem);
    sem_destroy(&g_state->deadlock_choice_sem);
    sem_destroy(&g_state->gui_input_sem);

    // Destroy mutexes
    pthread_mutex_destroy(&g_state->state_lock);
    pthread_mutex_destroy(&g_state->resource_table.table_lock);

    // Unmap and unlink shared memory
    munmap(g_state, SHM_SIZE);
    close(g_shm_fd);
    shm_unlink(SHM_NAME);

    exit(0);
}