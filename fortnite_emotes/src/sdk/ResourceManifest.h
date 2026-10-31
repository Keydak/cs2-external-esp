#pragma once
namespace emotes::sdk {
// Async resource loads through engine2 IGameResourceService, as C_CSGO_PreviewModel does. Game thread only; SEH-guarded.
void* GameResourceService();                                 // null if missing (warned once)
void* ManifestCreate(const char* const* names, int count);   // starts the load; null on failure
bool  ManifestLoaded(void* manifest);                        // true once everything is loaded; false on fault
void  ManifestDestroy(void* manifest);                       // null is fine
const char* ResourceReason();   // "emotes" in tier0 memory, for game calls that may keep the pointer; null if missing
}  // namespace emotes::sdk
