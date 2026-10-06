#include "Subtick.hpp"

#include "core/engine/Engine.hpp"
#include "core/features/Movement.hpp"
#include "core/features/View.hpp"
#include "core/offsets/Offsets.hpp"
#include "core/engine/types/Vec3.hpp"
#include "core/engine/world/MapCollision.hpp"

#include <cmath>
#include <climits>
#include <numbers>
#include <optional>

namespace {
    namespace uc = offsets::usercmd;

    // The input state of the first player slot in CCSGOInput, the events of the tick go into its commands
    constexpr std::ptrdiff_t SLOT = 0x228;
    constexpr std::ptrdiff_t SLOT_HELD = 0x30;      // uint64 buttons held
    constexpr std::ptrdiff_t SLOT_PRESSED = 0x38;   // uint64 buttons pressed in the tick
    constexpr std::ptrdiff_t SLOT_RELEASED = 0x40;  // uint64 buttons let go of in the tick
    constexpr std::ptrdiff_t SLOT_EVENTS = 0x5C;    // int32, emptied when a tick starts
    constexpr std::ptrdiff_t SLOT_EVENT = 0x60;     // Subtick::Event[MAX_EVENTS]
    constexpr std::ptrdiff_t SLOT_VIEW = 0x460;     // QAngle, m_angViewAngles of the first slot
    constexpr int MAX_EVENTS = 32;

    // The plan: SEGMENTS parts of a tick taking turns between A & D, let go of at LAST_WHEN
    constexpr int SEGMENTS = 4;
    constexpr float LAST_WHEN = 0.99f;
    constexpr float MIN_SPEED = 50.f;
    constexpr float ALIGNED = 3.f;                  // Degrees between the flight & where to, taking turns below
    constexpr float AIR_MAX_WISHSPEED = 30.f;       // sv_air_max_wishspeed & sv_airaccelerate of the game, until the
    constexpr float AIR_ACCELERATE = 12.f;          // ones of the server are read
    constexpr auto CONVAR_READ = 2s;
    constexpr float MAX_SPEED = 250.f;              // Knife, a bit less with a weapon: then we only fall a bit short
    constexpr float TICK_INTERVAL = 1.f / 64.f;
    constexpr uint32_t FL_ONGROUND = 1 << 0;
    constexpr auto POLL = 1ms;

    constexpr uint64_t IN_JUMP = 1 << 1;
    constexpr uint64_t IN_FORWARD = 1 << 3, IN_BACK = 1 << 4, IN_MOVELEFT = 1 << 9, IN_MOVERIGHT = 1 << 10;
    constexpr uint64_t MOVE_BUTTONS = IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT;

    // Bunny hop. The game takes a press on a 1/64 grid of the tick; a jump that keeps the speed of the landing needs
    // the press within a quarter tick of the landing, in the command after it (A). B, a tick later, is the jump that
    // keeps the feet from sticking when A was missed
    constexpr float STEP = 1.f / 64.f;
    constexpr float FIRST = STEP;               // First point a press may take
    constexpr float LAST = 1.f - STEP;          // Last point a press may take
    constexpr float LEAD = LAST - STEP;         // Last point A may take, B needs one of its own after it
    constexpr int JUMP_OFFSET = 3;              // Steps A stands past the landing
    constexpr int B_CATCH_UP = 4;               // Commands after its own B may still go in, the tickbase skips online
    constexpr auto LEAD_WINDOW = 250ms;
    constexpr float STANDABLE = 0.7f;           // Normal z of ground the feet stay on
    constexpr float HULL_EDGE = 15.9f;          // Half the width of the player, a bit inside

    float Grid(float frac) {
        return std::floor(frac * 64.f + 0.5f) * STEP;
    }

    // Room for A behind a landing that far into its tick
    bool PostFits(float frac) {
        return Grid(frac) + STEP <= LEAD;
    }

    // A past a landing inside its tick: a step clear of the landing at least, never past LEAD
    float PostContact(float contact) {
        float point = std::min(Grid(contact + JUMP_OFFSET * STEP), LEAD);
        return std::max(point, contact + STEP);
    }

    // A in the command after the landing. A landing on the boundary (frac 0) is the start of that command: all of it
    // is near enough. Too late in its tick for A behind it, A leads it by a step instead
    std::optional<float> PairA(float frac) {
        float when;
        if (!(frac > 0.f)) {
            when = JUMP_OFFSET * STEP;
        } else if (PostFits(frac)) {
            when = PostContact(Grid(frac));
        } else {
            float contact = Grid(frac);
            when = contact - STEP >= FIRST ? contact - STEP : contact + STEP;
        }

        if (!(when >= FIRST && when <= LAST))
            return std::nullopt;
        return when;
    }

    // Jump bug: ducked in the air, standing up right above the ground puts the feet on it (the game does within a
    // couple of units) & the jump in that same moment leaves before the landing. After the code of the same author
    constexpr uint64_t IN_DUCK = 1 << 2;
    constexpr uint32_t FL_DUCKING = 1 << 1;
    constexpr float BUG_PROBE = 2.f;            // The game puts the feet on ground this close under them
    constexpr float BUG_CLEARANCE = 1.f;        // Standing up this far above the ground
    constexpr float BUG_MAX_CONTACT = 4.f;      // Ticks ahead a landing is looked for
    constexpr float BUG_LANDING = 3.f * STEP;   // The jump again past the landing, in case standing up landed
    constexpr float MIN_DUCK_SPEED = 1.5f;      // Slower the duck is not done in time
    constexpr float DRAWN_LAG = 2.f;            // Ticks the drawn origin is about behind, for where across the player is

    struct Contact {
        float ticks;    // Until the feet are within BUG_PROBE of the ground
        float fall;     // Towards the ground per tick
        float drop;     // Down per tick
    };

    // The middle & the corners of the player, the first to reach standable ground
    std::optional<Contact> PredictBugContact(Vec3_t origin, Vec3_t velocity, float gravity) {
        if (!MapCollision::IsLoaded())
            return std::nullopt;

        // Half the gravity of the tick: its fall goes by the mean of the speeds at both ends
        velocity.z -= 0.5f * gravity * TICK_INTERVAL;
        auto travel = velocity * TICK_INTERVAL;
        if (!std::isfinite(origin.x) || !std::isfinite(origin.z) || !std::isfinite(travel.x) || !std::isfinite(travel.z))
            return std::nullopt;

        const float corners[5][2] = { { 0.f, 0.f }, { -HULL_EDGE, -HULL_EDGE }, { -HULL_EDGE, HULL_EDGE }, { HULL_EDGE, -HULL_EDGE }, { HULL_EDGE, HULL_EDGE } };
        std::optional<Contact> best;

        for (const auto& corner : corners) {
            Vec3_t start{ origin.x + corner[0], origin.y + corner[1], origin.z };
            Vec3_t end = start + travel + Vec3_t{ 0.f, 0.f, -BUG_PROBE };

            MapCollision::Hit hit;
            if (!MapCollision::Trace(start, end, hit) || !(hit.fraction > 0.f && hit.fraction < 1.f) || !(hit.normal.z >= STANDABLE))
                continue;

            // Along the normal of the ground: how far a tick goes, & how far it is with the probe under the feet
            float tick = hit.normal.x * travel.x + hit.normal.y * travel.y + hit.normal.z * travel.z;
            if (!(tick < 0.f))
                continue;

            float segment = tick - BUG_PROBE * hit.normal.z;
            float ticks = hit.fraction * segment / tick;
            if (!(ticks > 0.f && ticks < BUG_MAX_CONTACT))
                continue;

            Contact contact{ ticks, -tick / hit.normal.z, -travel.z };
            if (!best || contact.ticks < best->ticks)
                best = contact;
        }

        return best;
    }

