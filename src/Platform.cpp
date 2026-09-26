#include "Platform.hpp"

#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace blueprint {
namespace {

bool HasEngineContent(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path / "shaders" / "basic.vert", error);
}

std::filesystem::path ExecutableDirectory() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
#elif defined(__linux__)
    std::error_code error;
    const std::filesystem::path executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return {};
    }
    return executable.parent_path();
#else
    return {};
#endif
}

} // namespace

std::filesystem::path ContentRoot() {
    const std::filesystem::path compiledRoot{BLUEPRINT_SOURCE_DIR};
    if (HasEngineContent(compiledRoot)) {
        return compiledRoot;
    }

    const std::filesystem::path executableDirectory = ExecutableDirectory();
    if (HasEngineContent(executableDirectory)) {
        return executableDirectory;
    }

    std::error_code error;
    const std::filesystem::path workingDirectory = std::filesystem::current_path(error);
    if (!error && HasEngineContent(workingDirectory)) {
        return workingDirectory;
    }
    return compiledRoot;
}

float ProcessRamUsageMb() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        return static_cast<float>(counters.WorkingSetSize) / (1024.0F * 1024.0F);
    }
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    if (statm.is_open()) {
        unsigned long size = 0;
        unsigned long resident = 0;
        if (statm >> size >> resident) {
            const long pageSizeKb = sysconf(_SC_PAGE_SIZE) / 1024;
            return static_cast<float>(resident * static_cast<unsigned long>(pageSizeKb)) / 1024.0F;
        }
    }
#endif
    return 0.0F;
}

} // namespace blueprint
