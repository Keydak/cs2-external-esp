// Client-only prop_dynamic_override spawner (the dance prop and the agent copy). Game thread only; false = game fault.
#pragma once
#include <cstdint>

namespace emotes {

bool ClientPropReady();
bool ClientPropSpawn(const char* model, const float* origin, const float* angles,
                     const char* const (*keys)[2], int keyCount, uintptr_t& ent);
bool ClientPropRemove(uintptr_t ent);
bool ClientPropMove(uintptr_t ent, const float* pos, const float* ang);

}  // namespace emotes
