#pragma once
#include "core/memory/Memory.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

// Chams with materials of our own: where a player can be seen & behind walls, each its own material & color.
//
// The game draws the meshes of animated models through CAnimatableSceneObjectDesc::DrawArray. Its vtable entry points
// at our stub: meshes of the players we picked are drawn one at a time, first with the behind walls material (no depth
// test), then with the visible one, their own material put back after. Everything else goes to the game as it came.
// The materials are made by the game from KeyValues3 text (csgo_unlitgeneric) on its main thread, again when a color
// changes. Which field of a mesh holds its scene object is checked against the scene objects of the players first
class MaterialChams {
public:
    ~MaterialChams()                               = default;
    MaterialChams(const MaterialChams&)            = delete;
    MaterialChams(MaterialChams&&)                 = delete;
    MaterialChams& operator=(const MaterialChams&) = delete;
    MaterialChams& operator=(MaterialChams&&)      = delete;

    static bool Init();

    // Needs -insecure & the code of the game
    static bool IsAvailable();

    // Why they do not show yet, empty when they work
    static std::string GetStatus();

    // Behind walls only with Flat & Glow (user's pick): Hologram mixed its colors, Metallic left patches
    static bool HasBehindWalls(int material);

    // Something of a group is drawn in our material: its visible layer or its behind walls one
    static bool Wanted(const cfg::visuals::material_chams::group_t& group);

    // Pawns not drawn at all (the bodies of KillEffect). Only the drawing is left out, nothing of the game changes:
    // switching their mesh off crashed it
    static void SetHiddenBodies(const std::vector<uintptr_t>& pawns);

    // The vtable entry of the game back
    static void Shutdown();
private:
    MaterialChams() {};

    static MaterialChams& GetInstance()
    {
        static MaterialChams i{};
        return i;
    }

    bool InitImpl();
    void Thread();
    void Update();

    bool Install();
    void WriteTargets();
    void Calibrate();

    // The materials of the layers turned on, made again when one changed. False when the game did not make them
    bool UpdateMaterials();

    // Slots of the page: visible & behind walls of each group, in the order of their class in the table (1 enemy,
    // 2 team, 3 planted bomb, 4 items on the ground, 5 our agent, 6 first person weapon, 7 first person hands: the
    // group of the agent, apart for its own hologram)
    enum Slot {
        ENEMY_VISIBLE, ENEMY_HIDDEN, TEAM_VISIBLE, TEAM_HIDDEN, BOMB_VISIBLE, BOMB_HIDDEN, ITEMS_VISIBLE, ITEMS_HIDDEN,
        LOCAL_VISIBLE, LOCAL_HIDDEN, WEAPON_VISIBLE, WEAPON_HIDDEN, HANDS_VISIBLE, HANDS_HIDDEN, SLOT_COUNT
    };
    // layered: the visible pass of a group that also has a behind walls pass, see Layered(). first_person: the arms
    // & gloves in front of the camera
    static std::string MaterialText(int material, bool hidden, bool layered, float r, float g, float b, float a,
        bool first_person = false);

private:
    std::atomic<bool> stopping = false;
    std::atomic<bool> installed = false;

    uintptr_t page = 0;             // Stub, its data, the table of scene objects
    uintptr_t entry = 0;            // The vtable entry of DrawArray
    uintptr_t original = 0;         // DrawArray of the game

    uintptr_t material_system = 0;
    uintptr_t load_kv3 = 0;         // tier0 export

    // What the materials were made for, a key per slot ("" = off). Made again once it stays changed a moment
    std::array<std::string, SLOT_COUNT> made{};
    std::array<std::string, SLOT_COUNT> wanted_last{};
    std::chrono::steady_clock::time_point wanted_since{};
    int generation = 0;
    int failures = 0;

    // Owner field of a mesh: the stub votes, we pick once one is sure
    std::chrono::steady_clock::time_point calibrate_since{};
    bool calibrate_told = false;


    std::mutex bodies_mutex;
    std::vector<uintptr_t> hidden_bodies;

    void SetStatus(const std::string& text);
    std::mutex status_mutex;
    std::string status;
};
