#pragma once

enum class BombSite {
    Unknown = -1,
    A = 0,
    B = 1
};

class Bomb {
public:
    Bomb() {};

    bool Update();
    uintptr_t GetAddress() const { return address; }

    // Exact times from the game time, the plant time on its own is only whole seconds
    void SetGameTime(float now);

    // Time left & defuse left counted down to this moment, every frame so the timer runs smoothly
    void Tick();

    // The defuse ends before the bomb goes off
    bool CanDefuse() const { return defuse_left < time_left; }
public:
    Vec3_t pos;
    uintptr_t carrier;
    float time_left = 0.f;
    float timer_length = 40.f;
    bool is_planted = false;
    BombSite site = BombSite::Unknown;

    bool defusing = false;
    bool defuse_kit = false;
    float defuse_left = 0.f;
    float defuse_length = 10.f;
private:
    uintptr_t address = 0;
    float c4_blow = 0.f;
    float defuse_countdown = 0.f;

    // When it blows & the defuse ends on our clock, the game time only moves in ticks
    bool timed = false;
    std::chrono::steady_clock::time_point blow_at;
    std::chrono::steady_clock::time_point defuse_at;
    float last_countdown = 0.f;
    static bool prev_is_planted;
    static std::time_t plant_time;
};