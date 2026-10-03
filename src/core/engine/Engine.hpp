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

   static bool Init();
   static ProcessModule GetClient();
   static ProcessModule GetEngine();
   static std::shared_ptr<pProcess> GetProcess(); // Refactor this so its easier to access

   // True when the game was launched with -insecure (VAC disabled), required for features that write memory
   static bool IsInsecure();
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

    bool InitImpl();

    bool AwaitProcess();
    bool AwaitModules();

    void Thread();

private:
    std::shared_ptr<pProcess> process;
    ProcessModule client;
    ProcessModule engine;
    bool insecure = false;
};