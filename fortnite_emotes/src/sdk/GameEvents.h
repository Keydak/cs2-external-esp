#pragma once

namespace emotes::sdk {

// weapon_fire and round_start, for the emote wheel (shooting and a new round stop your emote).
void InstallGameEventListener();
void RemoveGameEventListener();

}  // namespace emotes::sdk
