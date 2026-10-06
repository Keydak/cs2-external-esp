#pragma once

#include "http/HttpHelper.hpp"

struct Status {
    bool unsafe;
    std::string notice;

    int version_current;
    int version_minimum;
};

class Updater {
public:
    ~Updater() = default;
    Updater(const Updater&) = delete;
    Updater(Updater&&) = delete;
    Updater& operator=(const Updater&) = delete;
    Updater& operator=(Updater&&) = delete;

    static bool Init();
    static bool Update(); // Not Implemented Yet
    static bool Process();

    static Status GetStatus();
private:
    Updater() {};

    static Updater& GetInstance()
    {
        static Updater i{};
        return i;
    }

    bool InitImpl();
    bool ProcessImpl();
private:
    Status status;
    bool isSetup = false;
    // This fork's own numbering, raise it with "current" in .github/status.json when a version is pushed
    int current_version = 200;
    std::string project_url = "https://github.com/Keydak/cs2-external-esp";
    std::string status_url = "https://raw.githubusercontent.com/Keydak/cs2-external-esp/main/.github/status.json";
};