    // In one tick: duck, stand up a little above the ground with the jump, the jump once more past the landing.
    // Across: duck now, stand up at the very end, the jump at the start of the next command
    struct BugPlan {
        enum Shape { IN_TICK, ACROSS, ACROSS_JUMP } shape = IN_TICK;
        float duck = 0.f, stand = 0.f, landing = 0.f;
        bool landing_edge = false;

        size_t Steps() const { return shape == IN_TICK ? (landing_edge ? 6 : 5) : 2; }
    };

    float AfterContact(float contact) {
        return std::max(std::min(Grid(contact + BUG_LANDING), LEAD), contact + STEP);
    }

    std::optional<BugPlan> PlanInTick(const Contact& c, float floor, float penalty, float clearance_aimed) {
        if (!(c.fall > 0.f) || !(c.ticks > 0.f && c.ticks < 1.f + BUG_PROBE / c.fall))
            return std::nullopt;

        float duck = std::max(floor, 0.f);
        float contact = Grid(c.ticks);
        float aimed = c.ticks - clearance_aimed / c.fall;
        float aimed_point = aimed > 0.f ? std::floor(aimed / STEP) * STEP : 0.f;
        float stand = std::max(std::min(aimed_point, LEAD), duck + STEP);

        if (!(stand < contact) || !(stand <= LAST))
            return std::nullopt;

        // Ducked while still higher than the probe, standing up within it
        if (!((c.ticks - duck) * c.fall > BUG_PROBE))
            return std::nullopt;

        float clearance = (c.ticks - stand) * c.fall;
        if (!(clearance > 0.f && clearance < BUG_PROBE))
            return std::nullopt;

        float landing = AfterContact(c.ticks < 1.f ? contact : stand);
        if (!(landing > stand) || !(landing <= LAST))
            return std::nullopt;

        return BugPlan{ BugPlan::IN_TICK, duck, stand, landing, (landing - stand) * TICK_INTERVAL > penalty };
    }

    std::optional<BugPlan> PlanAcross(const Contact& c, float floor) {
        if (!(c.fall > 0.f) || !(c.ticks >= 1.f && c.ticks < 1.f + BUG_PROBE / c.fall))
            return std::nullopt;

        float duck = std::max(floor, 0.f);
        if (!((c.ticks - duck) * c.fall > BUG_PROBE) || !(duck + STEP <= 1.f))
            return std::nullopt;
        if (!((c.ticks - 1.f) * c.fall >= BUG_CLEARANCE))
            return std::nullopt;

        return BugPlan{ BugPlan::ACROSS, duck, 1.f, 0.f, false };
    }

    float Normalize(float angle) {
        while (angle > 180.f) angle -= 360.f;
        while (angle < -180.f) angle += 360.f;
        return angle;
    }

    void SortByTime(Subtick::Event* events, int count) {
        std::stable_sort(events, events + count, [](const Subtick::Event& a, const Subtick::Event& b) { return a.when < b.when; });
    }

    // The game added events of its own after ours in the same tick, with an earlier time than our last ones. Out of order
    // the server might take the tick another way than we did. Jumps (like jump on the mouse wheel) go in between ours by
    // their time. Anything else is the player: ours up to the first of it, the move keys there back as the game holds
    // them, then its events. 0 when already in order
    int Merge(const Subtick::Event* ours, int our_count, const Subtick::Event* game, int game_count, uint64_t held,
        Subtick::Event* out, int max) {
        float first = 1.f, cut = 2.f;
        for (int i = 0; i < game_count; i++) {
            first = std::min(first, game[i].when);
            if (game[i].button != IN_JUMP)
                cut = std::min(cut, game[i].when);
        }

        if (our_count == 0 || first >= ours[our_count - 1].when)
            return 0;

        if (cut > 1.f) {
            if (our_count + game_count > max)
                return 0;
            int n = 0;
            for (int i = 0; i < our_count; i++)
                out[n++] = ours[i];
            for (int i = 0; i < game_count; i++)
                out[n++] = game[i];
            SortByTime(out, n);
            return n;
        }

        // What the game held before its events
        for (int i = game_count - 1; i >= 0; i--)
            if (game[i].button & MOVE_BUTTONS)
                held = game[i].pressed ? held & ~game[i].button : held | game[i].button;

        int n = 0, i = 0;
        uint64_t state = held;
        for (; i < our_count && ours[i].when < cut; i++) {
            out[n++] = ours[i];
            state = ours[i].pressed ? state | ours[i].button : state & ~ours[i].button;
        }

        if (i == our_count || n + 4 + game_count > max)
            return 0;

        // The part up to the cut moves with the view of our event that ended it
        for (auto button : { IN_FORWARD, IN_BACK, IN_MOVELEFT, IN_MOVERIGHT }) {
            if ((held & button) == (state & button))
                continue;
            auto& e = out[n++];
            e = {};
            e.when = cut;
            e.button = button;
            e.pressed = (held & button) != 0;
            e.pitch = ours[i].pitch;
            e.yaw = ours[i].yaw;
        }

        for (int k = 0; k < game_count; k++)
            out[n++] = game[k];
        SortByTime(out, n); // Jumps of the game before the cut
        return n;
    }
}

bool Subtick::Init() {
    return GetInstance().InitImpl();
}

bool Subtick::IsAvailable() {
    return GetInstance().available;
}

bool Subtick::IsAirStrafeOn() {
    return GetInstance().strafe_on;
}

bool Subtick::IsJumpBugOn() {
    return GetInstance().bug_on;
}

bool Subtick::IsKeyOn(int mode, int key, KeyState& state) {
    bool down = key && Movement::IsKeyUsable(key) && (GetAsyncKeyState(key) & 0x8000);
    if (down && !state.was_down)
        state.toggled = !state.toggled;
    state.was_down = down;

    switch (mode) {
    case 0:     return state.toggled;
    case 1:     return down;
    default:    return true;
    }
}

bool Subtick::HandlesBhop() {
    auto& s = GetInstance();
    return s.available && cfg::misc::bhop && !s.other_jump;
}

bool Subtick::InitImpl() {
    // The layout is only known for this build: the view of the first slot is where the dumper found it
    if (!Engine::IsInsecure() || !offsets::input::dwCSGOInput || !uc::dwManagers || offsets::input::m_angViewAngles != SLOT + SLOT_VIEW) {
        LOGF(WARNING, "The input of the game is not as expected, subtick strafe is disabled");
        return false;
    }

    this->available = true;
    std::thread(&Subtick::Thread, this).detach();
    return true;
}

