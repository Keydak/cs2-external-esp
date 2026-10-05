#include "Subtick.hpp"

#include "core/engine/Engine.hpp"
#include "core/features/Movement.hpp"
#include "core/features/View.hpp"
#include "core/offsets/Offsets.hpp"
#include "core/engine/types/Vec3.hpp"

#include <numbers>

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
        bool enabled = p && cfg::misc::auto_strafe && cfg::misc::auto_strafe_mode == 1 && Movement::IsPlaying()
            && (!cfg::misc::auto_strafe_space || (GetAsyncKeyState(VK_SPACE) & 0x8000));

        if (!enabled) {
            done_sequence = -1;
            std::this_thread::sleep_for(20ms);
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= next_convar_read) {
            next_convar_read = now + CONVAR_READ;
            ReadConVars();
        }

        auto input = p->read<uintptr_t>(Engine::GetClient().base + offsets::input::dwCSGOInput);
        int sequence = input ? GetSequence() : -1;
        if (sequence < 0)
            continue;

        auto slot = input + SLOT;

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
