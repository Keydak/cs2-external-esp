#pragma once
#include "core/memory/Memory.hpp"

#include <mutex>
#include <unordered_map>

// An enemy we kill: its body is gone (the dead pawn left out of the drawing by MaterialChams, still there for the
// effect) & a particle effect of the game plays on it. Made by the particle manager of the game on its main thread, like its
// own effects (-insecure). The kills come from HitEffects
class KillEffect {
public:
    ~KillEffect()                            = default;
    KillEffect(const KillEffect&)            = delete;
    KillEffect(KillEffect&&)                 = delete;
    KillEffect& operator=(const KillEffect&) = delete;
    KillEffect& operator=(KillEffect&&)      = delete;

    static bool Init();

    // Needs -insecure & the particle code of the game
    static bool IsAvailable();

    // The effects of the menu, by cfg::world::kill_effect::effect
    static const std::vector<const char*>& GetNames();
    // The effect is No gravity: the body floats, its gravity set in the menu
    static bool Floats(int effect);

    // The effects on our own player, by cfg::world::self_effect::effect
    static const std::vector<const char*>& GetSelfNames();
    // A particle effect of the game played each interval (the others are auras the overlay draws all the time)
    static bool SelfUsesInterval(int effect);
    // The aura the overlay draws around our player right now (Overlays::RenderSelfAura), NONE when off, in first
    // person with Third Person Only, or a particle effect is picked
    enum class SelfAura { NONE, STORM, VOID_ORBS, HALO, FROST, RINGS, FIREFLIES, SAKURA, BLADES, MATRIX, SHIELD, TRAIL };
    static SelfAura GetSelfAura();

    // Drawn by the overlay where a body was, for Duration() after the kill. Zeus: lightning. Singularity: a black hole
    // the body is pulled into. Shatter: the body glass breaking apart. Glitch: the body in pixels breaking up & going
    // up. Ascend: a beam of light from above, the body rising in it as light. Frost: frozen to ice, it crumbles. Slash:
    // cut in two by a blade of light. Pixels: falls apart into blocks that bounce on the ground. Meteor: a fireball from
    // the sky hits it. Tornado: a whirlwind takes it up. TV Off: switched off like an old television. Supernova: pulled
    // into a star that explodes
    enum class StrikeKind { ZEUS = 1, SINGULARITY = 2, SHATTER = 3, GLITCH = 4, ASCEND = 5, FROST = 7, SLASH = 8, PIXELS = 10,
        METEOR = 11, TORNADO = 12, TV_OFF = 13, SUPERNOVA = 14 };
    struct Strike {
        StrikeKind kind = StrikeKind::ZEUS;
        Vec3_t feet;            // The ground under the body
        Vec3_t center;          // The middle of the body
        std::vector<Vec3_t> bones;  // Where its bones were last seen alive
        std::chrono::steady_clock::time_point at{};
        uint32_t seed = 0;      // Its random shapes
    };
    static float Duration(StrikeKind kind) {
        switch (kind) {
        case StrikeKind::SINGULARITY:   return 2.1f;
        case StrikeKind::SHATTER:       return 1.9f;
        case StrikeKind::GLITCH:        return 1.5f;
        case StrikeKind::ASCEND:        return 2.4f;
        case StrikeKind::FROST:         return 2.3f;
        case StrikeKind::SLASH:         return 1.4f;
        case StrikeKind::PIXELS:        return 2.4f;
        case StrikeKind::METEOR:        return 2.3f;
        case StrikeKind::TORNADO:       return 2.4f;
        case StrikeKind::TV_OFF:        return 1.2f;
        case StrikeKind::SUPERNOVA:     return 2.1f;
        default:                        return 1.6f;
        }
    }
    static std::vector<Strike> GetStrikes();

    static void Shutdown();
private:
    KillEffect() {};

    static KillEffect& GetInstance()
    {
        static KillEffect i{};
        return i;
    }

    void Thread();
    void Update();

    // One effect in the world: where, the way it goes (zero: its own)
    struct Shot {
        Vec3_t position;
        Vec3_t forward;
    };
    struct Body;
    // Out of each part of the body, away from its middle
    static std::vector<Shot> Burst(const Body& body);
    // Plays a particle system: on the body (the shots left out), or in the world at each shot
    void Play(uintptr_t entity, const char* file, bool follow, int copies, std::vector<Shot> shots);

    // No gravity: ragdoll_gravity_scale written while a body of our kills is there, the value of the player back after
    void UpdateGravity();

    // The effect on our own player, again each interval
    void UpdateSelf();
    std::chrono::steady_clock::time_point self_next{};
    bool self_logged = false;
    bool quiet = false;     // Play logs nothing

private:
    std::atomic<bool> stopping = false;
    uintptr_t page = 0;     // Code & data of one call

    uintptr_t gravity_convar = 0;   // ConVarData of ragdoll_gravity_scale
    float gravity_original = 1.f;
    bool gravity_applied = false;
    std::chrono::steady_clock::time_point gravity_search{};

    // Enemies we killed, by pawn, while they are dead: the effect on them, their body kept hidden
    struct Body {
        Vec3_t center;
        std::vector<Vec3_t> bones;  // Where its bones were last seen alive
        std::chrono::steady_clock::time_point killed{};
        int waves = 0;          // Played so far
        std::chrono::steady_clock::time_point next_wave{};
    };
    std::unordered_map<uintptr_t, Body> bodies;
    std::vector<uintptr_t> hidden_last;     // The pawns last given to MaterialChams

    // Enemies seen dying, by player index: a kill whose victim HitEffects could not tell (no health drop matched to a
    // hit) takes the one that died at the same moment
    std::unordered_map<int, bool> was_alive;
    // The middle of each enemy while alive: dead, the cache reads neither its bones nor where it is
    std::unordered_map<int, Vec3_t> last_center;
    std::unordered_map<int, std::vector<Vec3_t>> last_bones;
    struct Death {
        int index = -1;
        std::chrono::steady_clock::time_point at{};
        bool used = false;
    };
    std::vector<Death> deaths;

    std::mutex strikes_mutex;
    std::vector<Strike> strikes;
    uint32_t strike_seed = 0x9E3779B9;
};
