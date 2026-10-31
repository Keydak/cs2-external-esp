#pragma once
#include <windows.h>

namespace emotes::hooks {

void Initialize(HMODULE selfModule);
void Shutdown();
bool ShouldUnload();
bool CanUnload();

void RecordPrologue(void* target);
bool SafeRemoveHook(void* target);

}  // namespace emotes::hooks