void Subtick::ReadConVars() {
    auto p = Engine::GetProcess();

    // Found once, their values change with the server we play on
    if (!this->convar_accelerate)
        this->convar_accelerate = View::FindConVar("sv_airaccelerate");
    if (!this->convar_wishspeed)
        this->convar_wishspeed = View::FindConVar("sv_air_max_wishspeed");

    auto read = [&](uintptr_t data, float fallback) {
        float value = data ? p->read<float>(data + View::CONVAR_VALUE) : fallback;
        return std::isfinite(value) && value > 0.f && value < 10000.f ? value : fallback;
    };

    float accelerate = read(this->convar_accelerate, AIR_ACCELERATE);
    float wishspeed = read(this->convar_wishspeed, AIR_MAX_WISHSPEED);

    if (accelerate != this->air_accelerate || wishspeed != this->air_max_wishspeed) {
        LOGF(INFO, "Subtick strafe for the air of this server: accelerate {:g}, max wish speed {:g}{}", accelerate, wishspeed,
            this->convar_accelerate && this->convar_wishspeed ? "" : " (not all read, defaults for the rest)");
        this->air_accelerate = accelerate;
        this->air_max_wishspeed = wishspeed;
    }

    // The jump: servers with automatic or the old jump take off another way, the bunny hop of Movement then
    if (!this->convar_autobhop)
        this->convar_autobhop = View::FindConVar("sv_autobunnyhopping");
    if (!this->convar_legacy_jump)
        this->convar_legacy_jump = View::FindConVar("sv_legacy_jump");
    if (!this->convar_gravity)
        this->convar_gravity = View::FindConVar("sv_gravity");
    if (!this->convar_duck_interval)
        this->convar_duck_interval = View::FindConVar("sv_timebetweenducks");
    if (!this->convar_jump_penalty)
        this->convar_jump_penalty = View::FindConVar("sv_jump_spam_penalty_time");

    this->gravity = read(this->convar_gravity, 800.f);

    // Zero is a value of these, not a missing one
    auto read_time = [&](uintptr_t data, float fallback) {
        float value = data ? p->read<float>(data + View::CONVAR_VALUE) : fallback;
        return std::isfinite(value) && value >= 0.f && value < 10.f ? value : fallback;
    };
    this->duck_interval = read_time(this->convar_duck_interval, 0.4f);
    this->jump_penalty = read_time(this->convar_jump_penalty, 1.f / 64.f);

    bool other = (this->convar_autobhop && p->read<uint8_t>(this->convar_autobhop + View::CONVAR_VALUE))
        || (this->convar_legacy_jump && p->read<uint8_t>(this->convar_legacy_jump + View::CONVAR_VALUE));

    if (other != this->other_jump) {
        LOGF(INFO, "Subtick bunny hop {}", other ? "off, this server jumps another way (sv_autobunnyhopping / sv_legacy_jump)" : "on");
        this->other_jump = other;
    }
}

int Subtick::GetSequence() {
    auto p = Engine::GetProcess();
    auto managers = p->read<uintptr_t>(Engine::GetClient().base + uc::dwManagers);
    if (!managers)
        return -1;

    // By player slot, the entity index of our controller - 1 (the game finds it so). On a server of our own we are the
    // first slot, elsewhere the first manager there can be a leftover one without commands
    constexpr std::ptrdiff_t ENTITY_IDENTITY = 0x10, IDENTITY_HANDLE = 0x10;
    constexpr int MAX_SLOTS = 64;

    auto controller = p->read<uintptr_t>(Engine::GetClient().base + offsets::localPlayerController);
    auto identity = controller ? p->read<uintptr_t>(controller + ENTITY_IDENTITY) : 0;
    int slot = identity ? static_cast<int>(p->read<uint32_t>(identity + IDENTITY_HANDLE) & 0x7FFF) - 1 : -1;

    if (slot >= 0 && slot < MAX_SLOTS) {
        if (auto manager = p->read<uintptr_t>(managers + slot * sizeof(uintptr_t))) {
            int sequence = p->read<int>(manager + uc::m_nSequence);
            if (sequence >= 0)
                return sequence;
        }
    }

    // Otherwise the one with commands
    for (int i = 0; i < MAX_SLOTS; i++) {
        auto manager = p->read<uintptr_t>(managers + i * sizeof(uintptr_t));
        int sequence = manager ? p->read<int>(manager + uc::m_nSequence) : -1;
        if (sequence >= 0)
            return sequence;
    }
    return -1;
}

int Subtick::Plan(uintptr_t slot, Event* out) {
    auto p = Engine::GetProcess();

    auto pawn = Engine::GetLocalPawn();
    if (!pawn || p->read<int>(pawn + offsets::pawn::m_iHealth) <= 0)
        return 0;

    // The player pressed or let go of something in this tick: their own. Only jumps (like jump on the mouse wheel) are
    // kept, in between ours by their time
    Event existing[MAX_EVENTS]{};
    int existing_count = p->read<int>(slot + SLOT_EVENTS);
    if (existing_count < 0 || existing_count > MAX_EVENTS)
        return 0;
    if (existing_count > 0) {
        p->read_raw(slot + SLOT_EVENT, existing, existing_count * sizeof(Event));
        for (int i = 0; i < existing_count; i++)
            if (existing[i].button != IN_JUMP)
                return 0;
    }

    auto view = p->read<Vec3_t>(slot + SLOT_VIEW);
    if (!std::isfinite(view.x) || !std::isfinite(view.y))
        return 0;

    int n = 0;
    auto add = [&](float when, uint64_t button, bool pressed, float yaw) {
        auto& e = out[n++];
        e = {};
        e.when = when;
        e.button = button;
        e.pressed = pressed;
        e.pitch = view.x;
        e.yaw = Normalize(yaw);
    };

    auto held = p->read<uint64_t>(slot + SLOT_HELD);

    // On the ground the jump is the bunny hop's. Jump events of ours there got the jumps lost
    if (p->read<uint32_t>(pawn + offsets::pawn::m_fFlags) & FL_ONGROUND)
        return 0;

    auto velocity = p->read<Vec3_t>(pawn + offsets::pawn::m_vecAbsVelocity);
    float speed = std::hypot(velocity.x, velocity.y);
    if (speed < MIN_SPEED)
        return 0;


    constexpr float DEGREES = 180.f / std::numbers::pi_v<float>;
    float flight = atan2f(velocity.y, velocity.x) * DEGREES;

    // Where to: the move keys held, from the view (W ahead, A left, D right, S back), ahead without any
    held &= MOVE_BUTTONS;
    float ahead = ((held & IN_FORWARD) ? 1.f : 0.f) - ((held & IN_BACK) ? 1.f : 0.f);
    float left = ((held & IN_MOVELEFT) ? 1.f : 0.f) - ((held & IN_MOVERIGHT) ? 1.f : 0.f);
    float target = view.y + ((ahead != 0.f || left != 0.f) ? atan2f(left, ahead) * DEGREES : 0.f);

    // The keys held stay held for the game: let go of at the start of the tick, ours move the player, pressed again at
    // the end. Their own view does not matter, nothing moves between them & the next event
    auto release_held = [&](bool pressed, float when, float yaw) {
        for (auto button : { IN_FORWARD, IN_BACK, IN_MOVELEFT, IN_MOVERIGHT })
            if (held & button)
                add(when, button, pressed, yaw);
    };

    // The game moves the player part by part, each adds up to `accel` towards the key while the speed towards it is
    // below 30. Most gain with the speed towards the key at 30 - accel, not at 30 (no gain at all there): the key that
    // far from the flight. A part bends the flight towards its side: towards the target with the same side again,
    // taking turns once there to fly straight
    const float wishspeed = this->air_max_wishspeed;
    const float accel = this->air_accelerate * MAX_SPEED * TICK_INTERVAL / SEGMENTS;
    float vx = velocity.x, vy = velocity.y;
    int side = Normalize(target - flight) > 0.f ? 0 : 1;
    bool first = true;

    for (int k = 0; k < SEGMENTS; k++) {
        float when = static_cast<float>(k) / SEGMENTS;
        float until = k + 1 < SEGMENTS ? static_cast<float>(k + 1) / SEGMENTS : LAST_WHEN;
        float speed_now = std::hypot(vx, vy);
        float flight_now = atan2f(vy, vx) * DEGREES;
        float away = acosf(std::clamp((wishspeed - accel) / speed_now, 0.f, 1.f)) * DEGREES;

        float turn = Normalize(target - flight_now);
        if (fabsf(turn) > ALIGNED)
            side = turn > 0.f ? 0 : 1;

        // A (left of the view) with its direction `away` left of the flight, D with it `away` right
        float wish = side == 0 ? flight_now + away : flight_now - away;
        float yaw = side == 0 ? wish - 90.f : wish + 90.f;

        if (first) {
            release_held(false, 0.f, yaw);
            first = false;
        }

        // The view of an event is the one the part of the tick up to it moves with: pressed & let go of with the same
        auto button = side == 0 ? IN_MOVELEFT : IN_MOVERIGHT;
        add(when, button, true, yaw);
        add(until, button, false, yaw);

        float add_speed = std::min(accel, wishspeed - speed_now * cosf(away / DEGREES));
        vx += cosf(wish / DEGREES) * add_speed;
        vy += sinf(wish / DEGREES) * add_speed;
        side ^= 1;
    }

    release_held(true, LAST_WHEN, view.y);

    if (n + existing_count > MAX_EVENTS)
        return 0;
    for (int i = 0; i < existing_count; i++)
        out[n++] = existing[i];
    SortByTime(out, n);
    return n;
}

