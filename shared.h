
#pragma once

#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>

// ─────────────────────────────────────────────
//  CONSTANTS
// ─────────────────────────────────────────────

#define SHM_NAME            "/chrono_rift_shm"

#define MAX_PLAYERS         4
#define MAX_ENEMIES         9
#define INVENTORY_SLOTS     20
#define MAX_LONG_TERM       32
#define ACTION_LOG_LINES    10
#define ACTION_LOG_LEN      128

#define MAX_STAMINA_PLAYER  100
#define MAX_STAMINA_ENEMY   150

// Section 5 — stun length in nanoseconds (CLOCK_MONOTONIC). Single source of
// truth for Arbiter, HIP, ASP, and renderer deadline math.
#define CHRONO_STUN_DURATION_NS 3000000000LL
// Section 8 — ultimate freeze window in seconds (used with alarm()).
#define CHRONO_ULTIMATE_DURATION_S 10

#define TOTAL_KILLS_TO_WIN  10



enum class WeaponType {
    None         = 0,
    SolarCore,        // 10 slots  95 dmg   ARTIFACT
    LunarBlade,       // 10 slots  90 dmg   ARTIFACT
    IronHalberd,      //  7 slots  55 dmg
    VenomDagger,      //  4 slots  30 dmg
    Thunderstaff,     //  6 slots  50 dmg
    ObsidianAxe,      //  5 slots  45 dmg
    Frostbow,         //  6 slots  48 dmg
    SplinterStick,    //  2 slots  12 dmg
    EclipseRelic,     //  8 slots  80 dmg   ARTIFACT (dynamic)
    COUNT
};

enum class ActionType {
    None = 0,
    Strike,       // basic attack        → reduce enemy HP
    Exhaust,      // stamina attack      → reduce enemy Stamina
    UseWeapon,    // use inventory weapon → enemy HP
    SwapIn,       // bring weapon from long-term storage
    MoveToLongTerm, // move one inventory weapon to long-term (full turn)
    Heal,         // restore 10% own HP
    Skip,         // skip turn, stamina → 50%
    Ultimate      // requires SolarCore + LunarBlade in active inventory
};

enum class GamePhase {
    Init     = 0,  // startup, not yet running
    Running  = 1,  // normal gameplay
    Ultimate = 2,  // Ultimate active — ASP suspended 10s
    GameOver = 3   // win or lose — cleanup in progress
};

enum class GameResult {
    None = 0,
    Win,
    Lose,
    Quit
};

enum class TurnOwner {
    Player = 0,
    Enemy  = 1
};

enum class ArtifactID {
    SolarCore    = 0,
    LunarBlade   = 1,
    EclipseRelic = 2,
    COUNT        = 3
};

// ─────────────────────────────────────────────
//  WEAPON LOOKUP TABLES
//  Indexed by  (int)WeaponType::XYZ
// ─────────────────────────────────────────────

static const int WEAPON_SLOTS[(int)WeaponType::COUNT] = {
    0,   // None
    10,  // SolarCore
    10,  // LunarBlade
    7,   // IronHalberd
    4,   // VenomDagger
    6,   // Thunderstaff
    5,   // ObsidianAxe
    6,   // Frostbow
    2,   // SplinterStick
    8,   // EclipseRelic
};

static const int WEAPON_DAMAGE[(int)WeaponType::COUNT] = {
    0,   // None
    95,  // SolarCore
    90,  // LunarBlade
    55,  // IronHalberd
    30,  // VenomDagger
    50,  // Thunderstaff
    45,  // ObsidianAxe
    48,  // Frostbow
    12,  // SplinterStick
    80,  // EclipseRelic
};

[[maybe_unused]] static const char* WEAPON_NAMES[(int)WeaponType::COUNT] = {
    "None",
    "Solar Core",
    "Lunar Blade",
    "Iron Halberd",
    "Venom Dagger",
    "Thunderstaff",
    "Obsidian Axe",
    "Frostbow",
    "Splinter Stick",
    "Eclipse Relic",
};

// ─────────────────────────────────────────────
//  INVENTORY SLOT
//  Primary inventory = flat array of 20 slots.
//  A weapon occupies N contiguous slots.
//  is_start = true only on the FIRST slot of a weapon.
// ─────────────────────────────────────────────

struct InventorySlot {
    WeaponType  weapon;    // WeaponType::None if empty
    bool        is_start;  // true = first slot of this weapon
};

// ─────────────────────────────────────────────
//  LONG-TERM STORAGE ENTRY
// ─────────────────────────────────────────────

struct LongTermEntry {
    WeaponType  weapon;
    bool        occupied;
};

// ─────────────────────────────────────────────
//  ACTION REQUEST
//  HIP / ASP write this into shared memory.
//  Arbiter reads it and applies it to game state.
// ─────────────────────────────────────────────

