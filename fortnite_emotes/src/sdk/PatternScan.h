#pragma once
#include <cstdint>

namespace emotes::sdk {

// Scans a loaded module for a byte pattern (IDA-style: "48 8B ? 05 ?" where ? is wildcard).
// Returns the address of the first match, or 0 on failure.
uintptr_t PatternScan(const char* moduleName, const char* pattern);

// Resolves a RIP-relative address at the given location.
// offset = bytes from the base to the RIP operand, instrLen = total instruction length.
uintptr_t ResolveRelative(uintptr_t base, int offset, int instrLen);

}  // namespace emotes::sdk
