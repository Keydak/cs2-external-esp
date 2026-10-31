// Fortnite dances (model from Cruze03/FortniteEmotesNDances, in the Emote Pack): a client-only prop_dynamic plays the
// dance, a copy of the player's agent is bonemerged onto it, and the real player is not drawn. Only you see it.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
namespace emotes {
struct FortniteDancesConfig {
    std::atomic<bool>  enabled{false};   // master switch: mount the folder, load the model, offer the dances
    std::atomic<bool>  music{false};     // music for your own dance
    std::atomic<float> volume{0.7f};     // 0-1
};
FortniteDancesConfig& GetFortniteDancesConfig();
enum class DanceStatus : int { Off, NoFiles, Loading, Ready, LoadFailed, Unavailable };
DanceStatus FortniteDancesStatus();                      // any thread
inline constexpr int kFortniteDanceCount = 118;          // FortniteDanceTable.inc rows (checked there)
int         FortniteDanceCount();                        // kFortniteDanceCount
const char* FortniteDanceName(int i);                    // "" out of range
bool        FortniteDanceIsEmote(int i);
float       FortniteDanceLength(int i);                  // endAfter seconds, INFINITY = until cancelled
const char* FortniteDanceSeq(int i);                     // sequence name = icon file name (icons/<seq>.png)
int         FortniteDanceRarity(int i);                  // FortniteDanceRarity.inc code, 0 unknown
void FortniteDancesSetDir(const std::filesystem::path& dir);  // the Emote Pack folder, any thread (mutex-copied)
std::filesystem::path FortniteDancesDir();               // the Emote Pack folder, empty before the config is read
// Game thread, during EmoteWheelFrame; cleared each frame. fixedYaw: hold the yaw it started with (the emote),
// else turn with the pawn.
void FortniteDancesWant(void* pawn, int dance, bool fixedYaw = false);
void FortniteDancesFrame();                              // game thread, stage 12 right after EmoteWheelFrame
void FortniteDancesOnEntityRemoved(void* ent);           // HkOnRemoveEntity: forget our props the game destroyed
void FortniteDancesLevelShutdown();                      // HkLevelShutdown: forget props, drop the manifest, stop music
void FortniteDancesRelease();                            // RemoveGameHooks step 1 (waits <= 100 ms)
bool FortniteDancesHides(void* sceneObject);             // render threads (HkOnGenPrims): true = skip drawing
bool FortniteDancesNeedDrawHook();                       // stage 5: install the GenPrims hook
}  // namespace emotes