struct ActionRequest {
    ActionType  type;
    int         source_id;         // index into players[] or enemies[]
    bool        source_is_player;  // true = player, false = enemy
    int         target_id;         // enemy index (attacks) or player index
    int         weapon_slot;       // inventory slot index (for UseWeapon)
    WeaponType  swap_weapon;       // weapon to bring back (for SwapIn)
};

// ─────────────────────────────────────────────
//  PLAYER CHARACTER
// ─────────────────────────────────────────────

struct PlayerCharacter {
    bool            alive;
    int             hp;
    int             max_hp;
    int             damage;              // lastDigit_of_rollno + 10
    float           stamina;             // current stamina
    float           speed;               // 100 / player_count
    bool            stunned;             // true during 3-second stun
    bool            ready_to_act;        // stamina reached MAX_STAMINA_PLAYER

    InventorySlot   inventory[INVENTORY_SLOTS];

    LongTermEntry   long_term[MAX_LONG_TERM];
    int             long_term_count;

    bool            just_swapped_in;     // cannot use swapped weapon this turn
    int             fill_tick;           // tick when stamina reached max (first-to-fill priority)
    long long       stun_apply_ns;       // CLOCK_MONOTONIC ns when stun was applied (Section 5)
};

// ─────────────────────────────────────────────
//  ENEMY CHARACTER
// ─────────────────────────────────────────────

struct EnemyCharacter {
    bool        alive;
    int         hp;
    int         max_hp;
    int         damage;       // secondLastDigit_of_rollno + 10
    float       stamina;
    float       speed;        // rand(10, 30)
    bool        stunned;
    bool        ready_to_act;

    WeaponType  held_weapon;  // NOT dropped on death (per spec)
    int         fill_tick;    // tick when stamina reached max (first-to-fill priority)
    long long   stun_apply_ns; // CLOCK_MONOTONIC ns when stun was applied (Section 5)
};

// ─────────────────────────────────────────────
//  ARTIFACT ENTRY
//  One per artifact in the resource table.
// ─────────────────────────────────────────────

struct ArtifactEntry {
    bool    exists;            // EclipseRelic = false until introduced
    bool    is_free;           // true = nobody holds it
    bool    held_by_player;    // true = player holds, false = enemy holds
    int     holder_index;      // index into players[] or enemies[]

    // Deadlock tracking — waiter must record entity kind + index so player i
    // is distinguishable from enemy i (Section 7 circular wait).
    bool    has_waiter;         // true = an entity is blocked waiting on this artifact
    bool    waiter_is_player;   // if has_waiter: waiter is players[waiter_index]
    int     waiter_index;       // index into players[] or enemies[]
};

// ─────────────────────────────────────────────
//  RESOURCE TABLE
//  Lock table_lock BEFORE reading or writing any entry.
// ─────────────────────────────────────────────

struct ResourceTable {
    ArtifactEntry   entries[(int)ArtifactID::COUNT];
    pthread_mutex_t table_lock;
};

// ─────────────────────────────────────────────
//  TURN CONTROL
//  Arbiter writes these. HIP / ASP read them.
// ─────────────────────────────────────────────

struct TurnControl {
    TurnOwner   owner;             // Player or Enemy
    int         entity_index;      // which player or enemy acts
    bool        action_submitted;  // HIP/ASP set true after writing action
    bool        action_applied;    // Arbiter sets true after applying action
};

// ─────────────────────────────────────────────
//  ACTION LOG  (circular buffer — read by renderer)
// ─────────────────────────────────────────────

struct ActionLog {
    char  lines[ACTION_LOG_LINES][ACTION_LOG_LEN];
    int   head;    // next write index
    int   count;   // lines populated so far
};

// ─────────────────────────────────────────────
//  MASTER SHARED STATE
//  One instance lives in POSIX shared memory.
//  All 3 processes (arbiter, hip, asp) mmap this.
// ─────────────────────────────────────────────

struct SharedState {

    // ── Entities ──────────────────────────────────────────────
    int             player_count;           // 1–4  set at startup
    int             enemy_count;            // 2–9  random each run
    PlayerCharacter players[MAX_PLAYERS];
    EnemyCharacter  enemies[MAX_ENEMIES];

    // ── Game Progress ──────────────────────────────────────────
    int             total_kills;            // win when this hits 10
    GamePhase       phase;
    GameResult      result;

    // ── Turn Scheduling ───────────────────────────────────────
    TurnControl     turn;

    // Semaphores for turn signalling:
    //   Arbiter posts player_turn_sem[i] → player thread i wakes up
    //   Arbiter posts enemy_turn_sem[i]  → enemy  thread i wakes up
    //   HIP/ASP post action_ready_sem    → Arbiter wakes to apply action
    sem_t           player_turn_sem[MAX_PLAYERS];
    sem_t           enemy_turn_sem[MAX_ENEMIES];
    sem_t           action_ready_sem;

