#pragma once
#include "core/memory/Memory.hpp"
#include "core/engine/classes/Grenades.hpp"

#include <unordered_map>
#include <unordered_set>

// Removals (flash, smoke, scope) & glow written into the game
class Visuals {
public:
    ~Visuals()                         = default;
    Visuals(const Visuals&)            = delete;
    Visuals(Visuals&&)                 = delete;
    Visuals& operator=(const Visuals&) = delete;
    Visuals& operator=(Visuals&&)      = delete;

    static bool Init();

    // Writes into the game: needs -insecure
    static bool IsAvailable();

    // Zoomed in: the scoped flag, or the zoom of the weapon
    static bool IsZoomed(uintptr_t pawn);

    // The glow chams can be used: their code of the game was found
    static bool HasModelGlow();

    // The scope of the game is off for no scope overlay or no scope blur: the ESP draws ours, its lines this thick
    static bool IsScopeHidden();
    static int GetScopeLineWidth();

    // Glow color of a grenade in the air, by its type
    static const color_t& ThrownColor(GrenadeType type);

    // Puts back what the game had
    static void Shutdown();
private:
    Visuals() {};

    static Visuals& GetInstance()
    {
        static Visuals i{};
        return i;
    }

    bool InitImpl();
    void Thread();

    void UpdateFlash(uintptr_t pawn);
    // Our code of the scope while allowed & turned on, the one of the game otherwise
    void UpdateScope(bool allowed);
    void RefreshTargets();
    void UpdateSmoke();
    void UpdateGlow();
    void UpdateChams();
    void UpdateModelGlow();
    void RestoreModelGlow();
    void UpdateGlowReason();
    void RestoreGlowReason();
    void Restore();

    // The entity is still there, not freed memory: its identity points back at it
    static bool IsLive(uintptr_t entity);

    static uintptr_t GetActiveWeapon(uintptr_t pawn);

    // SetRenderColor of the game on the main thread for each (pawn, color), pawns gone meanwhile are skipped
    bool SetRenderColors(const std::vector<std::pair<uintptr_t, uint32_t>>& colors);

    // Our scene object updater in the game, with its table. False when it cannot be made
    bool EnsureGlowUpdater();

    // Our reason in the list the game asks whether a player glows, with its page. False when it cannot be put there
    bool EnsureGlowReason();

private:
    std::atomic<bool> stopping = false;

    bool flash_written = false;

    std::atomic<bool> scope_hidden = false;
    std::atomic<int> scope_line_width = 1;      // cl_crosshair_sniper_width
    uintptr_t scope_width_convar = 0;
    std::chrono::steady_clock::time_point next_scope_width{};

    // From the cache, refreshed now & then: what glows in which color, the popped smokes & every entity it knows
    std::vector<std::pair<uintptr_t, uint32_t>> glow_targets;
    struct ChamsTarget {
        uintptr_t pawn = 0;
        color_t color;
        bool tint = false;              // Textured
        bool glow = false;              // The spawn protection shader
        color_t tint_color;             // Of the textured one
        float glow_strength = 1.f;      // Times the alpha of the color
    };
    std::vector<ChamsTarget> chams_targets;
    std::vector<uintptr_t> smokes;
    std::unordered_set<uintptr_t> known;

    // Smokes we hid, with the start times of the game
    struct HiddenSmoke {
        float volume_start = 0.f;
        uintptr_t render = 0;
        float render_start = 0.f;
    };
    std::unordered_map<uintptr_t, HiddenSmoke> hidden_smokes;
    std::unordered_map<uintptr_t, uint32_t> glowing; // Entities we made glow, with our color
    // Players we colored: the render color of the game & ours
    struct Tint {
        uint32_t original = 0;
        uint32_t applied = 0;
        bool done = false;              // Applied through the game
    };
    std::unordered_map<uintptr_t, Tint> tinted;
    uintptr_t chams_code = 0;           // Code of the calls into the game
    std::chrono::steady_clock::time_point chams_retry{};

    // Glow chams: our updater page, the pawns using it with their updater handle & the table as last written
    uintptr_t glow_page = 0;
    std::unordered_map<uintptr_t, uintptr_t> glow_hooked;
    std::vector<uint8_t> glow_table;

    // Outline glow of players: the pawns & their colors (packed) from the cache, our reason page, the reason of the game
    // it took the place of, the table read now & what it holds
    std::vector<std::pair<uintptr_t, uint32_t>> glow_pawns;
    uintptr_t reason_page = 0;
    uintptr_t reason_original = 0;
    int reason_active = 0;
    std::vector<uint8_t> reason_table;
    bool reason_failed = false;
};