// The landing record of the game names ticks of the tickbase: a landing inside a tick is known once the command that
// landed was run, & its A goes into that same command (the game builds it again every frame, the last one is sent).
// A landing on the boundary of a tick puts A in the next command. Before the game runs a command, where it lands is
// guessed from the map, so A is in already
Subtick::Build Subtick::UpdateBuild(uintptr_t controller, int sequence) {
    auto p = Engine::GetProcess();

    // The tickbase leads the command number by a few ticks, more online & not steadily. Prediction raises it once it
    // ran the command being built: the most of a moment back is the lead after that, one less a command not run yet
    int tickbase = p->read<int>(controller + offsets::jump::m_nTickBase);
    auto now = std::chrono::steady_clock::now();
    this->leads.emplace_back(now, tickbase - sequence);
    while (now - this->leads.front().first > LEAD_WINDOW)
        this->leads.pop_front();

    int lead = INT_MIN;
    for (const auto& [time, value] : this->leads)
        lead = std::max(lead, value);

    return { sequence + lead, tickbase - sequence < lead };
}

Subtick::JumpPlan Subtick::PlanJump(uintptr_t pawn, int sequence, const Build& current) {
    auto p = Engine::GetProcess();
    JumpPlan plan{};

    auto services = p->read<uintptr_t>(pawn + offsets::jump::m_pMovementServices);
    if (!services)
        return plan;

    int build = current.tickbase;
    bool fresh = current.fresh;
    plan.build = build;
    plan.sequence = sequence;

    auto record = services + offsets::jump::m_ModernJump;
    int landed_tick = p->read<int>(record + offsets::jump::m_nLastLandedTick);
    float landed_frac = p->read<float>(record + offsets::jump::m_flLastLandedFrac);
    bool on_ground = p->read<uint32_t>(pawn + offsets::pawn::m_fFlags) & FL_ONGROUND;
    bool falling = p->read<Vec3_t>(pawn + offsets::pawn::m_vecAbsVelocity).z < 0.f;

    // A guess the commands went past without a landing
    if (this->pending.live && build > this->pending.build)
        this->pending.live = false;

    // A record naming another tick is the next landing
    if (landed_tick > 0 && std::isfinite(landed_frac) && landed_frac >= 0.f && landed_frac <= 1.f
        && (!this->landing.valid || this->landing.tick != landed_tick)) {
        Landing opened;
        opened.valid = true;
        opened.tick = landed_tick;
        opened.frac = landed_frac;

        // The guess went out before the record & is the A of this landing
        if (this->pending.live && landed_tick + 1 == this->pending.build) {
            opened.a_placed = true;
            opened.a_build = this->pending.build;
            opened.a_when = this->pending.when;
        }

        this->pending.live = false;
        this->landing = opened;
    }
    this->landing.took_off = this->landing.took_off || !on_ground;

    const auto& l = this->landing;

    // One pair per command, the one nearest the landing first
    if (l.valid && build == l.tick + 1) {
        if (auto when = PairA(l.frac)) {
            plan.kind = JumpPlan::A;
            plan.when = *when;
        }
    }
    else if (l.valid && l.a_placed && l.a_sequence == sequence) {
        // The tickbase went up while the command of A is still being built: A stays, B is for the next one
        plan.kind = JumpPlan::A;
        plan.when = l.a_when;
        plan.build = l.a_build;
        return plan;
    }
    else if (l.valid && build >= l.tick + 2 && build <= l.tick + 2 + B_CATCH_UP
        && (build == l.tick + 2 || !l.b_placed || l.b_build == build)) {
        // A step past the later of the landing & A, clear of the one tick the game turns a second press away in
        float contact = Grid(l.frac);
        float behind = l.a_placed && l.a_when > contact ? l.a_when : contact;
        plan.kind = JumpPlan::B;
        plan.when = std::clamp(Grid(behind + STEP), FIRST, LAST);
    }

    // Not run yet & landing in it: A from where the map says the feet touch. Never a tick early, that press would
    // get the real A turned away
    if (plan.kind == JumpPlan::NONE && fresh && !on_ground && falling
        && !(l.valid && !l.took_off && l.tick + 1 == build)) {
        float contact = PredictContact(pawn);
        if (contact > 0.f && contact < 1.f && PostFits(contact)) {
            plan.kind = JumpPlan::GUESS;
            plan.when = PostContact(Grid(contact));
        }
    }

    return plan;
}

void Subtick::CommitJump(const JumpPlan& plan) {
    switch (plan.kind) {
    case JumpPlan::A:
        this->landing.a_placed = true;
        this->landing.a_build = plan.build;
        this->landing.a_when = plan.when;
        this->landing.a_sequence = plan.sequence;
        break;
    case JumpPlan::B:
        this->landing.b_placed = true;
        this->landing.b_build = plan.build;
        break;
    case JumpPlan::GUESS:
        this->pending = { true, plan.build, plan.when };
        break;
    default:
        break;
    }
}

float Subtick::PredictContact(uintptr_t pawn) {
    auto p = Engine::GetProcess();
    if (!MapCollision::IsLoaded())
        return -1.f;

    auto origin = p->read<Vec3_t>(pawn + offsets::pawn::m_vOldOrigin);
    auto velocity = p->read<Vec3_t>(pawn + offsets::pawn::m_vecAbsVelocity);
    velocity.z -= this->gravity * TICK_INTERVAL;
    if (!(velocity.z < 0.f) || !std::isfinite(origin.x) || !std::isfinite(velocity.x))
        return -1.f;

    // The feet in the middle & at the corners of the player, the first to touch ground it can stand on
    auto move = velocity * TICK_INTERVAL;
    const float corners[5][2] = { { 0.f, 0.f }, { -HULL_EDGE, -HULL_EDGE }, { -HULL_EDGE, HULL_EDGE }, { HULL_EDGE, -HULL_EDGE }, { HULL_EDGE, HULL_EDGE } };

    float best = 2.f;
    for (const auto& corner : corners) {
        Vec3_t start{ origin.x + corner[0], origin.y + corner[1], origin.z };
        MapCollision::Hit hit;
        if (MapCollision::Trace(start, start + move, hit) && hit.normal.z >= STANDABLE && hit.fraction > 0.f)
            best = std::min(best, hit.fraction);
    }

    return best < 1.f ? best : -1.f;
}

