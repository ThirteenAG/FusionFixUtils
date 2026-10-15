// FusionFixUtilsTests: one test file per header of FusionFixUtils (callbacks.cpp tests include/callbacks.h and so on).
//
//   FusionFixUtilsTests.exe                runs everything
//   FusionFixUtilsTests.exe gameref ...    only these suites (test file names)
//
// The exit code is the number of failed checks; a crash prints where it happened and exits with 255.

#include <windows.h>
#include <psapi.h>
#include <crtdbg.h>
#include <cstdlib>
#include "check.h"

namespace
{
    LONG WINAPI Crashed(EXCEPTION_POINTERS* info)
    {
        auto address = info->ExceptionRecord->ExceptionAddress;
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module))
            GetModuleFileNameA(module, name, MAX_PATH);
        std::printf("CRASH: exception %08lX at %p (%s+0x%IX) in [%s]\n", info->ExceptionRecord->ExceptionCode, address, name,
            reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module), check::CurrentCase());
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 255);
        return EXCEPTION_EXECUTE_HANDLER;
    }
}

// tests/callbacks.cpp: a child that registers DLL notifications and exits
int CallbacksExitChild();

int wmain(int argc, wchar_t** argv)
{
    // No error dialogs from the CRT or Windows: a test that crashes must fail the run, not wait for a click
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // Debug CRT: asserts and abort() report to stderr, never in a message box
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_error_mode(_OUT_TO_STDERR);
    for (int type : { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT })
    {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
    if (argc > 1 && wcscmp(argv[1], L"--callbacks-exit") == 0)
        return CallbacksExitChild();
    SetUnhandledExceptionFilter(Crashed);

    std::vector<std::string> only;
    for (int i = 1; i < argc; ++i)
    {
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, name, sizeof(name), nullptr, nullptr);
        only.emplace_back(name);
    }
    std::printf("FusionFixUtils tests, %s\n", sizeof(void*) == 8 ? "x64" : "Win32");
    auto failures = check::RunAll(only);
    check::CurrentCase() = "(exit)";
    std::fflush(stdout);
    return failures;
}
