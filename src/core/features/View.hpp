#pragma once
#include "core/memory/Memory.hpp"

class View {
public:
    ~View()                      = default;
    View(const View&)            = delete;
    View(View&&)                 = delete;
    View& operator=(const View&) = delete;
    View& operator=(View&&)      = delete;

    static bool Init();
    static bool IsAvailable();
    static bool IsThirdPersonAvailable();
    // Third person is on right now, by its key or forced by the free cam
    static bool IsThirdPersonOn();

    // The viewmodel_* console variables were found
    static bool IsViewmodelAvailable();

    // Restores what was changed in the game, on exit
    static void Shutdown();

    // ConVarData of a console variable of the game, its float value at VALUE. 0 when not there. Replicated ones hold
    // the value of the server we play on
    static uintptr_t FindConVar(std::string_view name);
    static constexpr size_t CONVAR_VALUE = 0x58;
private:
    View() {};

    static View& GetInstance()
    {
        static View i{};
        return i;
    }

    bool InitImpl();
    void Thread();

    void UpdateFov(uintptr_t pawn);
    void UpdateThirdPerson(uintptr_t pawn);
    void PatchCheatsCheck(bool patched);

    // Viewmodel: the console variables of the game, written like typing them in the console
    void UpdateViewmodel();
    void RestoreViewmodel();
    bool FindViewmodelVars();
    uintptr_t FindConVarList(uintptr_t& count);

    // The game clamps the values again where it places the weapon, with the limits written in the code
    // (mov dword ptr [stack], imm32). Those immediates are widened while the override is on
    bool FindViewmodelClamps();
    void PatchViewmodelClamps(bool patched);

    struct ClampPatch {
        uintptr_t address = 0;  // Of the imm32
        uint32_t original = 0;
        uint32_t widened = 0;
    };

    struct ConVar {
        const char* name;
        uintptr_t data = 0;     // ConVarData in the game
        float min = 0.f, max = 0.f;
        float original = 0.f;   // What the player had, put back when turned off
    };

private:
    ConVar viewmodel_vars[4] = { { "viewmodel_fov" }, { "viewmodel_offset_x" }, { "viewmodel_offset_y" }, { "viewmodel_offset_z" } };
    std::atomic<bool> viewmodel_found = false;
    std::vector<ClampPatch> clamp_patches;
    bool clamps_searched = false;
    bool clamps_patched = false;
    bool viewmodel_applied = false;
    std::chrono::steady_clock::time_point viewmodel_next_search{};

    bool fov_applied = false;   // Our fov was written, so it has to be written back when turned off
    uintptr_t last_pawn = 0;

    std::atomic<bool> third_person_applied = false;
    bool third_person_toggled = false;
    bool third_person_key_was_down = false;
    bool cheats_check_patched = false;
    std::atomic<bool> stopping = false;
};
