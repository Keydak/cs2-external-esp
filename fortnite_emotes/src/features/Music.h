// Dance music: .wav files played through waveOut on a thread of their own, not through the game's sound system.
#pragma once
#include <atomic>
#include <filesystem>

namespace emotes {

// Replaces any current music; any thread. `volume` (0-1, outlives the music) is read as it plays, on a squared curve,
// and every song is brought to one loudness first.
void MusicPlay(const std::filesystem::path& wav, const std::atomic<float>* volume);
void MusicStop();
void MusicShutdown();  // DLL unload, after hooks are disabled: waits (<= 2 s) for the player threads

}  // namespace emotes
