#include "bootstrap_diagnostics.hpp"
#include "executable_check.hpp"

#include <windows.h>

namespace
{
DWORD WINAPI bootstrap(void*)
{
    try
    {
        cinemania::report_bootstrap(cinemania::check_executable());
    }
    catch (...)
    {
        OutputDebugStringA("Cinemania: check failure; stage=exception; SHA256=unavailable\n");
    }

    return 0;
}
} // namespace

// The exported name is part of the ordinal-1 loader contract.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void CinemaniaLoadAnchor()
{
}

// Windows requires this entry-point name.
// NOLINTNEXTLINE(readability-identifier-naming)
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
    {
        return TRUE;
    }

    // Pin before starting the worker: v1 has no hot unload and the thread executes DLL code.
    HMODULE pinned_module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(module),
                            &pinned_module))
    {
        return TRUE;
    }

    HANDLE thread = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr);
    if (thread != nullptr)
    {
        CloseHandle(thread);
    }

    return TRUE;
}