Subtick::Motion Subtick::ReadMotion(uintptr_t pawn) {
    auto p = Engine::GetProcess();
    Motion m;

    auto services = p->read<uintptr_t>(pawn + offsets::jump::m_pMovementServices);
    if (!services)
        return m;

    m.origin = p->read<Vec3_t>(pawn + offsets::pawn::m_vOldOrigin);
    m.velocity = p->read<Vec3_t>(pawn + offsets::pawn::m_vecAbsVelocity);
    m.flags = p->read<uint32_t>(pawn + offsets::pawn::m_fFlags);
    m.duck_speed = p->read<float>(services + offsets::jump::m_flDuckSpeed);
    m.last_duck_time = p->read<float>(services + offsets::jump::m_flLastDuckTime);
    m.last_jump_tick = p->read<int>(services + offsets::jump::m_nLastJumpTick);
    m.jump_frac = p->read<float>(services + offsets::jump::m_flLastJumpFrac);
    m.jump_height = p->read<float>(services + offsets::jump::m_flHeightAtJumpStart);
    m.jump_vz = p->read<float>(services + offsets::jump::m_flLastJumpVelocityZ);
    m.landed_tick = p->read<int>(services + offsets::jump::m_ModernJump + offsets::jump::m_nLastLandedTick);
    m.landed_frac = p->read<float>(services + offsets::jump::m_ModernJump + offsets::jump::m_flLastLandedFrac);
    m.fall_velocity = p->read<float>(services + offsets::jump::m_flFallVelocity);

    float scale = p->read<float>(pawn + offsets::jump::m_flGravityScale);
    m.gravity_scale = std::isfinite(scale) && scale > 0.f ? scale : 1.f;

    m.valid = std::isfinite(m.origin.x) && std::isfinite(m.velocity.x) && std::isfinite(m.duck_speed) && std::isfinite(m.last_duck_time);
    return m;
}

// Decided once per command from where the player is at its start, like the game runs it. Ours go after the events the
// game has in the tick so far
std::vector<Subtick::Event> Subtick::PlanBug(uintptr_t slot, int sequence, const Build& build, bool jump_held, bool duck_held) {
    auto p = Engine::GetProcess();

    if (sequence == this->bug_sequence)
        return this->bug_steps;

    // Not known yet where this command starts: again on the next look
    auto start = this->starts.find(sequence);
    if (start == this->starts.end()) {
        this->bug_debug.no_start++;
        return {};
    }

    const auto m = start->second;
    this->bug_sequence = sequence;
    this->bug_steps.clear();

    // Once per fall: a new fall after landing or after a jump of the game
    bool grounded = m.flags & FL_ONGROUND;
    if (grounded || (this->bug_attempt_jump_tick >= 0 && m.last_jump_tick != this->bug_attempt_jump_tick))
        this->bug_attempted = false;

    // Landed: why the commands of the fall had no jump bug
    auto& debug = this->bug_debug;
    if (grounded) {
        if (debug.commands > 0) {
            LOGF(VERBOSE, "Jump bug, fall of {} commands: not started {}, jump not held {}, duck held {}, tried {}, duck not ready {}, "
                "no ground near {}, rejected {}, map {}", debug.commands, debug.no_start, debug.not_held, debug.duck_held,
                debug.attempted, debug.duck_not_ready, debug.no_ground, debug.rejected, MapCollision::IsLoaded() ? "loaded" : "NOT loaded");

            std::string trail;
            for (const auto& entry : debug.last)
                trail += entry + " | ";
            LOGF(VERBOSE, "Jump bug, last commands: {}landed at {:.1f} {:.1f} z {:.2f} vz {:.1f}, map {}", trail,
                m.origin.x, m.origin.y, m.origin.z, m.velocity.z, MapCollision::GetStatus());
        }
        debug = {};
    }
    else {
        debug.commands++;
    }

    bool owed = this->bug_across == sequence;
    this->bug_across = -1;

    // Jump held, not ducking themselves
    if (!jump_held || duck_held) {
        if (!grounded)
            (!jump_held ? debug.not_held : debug.duck_held)++;
        return {};
    }

    int count = p->read<int>(slot + SLOT_EVENTS);
    if (count < 0 || count > MAX_EVENTS)
        return {};

    Event events[MAX_EVENTS]{};
    if (count > 0)
        p->read_raw(slot + SLOT_EVENT, events, count * sizeof(Event));

    // Ours go after the other keys of the tick. Not after the move keys (the air strafe takes them up to the end of the
    // tick, the player taps them): they leave the duck & jump alone, sorted in between they move the same
    float floor = 0.f;
    for (int i = 0; i < count; i++) {
        uint64_t button = events[i].button;
        if (button == 0 || (button & (MOVE_BUTTONS | IN_JUMP | IN_DUCK)))
            continue;
        floor = std::max(floor, events[i].when);
    }
    size_t room = MAX_EVENTS - count;

    std::optional<BugPlan> plan;
    if (owed) {
        plan = BugPlan{ BugPlan::ACROSS_JUMP };
    }
    else if (!grounded && !this->bug_attempted) {
        // The duck has to be allowed again (sv_timebetweenducks) & fast enough to be done in time
        float now = (build.tickbase - 1) * TICK_INTERVAL;
        bool duck_ready = m.duck_speed >= MIN_DUCK_SPEED && ((m.flags & FL_DUCKING) || now >= m.last_duck_time + this->duck_interval);

        // The drawn origin & velocity are a tick or two behind, by how the frame falls: too rough for the height. The
        // game predicted the jump of this fall: from where, when & how fast up. The command of tickbase B is the tick
        // B - 1 of those. Across the drawn origin is near enough, on by the time it is behind
        float gravity = this->gravity * m.gravity_scale;
        Vec3_t origin = m.origin + m.velocity * (DRAWN_LAG * TICK_INTERVAL);
        Vec3_t velocity = m.velocity;

        double jumped = m.last_jump_tick + (double)m.jump_frac;
        double landed = m.landed_tick + (double)m.landed_frac;
        float since = (float)((build.tickbase - 1) - jumped) * TICK_INTERVAL;
        bool topped = gravity > 0.f && std::isfinite(m.jump_frac) && std::isfinite(m.jump_height) && std::isfinite(m.jump_vz)
            && m.jump_vz > 0.f && jumped > landed && since >= 0.f && since < 10.f;
        if (topped) {
            origin.z = m.jump_height + m.jump_vz * since - 0.5f * gravity * since * since;
            velocity.z = m.jump_vz - gravity * since;
        }

        if (topped) {
            this->lag_samples.push_back({ build.tickbase, origin, velocity, gravity });
            if (this->lag_samples.size() > 16)
                this->lag_samples.pop_front();
        }

        // Without the duck check, so the log says which one stood in the way
        auto contact = topped ? PredictBugContact(origin, velocity, gravity) : std::nullopt;

        // Where the map has ground straight under the player, however far
        MapCollision::Hit floor_hit;
        std::string floor_z = MapCollision::Trace(origin, origin - Vec3_t(0.f, 0.f, 300.f), floor_hit)
            ? std::format("{:.2f}", origin.z - 300.f * floor_hit.fraction) : std::string("none");

        debug.last.push_back(std::format("{} at {:.1f} {:.1f} z {:.2f} (drawn {:.2f}, jump {} + {:.3f} from {:.2f} at {:.1f}, fall speed {:.1f}) "
            "vz {:.1f} ground {} (map floor z {})", sequence, origin.x, origin.y, origin.z, m.origin.z, m.last_jump_tick,
            m.jump_frac, m.jump_height, m.jump_vz, m.fall_velocity, velocity.z,
            contact ? std::format("{:.2f} ticks", contact->ticks) : std::string("none"), floor_z));
        if (debug.last.size() > 4)
            debug.last.pop_front();

        if (!contact)
            debug.no_ground++;
        else if (!duck_ready)
            debug.duck_not_ready++;
        if (contact && duck_ready && (velocity.z < 0.f || contact->drop > 0.f)) {
            if (contact->ticks >= 1.f)
                plan = PlanAcross(*contact, floor);
            if (!plan)
                plan = PlanInTick(*contact, floor, this->jump_penalty, BUG_CLEARANCE);
        }

        // Debug: how high above the ground it stands up, to tell against the result
        if (plan && plan->shape != BugPlan::ACROSS_JUMP) {
            float stand = plan->shape == BugPlan::IN_TICK ? plan->stand : 1.f;
            this->bug_result = { true, build.tickbase, m.landed_tick, plan->shape == BugPlan::IN_TICK,
                (contact->ticks - stand) * contact->fall, stand - plan->duck, stand };
        }

        // Close to the ground: what was decided & why
        if (contact) {
            LOGF(VERBOSE, "Jump bug check, command {}: ground in {:.3f} ticks, fall {:.2f}/tick, z {:.2f}, vz {:.1f}, duck ready {} "
                "(speed {:.2f}, last {:.3f}, now {:.3f}), events {} up to {:.3f} -> {}", sequence, contact->ticks, contact->fall,
                origin.z, velocity.z, duck_ready, m.duck_speed, m.last_duck_time, now, count, floor,
                plan ? (plan->shape == BugPlan::IN_TICK ? "in tick" : "across") : "no plan");

            if (duck_ready && !plan)
                debug.rejected++;
        }
    }
    else if (!grounded) {
        debug.attempted++;
    }

    if (!plan || plan->Steps() > room)
        return {};

    if (plan->shape != BugPlan::ACROSS_JUMP) {
        this->bug_attempted = true;
        this->fall_attempted = true;
        this->bug_attempt_jump_tick = m.last_jump_tick;
    }
    if (plan->shape == BugPlan::ACROSS)
        this->bug_across = sequence + 1;

    auto push = [&](float when, uint64_t button, bool pressed) {
        Event e{};
        e.when = when;
        e.button = button;
        e.pressed = pressed;
        this->bug_steps.push_back(e);
    };

    switch (plan->shape) {
    case BugPlan::IN_TICK:
        push(plan->duck, IN_DUCK, true);
        push(plan->stand, IN_DUCK, false);
        push(plan->stand, IN_JUMP, false);
        push(plan->stand, IN_JUMP, true);
        if (plan->landing_edge)
            push(plan->landing, IN_JUMP, false);
        push(plan->landing, IN_JUMP, true);
        break;
    case BugPlan::ACROSS:
        push(plan->duck, IN_DUCK, true);
        push(1.f, IN_DUCK, false);
        break;
    case BugPlan::ACROSS_JUMP:
        push(0.f, IN_JUMP, false);
        push(0.f, IN_JUMP, true);
        break;
    }

    return this->bug_steps;
}

