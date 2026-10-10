#pragma once
#include "core/memory/Memory.hpp"
#include "core/engine/types/Vec3.hpp"

// The camera away from our player: flying on its own (free cam) or following another player (spectate).
//
// The game decides where the camera is each frame in ClientModeCSNormal::OverrideView(CViewSetup*). The vtable of the
// client mode is copied into our memory with that entry going through a stub: it runs the real function, then puts our
// position & angles into the view. Following a player, the stub reads their position itself, the one of that frame.
// Dead, the spectator camera of the game moves the view again after it: that call is skipped while ours is on, the
// game then follows the player in third person so the weapon in their hands is not drawn in front of our camera
class Freecam {
public:
    ~Freecam()                         = default;
    Freecam(const Freecam&)            = delete;
    Freecam(Freecam&&)                 = delete;
    Freecam& operator=(const Freecam&) = delete;
    Freecam& operator=(Freecam&&)      = delete;

    static bool Init();
    static bool IsAvailable();
    // After death too, the spectator camera of the game was found
    static bool IsDeadAvailable();

    // Puts the real vtable back
    static void Shutdown();

    // A key from the keyboard (from the keyboard hook of Movement): true keeps it from the game, the camera uses it
    static bool OnKey(DWORD vk, bool down);

    enum class Mode : uint32_t { Off = 0, Free = 1, Follow = 2 };

    static Mode GetMode();
    // The pawn followed, 0 when not following
    static uintptr_t GetTarget();
    static bool IsFirstPerson();
    // We are dead & watch like casual
    static bool IsDeadCamera();
private:
    Freecam() {};

    static Freecam& GetInstance()
    {
        static Freecam i{};
        return i;
    }

    bool InitImpl();
    bool Install();
    void Thread();
    void Update(float seconds);

    void SetMode(Mode mode);
    void UpdateFree(float seconds);
    void UpdateFollow(float seconds);
    bool PickTarget(int step);  // Next (1) or previous (-1) player, 0 for the first one

    // Dead: on by itself, its own keys
    void StartDead(uintptr_t pawn);
    void EndDead();
    void UpdateDead(uintptr_t pawn, bool playing, float seconds);
    void StartFree(const Vec3_t& from, const Vec3_t& look);
    Vec3_t CameraPosition();    // Where the camera is now
    Vec3_t CameraAngles();      // & where it looks
    void WriteFov();            // Of our camera, for the stub
    float fov_written = -1.f;
    void WritePunch(bool alive);    // View punch scale for the stub, alive in first person only
    bool punch_on = false;
    float punch_scale = 1.f;
    std::chrono::steady_clock::time_point punch_logged{};
    void PatchObserverView(bool patched);
    static uint32_t HandleOf(uintptr_t entity);

    // The call of the spectator camera of the game, through our stub that sets the position after it
    static bool PatchCall(uintptr_t call, uintptr_t target);
    void PatchObserverCall(bool patched);
    uintptr_t observer_view = 0;        // The spectator camera of the game, 0 when its call can't go through us
    uintptr_t settled_target = 0;
    std::chrono::steady_clock::time_point settle_until{};
    bool observer_call_patched = false;

    // Clicks while dead switch the player, kept from the game: it would switch to a teammate on the server
    void MouseThread();
    static LRESULT CALLBACK MouseHook(int code, WPARAM wparam, LPARAM lparam);

    // Keys the camera takes while it is on, W S A D space ctrl shift
    enum Key { KEY_FORWARD, KEY_BACK, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_FAST, KEY_COUNT };
    bool Held(Key key) const { return this->held[key]; }
    bool Pressed(Key key) const { return this->held[key] && !this->was_held[key]; }

private:
    uintptr_t page = 0;         // Stub, its data & the vtable copy
    uintptr_t object = 0;       // The client mode
    uintptr_t vtable = 0;       // Its real vtable
    uintptr_t copy = 0;         // Ours
    std::atomic<bool> installed = false;
    std::atomic<bool> stopping = false;

    std::atomic<Mode> mode = Mode::Off;
    std::atomic<bool> blocking = false;     // Keys go to the camera
    std::atomic<bool> held[KEY_COUNT]{};
    bool was_held[KEY_COUNT]{};
    bool freecam_key_was_down = false;

    uintptr_t local_pawn = 0;   // The camera turns off with a new pawn
    bool local_alive = false;

    std::atomic<bool> dead_camera = false;
    bool observer_patched = false;
    uintptr_t observer_services = 0;    // Of our pawn while dead, its mode set to third person
    uint8_t observer_mode = 0;          // What it was, put back
    uint32_t observer_target = 0;

    // cl_obs_interp_enable: the spectator camera flies from one player to the next. Across the map that shook
    // for a moment, off while we choose who to watch
    uintptr_t interp_convar = 0;
    uint8_t interp_value = 1;
    bool interp_changed = false;
    std::atomic<uint32_t> left_clicks = 0, right_clicks = 0;
    uint32_t left_handled = 0, right_handled = 0;
    bool left_held_back = false, right_held_back = false;  // Their release is kept from the game too
    Vec3_t position{};          // Free cam
    Vec3_t free_angles{};
    std::atomic<int> mouse_dx = 0, mouse_dy = 0;    // Counts the hook kept from the game
    uintptr_t sensitivity_convar = 0;

    // The spectator keys of the game, sending nothing while we choose who to watch
    void PatchBinds(bool patched);
    bool binds_patched = false;
    bool binds_filter = false;  // Our filter in the page, else all of them are taken

    // The mouse & key presses of the game, off in SDL (SDL_SetEventEnabled) while the free cam is on: the game reads
    // the mouse in ways the hook does not keep from it. Our hooks still see them
    bool SetupGameInput();
    void BlockGameInput(bool blocked, bool force = false);
    uintptr_t input_code = 0;
    bool input_blocked = false;

    std::atomic<uintptr_t> target = 0;
    std::atomic<bool> first_person = false;
    float distance = 100.f;

    // Where the one followed looks: the game gets it once a tick, moved between the ticks here
    Vec3_t SmoothAngles(const Vec3_t& raw);
    uintptr_t smooth_target = 0;
    Vec3_t angles_from{}, angles_to{}, angles_shown{};
    std::chrono::steady_clock::time_point time_from{}, time_to{};
    float tick = 1.f / 64.f;
};
