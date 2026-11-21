/* Standalone test for the Windows Vulkan UUID-to-LUID probe. */
#include "modules/cfsk/cfsk.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string_view>

static unsigned gVulkanProbes = 0;

static HMODULE WINAPI count_load(LPCWSTR name, HANDLE file, DWORD flags) {
    if (name && std::wcscmp(name, L"vulkan-1.dll") == 0 &&
        flags == LOAD_LIBRARY_SEARCH_SYSTEM32) {
        ++gVulkanProbes;
    }
    const auto load = reinterpret_cast<decltype(&LoadLibraryExW)>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryExW"));
    return load(name, file, flags);
}

extern "C" decltype(&LoadLibraryExW) __imp_LoadLibraryExW = count_load;

static bool gFailNextThread = false;
static unsigned gInjections = 0;

extern "C" uintptr_t __cdecl _beginthreadex(void* attributes, unsigned stack,
                                             unsigned (__stdcall *run)(void*), void* arg,
                                             unsigned flags, unsigned* id) {
    if (gFailNextThread) {
        gFailNextThread = false;
        ++gInjections;
        std::puts("injected thread creation failure");
        std::fflush(stdout);
        return 0;
    }
    const auto start = reinterpret_cast<decltype(&_beginthreadex)>(
            GetProcAddress(GetModuleHandleW(L"ucrtbase.dll"), "_beginthreadex"));
    return start(attributes, stack, run, arg, flags, id);
}

static bool test_precompile() {
    cfsk_context* context = nullptr;
    if (cfsk_context_make_vulkan(nullptr, {}, &context) != CFSK_OK) {
        return false;
    }
    const uint8_t blob[] = {'C', 'F', 'P', '1', 1, 0, 0, 0, 0};
    const cfsk_bytes keys{blob, sizeof(blob)};
    gFailNextThread = true;
    const auto failed = cfsk_context_precompile_deferred(context, keys);
    const auto retry = cfsk_context_precompile_deferred(context, keys);
    cfsk_context_destroy(context);
    std::printf("precompile first=%d retry=%d injections=%u\n", failed, retry, gInjections);
    return failed == CFSK_FAILED && retry == CFSK_OK && gInjections == 1;
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    if (argc == 2 && std::string_view(argv[1]) == "precompile") {
        return !test_precompile();
    }
    size_t adapters = 0;
    const auto first = cfsk_dawn_adapters(nullptr, 0, &adapters);
    const auto second = cfsk_dawn_adapters(nullptr, 0, &adapters);
    cfsk_dawn_host* host = nullptr;
    const auto opened = cfsk_dawn_host_make(&host);
    if (host) {
        cfsk_dawn_host_release(&host);
    }
    std::printf("list=%d,%d open=%d adapters=%zu vulkanProbes=%u\n",
                first, second, opened, adapters, gVulkanProbes);
    return !(first == CFSK_OK && second == CFSK_OK && opened == CFSK_OK && gVulkanProbes == 1);
}
