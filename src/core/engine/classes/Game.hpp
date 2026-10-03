#pragma once

class Game {
public:
    Game() {};

    bool Update();
    bool UpdateMatrix();
    bool UpdateEntityList();
    bool UpdateMode();

    // Is the player an enemy of the local one, everyone is in deathmatch
    bool IsEnemy(int local_team, int team) const { return deathmatch || team != local_team; }

public:
    view_matrix_t view_matrix;

    uintptr_t entity_list;
    uintptr_t list_entry;

    bool deathmatch = false;
private:
    uintptr_t address;
    uintptr_t mode_rules = 0; // The mode is only looked up again when this changes
};