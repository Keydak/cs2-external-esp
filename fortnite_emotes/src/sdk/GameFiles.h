// Adding a folder of compiled game files (models, particles) to the game's file system.
#pragma once

namespace emotes::sdk {

// A "GAME" search path at `utf8Dir` (the folder holding e.g. particles/..., characters/...), so the
// game finds resources there by name. Do it before the first manifest that loads from it.
bool MountGamePath(const char* utf8Dir);
bool CanMountGamePaths();   // AddSearchPath was found

}  // namespace emotes::sdk
