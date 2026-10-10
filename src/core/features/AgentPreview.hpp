#pragma once
#include "core/memory/Memory.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

// Pictures of the agents of the ESP preview taken from the game itself (-insecure), once: Number K for the enemies,
// the default CT for the team. Saved as PNG in cache/agents, the menu shows them from then on, also without -insecure.
// Taken while the program starts: the menu opens once they are there (or could not be taken).
//
// Taking one: a MapPlayerPreviewPanel of Panorama (like the agent of the loadout) is made by JavaScript right under the
// loading card of the overlay, which covers it, in the root panels of the main menu & of the hud (the game draws the
// one shown).
// The screen is captured with the overlay left out (like streamproof), over a black then a white background: how much
// the two differ is how see-through each pixel is. The scripts run through CUIEngine::RunScript at the start of
// CUIEngine::RunFrame, on the main thread every frame in the menu too: the vtable of the engine is a copy of ours with
// that entry going through a stub
class AgentPreview {
public:
    ~AgentPreview()                              = default;
    AgentPreview(const AgentPreview&)            = delete;
    AgentPreview(AgentPreview&&)                 = delete;
    AgentPreview& operator=(const AgentPreview&) = delete;
    AgentPreview& operator=(AgentPreview&&)      = delete;

    static bool Init();

    // Taking pictures needs -insecure & the code of Panorama
    static bool IsAvailable();

    // Where the picture of a team is saved
    static std::string PicturePath(bool terrorist);
    // Changes when a picture was saved: the menu loads them again
    static int GetGeneration();

    // The pictures are there, or could not be taken: the program is ready, the menu may open
    static bool IsReady();
    // What it does while it is not ready, for the loading card
    static std::string GetProgress();
    // How far it is, 0 - 1
    static float GetPercent();

    // Every frame the loading card shows: where its spot for the agent is (pixels of the window, the card covers it) &
    // where the window is on the screen. Pictures are taken under it, not asked for a moment they wait
    static void Request(float x, float y, float width, float height, float screen_x, float screen_y);

    // A picture is being taken: the overlay is left out of screen captures meanwhile
    static bool IsCapturing();

    // Why no picture was taken, empty when none failed
    static std::string GetStatus();

    // The panels removed, the vtable of the engine put back
    static void Shutdown();
private:
    AgentPreview() {};

    static AgentPreview& GetInstance()
    {
        static AgentPreview i{};
        return i;
    }

    bool InitImpl();
    void Thread();
    void Update();

    struct Wanted {
        float x = 0.f, y = 0.f, width = 0.f, height = 0.f;
        float screen_x = 0.f, screen_y = 0.f;
    };

    // Asked for right now, what for
    bool Asked(Wanted* want = nullptr);

    // One picture: false when it stopped or failed (status says why)
    bool Capture(const Wanted& want, bool terrorist);

    // The panels (CUIPanel*) that have a JavaScript context: the roots of the layouts. Gone ones are not in it
    std::vector<uintptr_t> ContextPanels();
    bool HasContext(uintptr_t panel);
    // The one with this id, 0 when not there
    uintptr_t FindRoot(const char* id);
    // The roots of the main menu & of the hud that are there
    std::vector<uintptr_t> Roots();

    // Runs the script in the context of the panel on the main thread
    bool Run(uintptr_t panel, const std::string& script);
    bool Run(const std::vector<uintptr_t>& panels, const std::string& script);  // In each, in the same frame

    // Our vtable copy of the engine, the stub at the start of RunFrame. Put back when we stop
    bool InstallFrameHook();
    void RemoveFrameHook();
    // Runs function (no arguments) at the start of the next frame, false when it did not in time
    bool CallInFrame(uintptr_t function, DWORD timeout_ms);

    void SetStatus(const std::string& text);
    void SetProgress(const std::string& text);
    // How far the picture being taken is, 0 - 1: the percent goes from stage_from over stage_span
    void SetStage(float stage);

private:
    std::atomic<bool> stopping = false;
    std::atomic<bool> capturing = false;
    std::atomic<bool> ready = false;
    std::atomic<int> generation = 0;
    std::atomic<float> percent = 0.f;
    float stage_from = 0.f, stage_span = 1.f;

    uintptr_t engine = 0;           // CUIEngine
    uintptr_t page = 0;             // The call & the script
    uintptr_t frame_page = 0;       // The job, the stub & the vtable copy
    uintptr_t real_vtable = 0;
    uintptr_t copy_vtable = 0;

    std::mutex wanted_mutex;
    std::condition_variable wake;
    Wanted wanted{};
    std::chrono::steady_clock::time_point asked_at{};

    // A team that failed is tried again after a moment, a few times
    std::chrono::steady_clock::time_point failed_at[2]{};
    int attempts[2]{};
    int failures = 0;
    std::chrono::steady_clock::time_point started{};    // First asked, under wanted_mutex

    std::mutex status_mutex;
    std::string status;
    std::string progress;
};
