#pragma once
#include "config/Config.hpp"
#include "core/memory/Memory.hpp"

class Engine {
public:
    ~Engine()                           = default;
    Engine(const Engine&)            = delete;
    Engine(Engine&&)                 = delete;
    Engine& operator=(const Engine&) = delete;
    Engine& operator=(Engine&&)      = delete;

   // The game found & read: the process, its modules, -insecure, the offsets of this build & the config. Waits
   // for the game to be opened. False when it cannot be used, GetProgress says why
   static bool Attach();
   // The thread reading the game & the features, once the loader is done
   static void Start();
   // What Attach does, or why it failed
   static std::string GetProgress();
   // Attach waits for CS2 to be opened
   static bool IsWaitingForGame();
   static ProcessModule GetClient();
   static ProcessModule GetEngine();
   static std::shared_ptr<pProcess> GetProcess(); // Refactor this so its easier to access

   // True when the game was launched with -insecure (VAC disabled), required for features that write memory.
   // False once the program is outdated, so nothing is written while it stops
   static bool IsInsecure();

   // Something of the game is not where this build of the program expects it (CS2 was updated): everything stops,
   // the renderer closes & the features put back what they changed
   static void ReportOutdated(const std::string& what);
   static bool IsOutdated();
   static std::string GetOutdated();
   static uintptr_t GetLocalPawn();
   static uintptr_t GetEntityFromHandle(uint32_t handle);

   // Asks the server to resend every entity, which also resets values we wrote that the game keeps predicting
   static bool ForceFullUpdate();
private:
    Engine() {};

    static Engine& GetInstance()
    {
        static Engine i{};
        return i;
    }

    bool AttachImpl();
    void StartImpl();
    void SetProgress(const std::string& text);

    bool AwaitProcess();
    bool AwaitModules();

    void Thread();

    // Reads a few things that are always the same in a match: wrong ones mean offsets of another build
    void CheckSanity();

private:
    std::shared_ptr<pProcess> process;
    ProcessModule client;
    ProcessModule engine;
    bool insecure = false;

    std::atomic<bool> outdated = false;
    std::mutex outdated_mtx;
    std::string outdated_reason;
    int failed_checks = 0;      // Seconds in a row the checks failed

    std::mutex progress_mtx;
    std::string progress;
    std::atomic<bool> waiting_for_game = false;
    bool just_opened = false;   // CS2 was opened while we waited: it gets a moment to load all of itself
};