// The game writes down when it landed (tick & part of it). The path we worked out for the last command before, followed
// down to the ground of the map, against it. Not after a jump bug: the duck moves the feet & the landing with them
void Subtick::MeasureLag(uintptr_t pawn) {
    auto p = Engine::GetProcess();

    auto services = p->read<uintptr_t>(pawn + offsets::jump::m_pMovementServices);
    if (!services)
        return;

    auto record = services + offsets::jump::m_ModernJump;
    int landed_tick = p->read<int>(record + offsets::jump::m_nLastLandedTick);
    float landed_frac = p->read<float>(record + offsets::jump::m_flLastLandedFrac);

    if (landed_tick == this->lag_landing_tick || !std::isfinite(landed_frac))
        return;

    bool first = this->lag_landing_tick < 0 || this->fall_attempted;
    this->lag_landing_tick = landed_tick;
    this->fall_attempted = false;

    // The command of tickbase B is the tick B - 1 of the landing (& of the jump). The last command that started before it
    double landing = landed_tick + (double)landed_frac;
    const LagSample* sample = nullptr;
    for (const auto& s : this->lag_samples)
        if (s.build - 1 <= landing && s.build >= landed_tick - 6 && (!sample || s.build > sample->build))
            sample = &s;

    if (first || !sample) {
        this->lag_samples.clear();
        return;
    }

    MapCollision::Hit hit;
    if (!MapCollision::Trace(sample->origin, sample->origin - Vec3_t(0.f, 0.f, 400.f), hit) || hit.normal.z < STANDABLE) {
        this->lag_samples.clear();
        return;
    }
    float floor_z = sample->origin.z - 400.f * hit.fraction;

    // z + vz * t - g / 2 * t^2 = floor, in ticks
    float a = -0.5f * sample->gravity * TICK_INTERVAL * TICK_INTERVAL;
    float b = sample->velocity.z * TICK_INTERVAL;
    float c = sample->origin.z - floor_z;
    float disc = b * b - 4.f * a * c;

    if (c >= 0.f && disc >= 0.f) {
        float ticks = (-b - std::sqrt(disc)) / (2.f * a);
        float measured = (float)((sample->build - 1) + ticks - landing);

        // Only a check: the jump the game predicted gives the landing to the 1/64, off only where the ground under the
        // middle is not the ground landed on (an edge, a slope)
        if (std::isfinite(measured) && std::abs(measured) <= 3.f) {
            LOGF(VERBOSE, "Jump bug, landing worked out {:+.3f} ticks off (command {}, top {:.2f})", measured, sample->build,
                sample->origin.z + sample->velocity.z * sample->velocity.z / (2.f * sample->gravity));
        }
    }

    this->lag_samples.clear();
}

bool Subtick::PlaceSteps(uintptr_t slot, const std::vector<Event>& steps, uint64_t owned, bool fix_order, bool& wrote) {
    auto p = Engine::GetProcess();
    wrote = false;

    int count = p->read<int>(slot + SLOT_EVENTS);
    if (count < 0 || count > MAX_EVENTS)
        return false;

    Event events[MAX_EVENTS]{};
    if (count > 0)
        p->read_raw(slot + SLOT_EVENT, events, count * sizeof(Event));

    auto same = [](const Event& a, const Event& b) {
        return a.when == b.when && a.button == b.button && a.pressed == b.pressed;
    };

    // Each of a list in the events, none counted twice
    auto find_all = [&](const std::vector<Event>& list, bool* used) {
        for (const auto& e : list) {
            bool found = false;
            for (int i = 0; i < count && !found; i++) {
                if (!used[i] && same(events[i], e)) {
                    used[i] = true;
                    found = true;
                }
            }
            if (!found)
                return false;
        }
        return true;
    };

    // Keys the game adds after ours (the player) come with the time they were pressed, often before ours. Out of
    // order the game drops every event of the tick, ours with them
    bool in_order = true;
    for (int i = 1; i < count; i++)
        in_order = in_order && events[i].when >= events[i - 1].when;

    // The game builds the command again from these every frame, ours stay until the tick ends
    bool there = false;
    if (this->placed.size() == steps.size() && std::equal(steps.begin(), steps.end(), this->placed.begin(), same)) {
        bool used[MAX_EVENTS]{};
        there = find_all(steps, used);
    }

    if (there && (in_order || !fix_order))
        return true;

    Event out[MAX_EVENTS]{};
    int n = 0;

    if (there) {
        // Only the order
        for (int i = 0; i < count; i++)
            out[n++] = events[i];
    }
    else {
        // Without the ones we put there before, & the keys ours take over
        bool used[MAX_EVENTS]{};
        find_all(this->placed, used);
        for (int i = 0; i < count; i++) {
            if (used[i] || (events[i].button & owned))
                continue;
            out[n++] = events[i];
        }

        // Past 32 the game drops every event of the tick
        if (n + static_cast<int>(steps.size()) > MAX_EVENTS)
            return false;

        // The view of an event is the one the part of the tick up to it moves with: the one of the event after it
        auto view = p->read<Vec3_t>(slot + SLOT_VIEW);
        int base = n;
        for (auto e : steps) {
            e.pitch = view.x;
            e.yaw = view.y;
            for (int i = 0; i < base; i++) {
                if (out[i].when >= e.when) {
                    e.pitch = out[i].pitch;
                    e.yaw = out[i].yaw;
                    break;
                }
            }
            out[n++] = e;
        }
    }

    // Stable: a release stays right before its press, keys of the game keep their own order
    if (fix_order || !there)
        SortByTime(out, n);

    // The game added an event meanwhile: again on the next look
    if (p->read<int>(slot + SLOT_EVENTS) != count)
        return false;

    uint64_t buttons = 0;
    for (const auto& e : steps)
        buttons |= e.button;

    p->write_bytes(slot + SLOT_EVENT, std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(out), reinterpret_cast<const uint8_t*>(out) + n * sizeof(Event)));
    p->write<int>(slot + SLOT_EVENTS, n);
    p->write<uint64_t>(slot + SLOT_PRESSED, p->read<uint64_t>(slot + SLOT_PRESSED) | buttons);
    p->write<uint64_t>(slot + SLOT_RELEASED, p->read<uint64_t>(slot + SLOT_RELEASED) | buttons);

    this->placed = steps;
    wrote = true;
    return true;
}