    // ── Artifacts & Deadlock ──────────────────────────────────
    ResourceTable   resource_table;
    // Section 7 — deadlock-resolution HUD (arbiter sets; renderer reads/decays)
    int             resource_hud_resolve_ttl;
    int             resource_hud_last_victim_artifact; // (int)ArtifactID or -1

    // ── Pending Action ─────────────────────────────────────────
    ActionRequest   pending_action;

    // ── Weapon Drop (triggered after enemy death) ──────────────
    bool            weapon_drop_pending;
    WeaponType      dropped_weapon;
    int             drop_for_player;       // which player is being asked
    bool            player_accepted_drop;
    bool            drop_decision_ready;   // HIP sets true after player decides
    sem_t           weapon_drop_sem;       // Arbiter blocks; HIP posts when decided
    long long       weapon_drop_deadline_ns;

    // Section 7 — PvP Solar/Lunar deadlock: player chooses who releases (GUI + HIP)
    bool            deadlock_modal_pending;
    int             deadlock_prompt_player;
    int             deadlock_idx_solar;
    int             deadlock_idx_lunar;
    int             deadlock_user_choice;
    char            deadlock_line_main[160];
    char            deadlock_line_opt1[120];
    char            deadlock_line_opt2[120];
    long long       deadlock_modal_deadline_ns;
    sem_t           deadlock_choice_sem;

    // ── Action Log (for SFML renderer) ────────────────────────
    ActionLog       log;

    // ── SFML GUI Keyboard Input ───────────────────────────────
    // Arbiter rendering thread captures key presses; HIP consumes them.
    // No pipes/FIFOs — pure shared memory + semaphore handshake.
    int             gui_pressed_key;        // 0-9 last digit pressed
    int             gui_prompt_mode;        // 0 = idle, 1 = HIP waiting for input
    int             gui_prompt_max;         // max valid digit accepted
    int             gui_prompt_player;      // which player the prompt is for
    char            gui_prompt_text[80];    // human-readable prompt for renderer
    char            gui_action_label[24];   // currently chosen action label (visual)
    sem_t           gui_input_sem;          // posted by renderer; HIP waits on it

    // ── Action Animation Hint (renderer reads, arbiter writes) ─
    // Set by apply_action; renderer plays slide-attack + damage popup.
    int             anim_attacker;          // index, -1 = none
    bool            anim_attacker_is_player;
    int             anim_target;            // index, -1 = none
    bool            anim_target_is_player;
    int             anim_damage;            // damage to display above target
    int             anim_kind;              // 0 = none, 1 = strike, 2 = useweapon,
                                            // 3 = exhaust, 4 = heal, 5 = skip, 6 = swap, 7 = ultimate
    int             anim_frames_left;       // counts down each frame in renderer

    // ── Process IDs ───────────────────────────────────────────
    pid_t           arbiter_pid;
    pid_t           hip_pid;
    pid_t           asp_pid;

    // Section 6 — inventory animation hints (renderer reads, arbiter writes).
    int             inv_anim_player;
    int             inv_anim_place_slot;
    int             inv_anim_place_size;
    int             inv_anim_place_ttl;
    int             inv_anim_evict_slot;
    int             inv_anim_evict_size;
    int             inv_anim_evict_ttl;
    int             inv_anim_swapin_slot;
    int             inv_anim_swapin_size;
    int             inv_anim_swapin_ttl;

    // Section 5 — per-thread kernel TIDs for tgkill stun targeting.
    pid_t           player_tid[MAX_PLAYERS];
    pid_t           enemy_tid[MAX_ENEMIES];

    // ── Global State Lock ─────────────────────────────────────

    pthread_mutex_t state_lock;
};

#define SHM_SIZE  sizeof(SharedState)

// ─────────────────────────────────────────────
//  INLINE HELPER FUNCTIONS
// ─────────────────────────────────────────────

inline bool weapon_is_artifact(WeaponType w) {
    return (w == WeaponType::SolarCore   ||
            w == WeaponType::LunarBlade  ||
            w == WeaponType::EclipseRelic);
}

// Returns true if player has SolarCore AND LunarBlade in active inventory
inline bool player_has_ultimate(const PlayerCharacter& p) {
    bool has_solar = false;
    bool has_lunar = false;
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        if (p.inventory[i].is_start) {
            if (p.inventory[i].weapon == WeaponType::SolarCore)  has_solar = true;
            if (p.inventory[i].weapon == WeaponType::LunarBlade) has_lunar = true;
        }
    }
    return has_solar && has_lunar;
}

// Returns first slot index of a weapon in inventory, or -1 if not found
inline int find_weapon_in_inventory(const PlayerCharacter& p, WeaponType w) {
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        if (p.inventory[i].weapon == w && p.inventory[i].is_start)
            return i;
    }
    return -1;
}

