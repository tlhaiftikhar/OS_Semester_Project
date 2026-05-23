
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
#include <ctime>
#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/syscall.h>     
#include <iostream>

#include "../shared.h"

// Section 5 — kernel TID of the calling thread (Linux). Each enemy worker
// publishes itself in shared memory so the Arbiter can deliver SIGUSR1
// straight to that one thread via tgkill.
static inline pid_t my_tid() { return (pid_t)syscall(SYS_gettid); }

// ─────────────────────────────────────────────
//  GLOBALS
// ─────────────────────────────────────────────
static SharedState *g_state = nullptr;
static int g_shm_fd = -1;

struct EnemyThreadArg {
    int enemy_index;
};

static EnemyThreadArg g_thread_args[MAX_ENEMIES];
static pthread_t g_enemy_threads[MAX_ENEMIES];

// Quit flag — set by signal handler, read by main loop
static sig_atomic_t g_quit = 0;

// ─────────────────────────────────────────────
//  FORWARD DECLARATIONS
// ─────────────────────────────────────────────
void attach_shared_memory();
void *enemy_thread(void *arg);
void decide_enemy_action(int enemy_idx);
void cleanup_and_exit();

// ─────────────────────────────────────────────
//  SIGNAL HANDLERS — async-signal-safe ONLY
// ─────────────────────────────────────────────

void sig_usr1_handler(int)
{
    struct timespec end;
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0)
        return;
    end.tv_sec += (time_t)(CHRONO_STUN_DURATION_NS / 1000000000LL);
    end.tv_nsec += (long)(CHRONO_STUN_DURATION_NS % 1000000000LL);
    while (end.tv_nsec >= 1000000000L)
    {
        end.tv_sec++;
        end.tv_nsec -= 1000000000L;
    }
    int r;
    do
    {
        r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &end, nullptr);
    } while (r == EINTR);
}

// Section 5 — state_lock acquired with SIGUSR1 BLOCKED, so the 3-second

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

// SIGTERM — Arbiter signals quit
void sig_term_handler(int)
{
    g_quit = 1;
}

// SIGSTOP / SIGCONT handled by OS directly (Ultimate Ability — Section 8)
// Arbiter sends SIGSTOP → entire ASP process freezes
// Arbiter sends SIGCONT after 10 seconds → process resumes
// No handler needed — OS manages this automatically

// ─────────────────────────────────────────────
//  MAIN
// ─────────────────────────────────────────────
int main()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_usr1_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    // Block SIGUSR1 while the handler runs so we never queue a second
    // 3 s nanosleep on top of the current one.
    sigaddset(&sa.sa_mask, SIGUSR1);
    sigaction(SIGUSR1, &sa, nullptr);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_term_handler;
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);

    // Section 5 — block SIGUSR1 in main thread BEFORE pthread_create so
    // each enemy worker thread inherits a blocked SIGUSR1. The worker
    // then unblocks SIGUSR1 only for itself (after publishing its TID),
    // ensuring the SIGUSR1 stun handler ONLY ever runs on the targeted
    // worker thread — never on main, never on the wrong worker.
    {
        sigset_t block;
        sigemptyset(&block);
        sigaddset(&block, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &block, nullptr);
    }

    attach_shared_memory();

    // Wait for Arbiter to finish initializing
    int retries = 20;
    while (retries-- > 0)
    {
        pthread_mutex_lock(&g_state->state_lock);
        GamePhase ph = g_state->phase;
        pthread_mutex_unlock(&g_state->state_lock);
        if (ph == GamePhase::Running)
            break;
        sleep(1);
    }

    pthread_mutex_lock(&g_state->state_lock);
    int enemy_count = g_state->enemy_count;
    pthread_mutex_unlock(&g_state->state_lock);

    // Section 2: "Each NPC must run in its own dedicated thread"
    for (int i = 0; i < enemy_count; i++)
    {
        g_thread_args[i].enemy_index = i;
        pthread_create(&g_enemy_threads[i], nullptr,
                       enemy_thread, &g_thread_args[i]);
    }

    // Main thread waits for quit or game over
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
        perror("ASP: shm_open failed");
        exit(1);
    }
    g_state = (SharedState *)mmap(nullptr, SHM_SIZE,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED, g_shm_fd, 0);
    if (g_state == MAP_FAILED)
    {
        perror("ASP: mmap failed");
        exit(1);
    }
}

// ─────────────────────────────────────────────
//  DECIDE ENEMY ACTION
//   chooses Strike or Skip
// ─────────────────────────────────────────────
void decide_enemy_action(int enemy_idx)
{
    lock_state();

    EnemyCharacter me = g_state->enemies[enemy_idx];
    (void)me; // snapshot for potential future AI logic
    int player_count = g_state->player_count;
    PlayerCharacter snap_p[MAX_PLAYERS];
    memcpy(snap_p, g_state->players,
           sizeof(PlayerCharacter) * player_count);

    unlock_state();

    ActionRequest req;
    memset(&req, 0, sizeof(req));
    req.source_id = enemy_idx;
    req.source_is_player = false;
    req.swap_weapon = WeaponType::None;
    req.weapon_slot = -1;

    // Find alive players — target the one with lowest HP
    int target = -1;
    int lowest_hp = INT_MAX;
    bool any_alive = false;

    for (int i = 0; i < player_count; i++)
    {
        if (snap_p[i].alive)
        {
            any_alive = true;
            if (snap_p[i].hp < lowest_hp)
            {
                lowest_hp = snap_p[i].hp;
                target = i;
            }
        }
    }

    if (!any_alive || target == -1)
    {
        req.type = ActionType::Skip;
    }
    else
    {
        int roll = rand() % 10;
        if (roll == 0)
        {
            req.type = ActionType::Skip;
        }
        else
        {
            req.type = ActionType::Strike;
            req.target_id = target;
        }
    }

    // Section 2: ASP must not directly modify game state
    lock_state();
    g_state->pending_action = req;
    g_state->turn.action_submitted = true;
    unlock_state();

    sem_post(&g_state->action_ready_sem);
}

// ─────────────────────────────────────────────
//  ENEMY THREAD
//  One per enemy — mandatory (Section 2)
// ─────────────────────────────────────────────
void *enemy_thread(void *arg)
{
    EnemyThreadArg *earg = (EnemyThreadArg *)arg;
    int idx = earg->enemy_index;

    srand(idx * 1000 + (int)time(nullptr));

    // Section 5 — publish kernel TID under state_lock (SIGUSR1 still

    pthread_mutex_lock(&g_state->state_lock);
    g_state->enemy_tid[idx] = my_tid();
    pthread_mutex_unlock(&g_state->state_lock);

    // Section 5 — unblock SIGUSR1 for this thread only.
    {
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, SIGUSR1);
        pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
    }

    while (true)
    {
        // Block on this enemy's semaphore — only woken when it's our turn.
        
        while (sem_wait(&g_state->enemy_turn_sem[idx]) == -1 && errno == EINTR)
            ;

        if (g_quit)
            break;

        lock_state();
        bool over = (g_state->phase == GamePhase::GameOver);
        bool alive = g_state->enemies[idx].alive;
        unlock_state();

        if (over)
            break;
        // If dead, loop back to sem_wait — supports respawning
        if (!alive)
            continue;

        decide_enemy_action(idx);
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
