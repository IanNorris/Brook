#pragma once

// Build info — populated by CMake defines at compile time.
// Used in panic screens and crash dumps.

namespace brook {

#ifndef BROOK_GIT_HASH
#define BROOK_GIT_HASH "unknown"
#endif

#ifndef BROOK_BUILD_DATE
#define BROOK_BUILD_DATE "unknown"
#endif

inline const char* BuildGitHash()  { return BROOK_GIT_HASH; }
inline const char* BuildDate()     { return BROOK_BUILD_DATE; }

} // namespace brook