void Subtick::Thread() {
    auto next_convar_read = std::chrono::steady_clock::now();

    // The tick our events went into, & whether they had to go in again (the tick started right after we wrote)
    int done_sequence = -1;
    bool written = false, rewritten = false;

    // Ours in the tick, to put the events the game adds after them in order
    Event ours[MAX_EVENTS]{};
    int our_count = 0;

    // The events first, their count last: the game reads the count first. Then the buttons as pressed & let go of
    auto write = [](uintptr_t slot, const Event* plan, int count) {
        auto p = Engine::GetProcess();
        uint64_t buttons = 0;
        for (int i = 0; i < count; i++)
            buttons |= plan[i].button;

        p->write_bytes(slot + SLOT_EVENT, std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(plan), reinterpret_cast<const uint8_t*>(plan) + count * sizeof(Event)));
        p->write<int>(slot + SLOT_EVENTS, count);
        p->write<uint64_t>(slot + SLOT_PRESSED, p->read<uint64_t>(slot + SLOT_PRESSED) | buttons);
        p->write<uint64_t>(slot + SLOT_RELEASED, p->read<uint64_t>(slot + SLOT_RELEASED) | buttons);
    };

    while (!this->stopping) {
        std::this_thread::sleep_for(POLL);

        auto p = Engine::GetProcess();
        bool playing = p && Movement::IsPlaying();
        bool space = GetAsyncKeyState(VK_SPACE) & 0x8000;
        bool ducking = GetAsyncKeyState(VK_CONTROL) & 0x8000;

        // Air strafe & jump bug by their keys: toggled, held or always, like third person
        this->strafe_on = this->available && cfg::misc::auto_strafe
            && IsKeyOn(cfg::misc::air_strafe_mode, cfg::misc::air_strafe_key, this->strafe_key);
        this->bug_on = this->available && cfg::misc::jump_bug
            && IsKeyOn(cfg::misc::jump_bug_mode, cfg::misc::jump_bug_key, this->bug_key);

        bool strafing = playing && this->strafe_on;
        bool want_hop = playing && cfg::misc::bhop;
        bool hopping = want_hop && space && HandlesBhop();
        bool bugging = playing && this->bug_on && !this->other_jump;

        if (this->bug_on != this->bug_was_on) {
            this->bug_was_on = this->bug_on;
            LOGF(VERBOSE, "Jump bug {}{}", this->bug_was_on ? "on" : "off", this->bug_was_on && this->other_jump
                ? ", but this server jumps another way (sv_autobunnyhopping / sv_legacy_jump)" : "");

            // Where the player is by both origins, & the ground of the map under them
            if (auto local = this->bug_was_on && p ? Engine::GetLocalPawn() : 0) {
                auto old_origin = p->read<Vec3_t>(local + offsets::pawn::m_vOldOrigin);
                auto node = p->read<uintptr_t>(local + offsets::pawn::m_pGameSceneNode);
                auto node_origin = node ? p->read<Vec3_t>(node + offsets::bomb::m_vecAbsOrigin) : Vec3_t{};

                MapCollision::Hit floor_hit;
                auto from = old_origin + Vec3_t(0.f, 0.f, 40.f);
                std::string floor_z = MapCollision::Trace(from, from - Vec3_t(0.f, 0.f, 340.f), floor_hit)
                    ? std::format("{:.2f}", from.z - 340.f * floor_hit.fraction) : std::string("none");

                LOGF(VERBOSE, "Jump bug, player: old origin {:.2f} {:.2f} {:.2f}, scene node {:.2f} {:.2f} {:.2f}, on ground {}, "
                    "map floor under z {} ({})", old_origin.x, old_origin.y, old_origin.z, node_origin.x, node_origin.y, node_origin.z,
                    (p->read<uint32_t>(local + offsets::pawn::m_fFlags) & FL_ONGROUND) != 0, floor_z, MapCollision::GetStatus());
            }
        }

        // Also for the jumps alone: the jump of the server decides whether they are ours
        auto now = std::chrono::steady_clock::now();
        if ((strafing || want_hop || bugging) && now >= next_convar_read) {
            next_convar_read = now + CONVAR_READ;
            ReadConVars();
        }

        if (!strafing)
            done_sequence = -1;

        if (!hopping && !bugging) {
            this->own_sequence = -1;
            this->placed.clear();
        }

        if (!strafing && !hopping && !bugging) {
            std::this_thread::sleep_for(20ms);
            continue;
        }

        auto input = p->read<uintptr_t>(Engine::GetClient().base + offsets::input::dwCSGOInput);
        int sequence = input ? GetSequence() : -1;
        if (sequence < 0)
            continue;

        auto slot = input + SLOT;

        // The jumps first, on every look: the strafe of a new tick keeps jumps that are there
        auto pawn = (hopping || bugging) ? Engine::GetLocalPawn() : 0;
        auto controller = pawn ? p->read<uintptr_t>(Engine::GetClient().base + offsets::localPlayerController) : 0;

        if (controller && p->read<int>(pawn + offsets::pawn::m_iHealth) > 0) {
            // A new pawn (respawn, reconnect): what we knew of the old one answers nothing
            if (pawn != this->jump_pawn) {
                this->jump_pawn = pawn;
                this->landing = {};
                this->pending = {};
                this->leads.clear();
                this->starts.clear();
                this->bug_attempted = false;
                this->bug_attempt_jump_tick = -1;
                this->bug_across = -1;
                this->bug_sequence = -1;
                this->lag_samples.clear();
                this->lag_landing_tick = -1;
            }

            auto build = UpdateBuild(controller, sequence);

            if (sequence != this->own_sequence) {
                // The jump bug command went out with these events: what the player did in the commands after
                if (this->bug_seen_sequence >= 0) {
                    std::string list;
                    for (const auto& e : this->bug_seen)
                        list += std::format(" | {:.4f} {:#x} {}", e.when, e.button, e.pressed ? "down" : "up");
                    LOGF(VERBOSE, "Jump bug, command {} went with {} events{}", this->bug_seen_sequence, this->bug_seen.size(), list);
                    this->bug_seen_sequence = -1;
                    this->bug_follow = 4;
                }
                if (this->bug_follow > 0) {
                    this->bug_follow--;
                    auto services = p->read<uintptr_t>(pawn + offsets::jump::m_pMovementServices);
                    if (services) {
                        auto record = services + offsets::jump::m_ModernJump;
                        uint32_t flags = p->read<uint32_t>(pawn + offsets::pawn::m_fFlags);
                        LOGF(VERBOSE, "Jump bug, after: command {} (tickbase {}) on ground {} ducking {} eyes {:.1f}, drawn z {:.2f} vz {:.1f}, "
                            "last jump {}, landed {} + {:.4f}", sequence, build.tickbase, (flags & FL_ONGROUND) != 0, (flags & FL_DUCKING) != 0,
                            p->read<Vec3_t>(pawn + offsets::pawn::m_vecViewOffset).z, p->read<Vec3_t>(pawn + offsets::pawn::m_vOldOrigin).z,
                            p->read<Vec3_t>(pawn + offsets::pawn::m_vecAbsVelocity).z, p->read<int>(services + offsets::jump::m_nLastJumpTick),
                            p->read<int>(record + offsets::jump::m_nLastLandedTick), p->read<float>(record + offsets::jump::m_flLastLandedFrac));
                    }
                }

                // Debug: a few ticks on, whether it jumped without landing
                auto& result = this->bug_result;
                if (result.live && build.tickbase >= result.tickbase + 3) {
                    result.live = false;
                    auto services = p->read<uintptr_t>(pawn + offsets::jump::m_pMovementServices);
                    if (services) {
                        int landed = p->read<int>(services + offsets::jump::m_ModernJump + offsets::jump::m_nLastLandedTick);
                        int jumped = p->read<int>(services + offsets::jump::m_nLastJumpTick);
                        bool landed_now = landed != result.landed_before && landed >= result.tickbase - 1 && landed <= result.tickbase + 1;
                        bool jumped_now = jumped >= result.tickbase - 1 && jumped <= result.tickbase;
                        // The game writes down when the duck was let go: against when we meant it, 0 when in the
                        // command we meant, -1 a command early
                        float stood = p->read<float>(services + offsets::jump::m_flLastDuckTime) / TICK_INTERVAL;
                        float meant = (result.tickbase - 1) + result.stand;
                        LOGF(VERBOSE, "Jump bug RESULT {}: {}, stood up {:.2f} above the ground after {:.3f} ticks ducked, "
                            "stood up {:+.2f} ticks off", landed_now ? "FAIL" : jumped_now ? "OK" : "unknown",
                            result.in_tick ? "in tick" : "across", result.clearance, result.ducked, stood - meant);
                    }
                }

                this->own_sequence = sequence;
                this->placed.clear();
            }

            // Where the player is at the start of a command: now when the game did not run it yet, else the next one
            if (bugging) {
                auto motion = ReadMotion(pawn);
                if (motion.valid)
                    this->starts[build.fresh ? sequence : sequence + 1] = motion;
                while (!this->starts.empty() && this->starts.begin()->first < sequence - 2)
                    this->starts.erase(this->starts.begin());
            }

            // The jump bug takes the jump & duck of its command, the bunny hop the jump of the others
            std::vector<Event> steps;
            uint64_t owned = 0;
            if (bugging) {
                MeasureLag(pawn);
                steps = PlanBug(slot, sequence, build, space, ducking);
                if (!steps.empty())
                    owned = IN_JUMP | IN_DUCK;
            }

            JumpPlan plan{};
            if (hopping) {
                plan = PlanJump(pawn, sequence, build);
                if (steps.empty() && plan.kind != JumpPlan::NONE) {
                    for (bool pressed : { false, true }) {
                        Event e{};
                        e.when = plan.when;
                        e.button = IN_JUMP;
                        e.pressed = pressed;
                        steps.push_back(e);
                    }
                }
            }

            // With the subtick strafe on, its merge puts the events of the game in order its own way
            bool wrote = false;
            if (!steps.empty() && PlaceSteps(slot, steps, owned, !strafing, wrote) && !owned)
                CommitJump(plan);

            // The events of the jump bug command as they stand now, logged once it went
            if (owned) {
                int count = p->read<int>(slot + SLOT_EVENTS);
                if (count >= 0 && count <= MAX_EVENTS) {
                    this->bug_seen.resize(count);
                    if (count > 0)
                        p->read_raw(slot + SLOT_EVENT, this->bug_seen.data(), count * sizeof(Event));
                    this->bug_seen_sequence = sequence;
                }
            }

            // What the slot holds is ours once we wrote it, the strafe puts later events of the game after it
            if (wrote) {
                if (owned) {
                    LOGF(VERBOSE, "Jump bug: {} steps from {:.4f} to {:.4f} in command {} (tickbase {})", steps.size(),
                        steps.front().when, steps.back().when, sequence, build.tickbase);
                }
                else {
                    const char* kinds[] = { "-", "A", "B", "guess" };
                    LOGF(VERBOSE, "Jump {} at {:.4f} in command {} (tickbase {}), landed {} + {:.4f}", kinds[plan.kind],
                        plan.when, sequence, plan.build, this->landing.tick, this->landing.frac);
                }

                int count = p->read<int>(slot + SLOT_EVENTS);
                if (done_sequence == sequence && count >= 0 && count <= MAX_EVENTS) {
                    p->read_raw(slot + SLOT_EVENT, ours, count * sizeof(Event));
                    our_count = count;
                }
            }
        }

        if (!strafing)
            continue;

        if (sequence == done_sequence) {
            int count_now = p->read<int>(slot + SLOT_EVENTS);

            // Events of the game after ours
            if (our_count > 0 && count_now > our_count && count_now <= MAX_EVENTS) {
                Event events[MAX_EVENTS]{};
                p->read_raw(slot + SLOT_EVENT, events, count_now * sizeof(Event));

                // Still ours in front, not a tick that started meanwhile
                if (memcmp(events, ours, our_count * sizeof(Event)) == 0) {
                    Event out[MAX_EVENTS]{};
                    int n = Merge(ours, our_count, events + our_count, count_now - our_count,
                        p->read<uint64_t>(slot + SLOT_HELD) & MOVE_BUTTONS, out, MAX_EVENTS);

                    // What the slot holds is ours from now on, later events of the game come after it
                    if (n > 0 && p->read<int>(slot + SLOT_EVENTS) == count_now) {
                        p->write_bytes(slot + SLOT_EVENT, std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(out), reinterpret_cast<const uint8_t*>(out) + n * sizeof(Event)));
                        p->write<int>(slot + SLOT_EVENTS, n);
                        memcpy(ours, out, n * sizeof(Event));
                        our_count = n;
                    } else if (n == 0) {
                        memcpy(ours, events, count_now * sizeof(Event));
                        our_count = count_now;
                    }
                }
            }

            // The tick started right after we wrote: the game emptied the events, ours go in again once
            if (written && !rewritten && count_now == 0) {
                Event plan[MAX_EVENTS]{};
                int count = Plan(slot, plan);
                our_count = 0;
                if (count > 0) {
                    write(slot, plan, count);
                    memcpy(ours, plan, sizeof(plan));
                    our_count = count;
                }
                rewritten = true;
            }
            continue;
        }

        // A new tick: once, at its start
        done_sequence = sequence;
        written = rewritten = false;
        our_count = 0;

        Event plan[MAX_EVENTS]{};
        int count = Plan(slot, plan);
        if (count <= 0)
            continue;

        write(slot, plan, count);
        memcpy(ours, plan, sizeof(plan));
        our_count = count;
        written = true;
    }
}

void Subtick::Shutdown() {
    GetInstance().stopping = true;
}
