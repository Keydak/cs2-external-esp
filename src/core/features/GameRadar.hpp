#pragma once
#include "core/memory/Memory.hpp"

#include <unordered_set>

// Marks enemies as spotted, so the radar of the game shows them
class GameRadar {
public:
    ~GameRadar()                           = default;
    GameRadar(const GameRadar&)            = delete;
    GameRadar(GameRadar&&)                 = delete;
    GameRadar& operator=(const GameRadar&) = delete;
    GameRadar& operator=(GameRadar&&)      = delete;

    static bool Init();
    static bool IsAvailable();
private:
    GameRadar() {};

    static GameRadar& GetInstance()
    {
        static GameRadar i{};
        return i;
    }

    bool InitImpl();
    void Thread();

    // Clears the spotted flags we wrote, so enemies leave the radar of the game
    void Restore(const std::unordered_set<uintptr_t>& marked);
};
