#pragma once
#include "core/memory/Memory.hpp"

#include <cmath>
#include <deque>
#include "core/engine/types/Vec3.hpp"

// Subtick air strafe. CreateMove builds the subtick move steps, the button state & move_crc of the command of a tick
// from the input events of that tick in CCSGOInput, all before the command is predicted & sent. We add our events
// there at the start of every tick in the air, when the player pressed nothing: the game makes them steps itself.
// The bunny hop goes there too: a jump press right after each landing, at its point in the tick
class Subtick {
public:
    ~Subtick()                         = default;
    Subtick(const Subtick&)            = delete;
    Subtick(Subtick&&)                 = delete;
    Subtick& operator=(const Subtick&) = delete;
    Subtick& operator=(Subtick&&)      = delete;

    static bool Init();
    static bool IsAvailable();
    static void Shutdown();

    // The bunny hop is ours, a subtick jump right after each landing. Movement leaves the jump key alone then
    static bool HandlesBhop();

    // The air strafe & jump bug are turned on by their key (or always), whether in the air or not
    static bool IsAirStrafeOn();
    static bool IsJumpBugOn();
private:
    Subtick() {};

    static Subtick& GetInstance()
    {
        static Subtick i{};
        return i;
    }

    bool InitImpl();
    void Thread();

public:
    // An input event of a tick in CCSGOInput, its view is the absolute one
    struct Event {
        float when;             // Part of the tick, 0 to 1
        uint32_t padding;
        uint64_t button;
        uint32_t pressed;       // bool, or the forward analog delta when button is 0
        float analog_left;
        float pitch;
        float yaw;
    };
    static_assert(sizeof(Event) == 0x20);

private:
    // The air of the server we play on, community servers change it
    void ReadConVars();
    // The sequence of the command of this tick, -1 without one
    int GetSequence();
    // Events for this tick, 0 for none
    int Plan(uintptr_t slot, Event* out);

    // Ticks of the tickbase: the command being built & whether the game ran it yet. Prediction raises the tickbase once
    // it ran it, the most of a moment back is the lead after that
    struct Build {
        int tickbase = 0;   // Of the command being built
        bool fresh = false; // Not run yet: the player is where it starts
    };
    Build UpdateBuild(uintptr_t controller, int sequence);

    // Bunny hop: where in the command being built our jump goes, if anywhere
    struct JumpPlan {
        enum Kind { NONE, A, B, GUESS } kind = NONE;
        float when = 0.f;
        int build = 0;  // Tickbase of the command being built
        int sequence = 0;
    };
    JumpPlan PlanJump(uintptr_t pawn, int sequence, const Build& build);
    void CommitJump(const JumpPlan& plan);
    // Part of the coming tick where the feet touch standable ground, -1 when they do not
    float PredictContact(uintptr_t pawn);

    // Jump bug: duck in the air, stand up right above the ground & jump in that moment, the landing never happens
    struct Motion {
        bool valid = false;
        Vec3_t origin{}, velocity{};
        uint32_t flags = 0;
        float gravity_scale = 1.f, duck_speed = 0.f, last_duck_time = 0.f;
        int last_jump_tick = 0;
        // The last jump & landing as the game predicted them: tick & part of it, the jump from where & how fast up
        float jump_frac = 0.f, jump_height = 0.f, jump_vz = 0.f;
        int landed_tick = 0;
        float landed_frac = 0.f;
        float fall_velocity = 0.f;
    };
    Motion ReadMotion(uintptr_t pawn);
    // Our steps for the command being built, decided once per command. Empty for none
    std::vector<Event> PlanBug(uintptr_t slot, int sequence, const Build& build, bool jump_held, bool duck_held);

    // Our steps in the events of the tick, in place of the ones we put there before. Events of the game with a button
    // of `owned` go. True when they are there, `wrote` when we put them there now. With `fix_order` events the game
    // added after ours with an earlier time are put in order too
    bool PlaceSteps(uintptr_t slot, const std::vector<Event>& steps, uint64_t owned, bool fix_order, bool& wrote);

    // A feature turned on by a key: toggled, held or always
    struct KeyState {
        bool toggled = false;
        bool was_down = false;
    };
    static bool IsKeyOn(int mode, int key, KeyState& state);

private:
    bool available = false;
    uintptr_t convar_accelerate = 0, convar_wishspeed = 0;
    float air_accelerate = 12.f, air_max_wishspeed = 30.f;
    std::atomic<bool> stopping = false;

    // Keys
    KeyState strafe_key{}, bug_key{};
    std::atomic<bool> strafe_on = false, bug_on = false;

    // Bunny hop
    uintptr_t convar_autobhop = 0, convar_legacy_jump = 0, convar_gravity = 0;
    std::atomic<bool> other_jump = false;   // sv_autobunnyhopping or sv_legacy_jump: the game jumps another way
    float gravity = 800.f;

    // The landing being answered
    struct Landing {
        bool valid = false;
        int tick = 0;
        float frac = 0.f;
        bool took_off = false;
        bool a_placed = false; int a_build = 0; float a_when = 0.f; int a_sequence = -1;
        bool b_placed = false; int b_build = 0;
    } landing;

    // A press written before the game had a landing to write it from
    struct Pending {
        bool live = false;
        int build = 0;
        float when = 0.f;
    } pending;

    uintptr_t jump_pawn = 0;
    std::deque<std::pair<std::chrono::steady_clock::time_point, int>> leads; // Tickbase - command number, a moment back

    // Jump bug
    uintptr_t convar_duck_interval = 0, convar_jump_penalty = 0;
    float duck_interval = 0.4f, jump_penalty = 1.f / 64.f;
    std::map<int, Motion> starts;       // Where the player is at the start of a command, by command number
    bool bug_attempted = false;         // Once per fall
    int bug_attempt_jump_tick = -1;
    int bug_across = -1;                // Command owed the jump of a stand up that ends the one before
    int bug_sequence = -1;              // Command decided
    std::vector<Event> bug_steps;

    // Why the jump bug passed over the commands of a fall, logged on landing
    struct BugDebug {
        int commands = 0, no_start = 0, not_held = 0, duck_held = 0, attempted = 0, duck_not_ready = 0, no_ground = 0, rejected = 0;
        std::deque<std::string> last;   // Where the last commands of the fall started, & the ground found from there
    } bug_debug;
    bool bug_was_on = false;

    // Checked on landings without a jump bug, for the log: where the path we worked out meets the ground against the
    // landing the game wrote down
    struct LagSample {
        int build = 0;
        Vec3_t origin{}, velocity{};
        float gravity = 800.f;
    };
    std::deque<LagSample> lag_samples;
    int lag_landing_tick = -1;
    bool fall_attempted = false;
    void MeasureLag(uintptr_t pawn);

    // Debug: the events the jump bug command went with, & the commands after it to log
    std::vector<Event> bug_seen;
    int bug_seen_sequence = -1;
    int bug_follow = 0;
    struct BugResult {
        bool live = false;
        int tickbase = 0, landed_before = 0;
        bool in_tick = false;
        float clearance = 0.f, ducked = 0.f, stand = 0.f;
    } bug_result;

    // Our steps in the command being built
    int own_sequence = -1;
    std::vector<Event> placed;
};
