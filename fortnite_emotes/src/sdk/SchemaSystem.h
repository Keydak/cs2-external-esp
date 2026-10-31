#pragma once
#include <cstdint>

namespace emotes::sdk {

// Looks up the in-memory offset for a schema field.
// Returns -1 if the field is not found or the schema system is unavailable.
int32_t GetSchemaOffset(const char* module, const char* className, const char* fieldName);

// Initializes the schema system interface from schemasystem.dll.
bool InitSchemaSystem();

}  // namespace emotes::sdk
