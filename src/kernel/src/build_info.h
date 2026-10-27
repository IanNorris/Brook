#pragma once
namespace brook {
// Refreshed on every build; only the dedicated generated TU recompiles.
const char* BuildGitHash();
const char* BuildGitBranch();
const char* BuildDate();
}
