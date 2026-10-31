#pragma once

namespace emotes::hooks {

// FrameStageNotify, LevelShutdown, OnRemoveEntity and the feature hooks (ApplyAnimGraphPose, CAM_Think).
bool InstallGameHooks();
// GenerateScenePrimitives: the dances hide the real player here. Installed on the first stage 5 that needs it.
void InstallDrawHookIfNeeded();
bool DrawHookFailed();  // the draw hook was tried and is not live (dances can't hide players)
bool RemoveGameHooks();

}  // namespace emotes::hooks
