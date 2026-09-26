#pragma once

#include <filesystem>

namespace blueprint {

// Directory that contains shaders, maps, and the other runtime files.
// Prefers the source tree used at compile time, then the folder containing the
// executable, then the current working directory.
std::filesystem::path ContentRoot();

// Resident memory of this process, in megabytes. Returns 0 when the OS cannot
// report it.
float ProcessRamUsageMb();

} // namespace blueprint
