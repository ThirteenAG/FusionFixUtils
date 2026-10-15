#pragma once
// Module load/unload callbacks and deferred initialization for ASI plugins. Needs Hooking.Patterns and safetyhook
// (ThirteenAG/injector) on the include path.
//
// RegisterCallback(module, fn) and RegisterModuleUnloadCallback(module, fn) keep ONE callback per module name (case
// insensitive): a second registration for a module that isn't loaded yet is dropped, so chain the inits in the one
// registration. The callbacks fire on every load and unload of the module.
#include <Windows.h>
#include <algorithm>
#include <cwctype>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <subauth.h>
#include "Hooking.Patterns.h"
#include "safetyhook.hpp"

class CallbackHandler
{
public:
    static inline void RegisterCallback(std::function<void()>&& fn)
    {
        return fn();
    }

    static inline void RegisterCallback(std::wstring_view module_name, std::function<void()>&& fn)
    {
        return RegisterModuleLoadCallback(module_name, std::forward<std::function<void()>>(fn));
    }

    static inline void RegisterModuleLoadCallback(std::wstring_view module_name, std::function<void()>&& fn)
    {
        if ((module_name.empty() || GetModuleHandleW(module_name.data()) != NULL))
        {
            return fn();
        }
        else
        {
            RegisterDllNotification();
            GetOnModuleLoadCallbackList().emplace(module_name, std::forward<std::function<void()>>(fn));
        }
    }

    static inline void RegisterModuleUnloadCallback(std::wstring_view module_name, std::function<void()>&& fn)
    {
        RegisterDllNotification();
        GetOnModuleUnloadCallbackList().emplace(module_name, std::forward<std::function<void()>>(fn));
    }

    static inline void RegisterAnyModuleLoadCallback(std::function<void(HMODULE)>&& fn)
    {
        RegisterDllNotification();
        GetOnAnyModuleLoadCallbackList().emplace_back(std::forward<std::function<void(HMODULE)>>(fn));
    }

    static inline void RegisterAnyModuleUnloadCallback(std::function<void(std::wstring_view module_name)>&& fn)
    {
        RegisterDllNotification();
        GetOnAnyModuleUnloadCallbackList().emplace_back(std::forward<std::function<void(std::wstring_view)>>(fn));
    }

    //[[deprecated("Use RegisterCallbackAtGetSystemTimeAsFileTime instead.")]]
    //static inline void RegisterCallback(std::function<void()>&& fn, bool bPatternNotFound, ptrdiff_t offset = 0x1100, uint32_t* ptr = nullptr)
    //{
    //    if (!bPatternNotFound)
    //    {
    //        return fn();
    //    }
    //    else
    //    {
    //        auto mh = GetModuleHandle(NULL);
    //        IMAGE_NT_HEADERS* ntHeader = (IMAGE_NT_HEADERS*)((DWORD)mh + ((IMAGE_DOS_HEADER*)mh)->e_lfanew);
    //        if (ptr == nullptr)
    //            ptr = (uint32_t*)((DWORD)mh + ntHeader->OptionalHeader.BaseOfCode + ntHeader->OptionalHeader.SizeOfCode - offset);
    //        std::thread([](std::function<void()>&& fn, uint32_t* ptr, uint32_t val)
    //        {
    //            while (*ptr == val)
    //                std::this_thread::yield();
    //
    //            fn();
    //        }, fn, ptr, *ptr).detach();
    //    }
    //}
    
    [[deprecated("Use RegisterCallbackAtGetSystemTimeAsFileTime instead.")]]
    static inline void RegisterCallback(std::function<void()>&& fn, hook::pattern pattern)
    {
        if (!pattern.empty())
        {
            fn();
        }
        else
        {
            auto* ptr = new ThreadParams{ fn, pattern };
            CloseHandle(CreateThread(0, 0, (LPTHREAD_START_ROUTINE)&ThreadProc, (LPVOID)ptr, 0, NULL));
        }
    }

    // fn runs at the next call of GetSystemTimeAsFileTime (the game's CRT calls it early in its startup), in the order
    // of registration. A callback may register further callbacks; they run at a later call.
    static inline void RegisterCallbackAtGetSystemTimeAsFileTime(std::function<void()>&& fn)
    {
        std::lock_guard<std::recursive_mutex> lock(GetCallbackParamsMutex());
        GetCallbackParamsList().emplace_back(fn, std::nullopt);
        if (!shGetSystemTimeAsFileTime)
            shGetSystemTimeAsFileTime = safetyhook::create_inline(GetSystemTimeAsFileTime, GetSystemTimeAsFileTimeHook);
    }

    // fn runs now when the pattern is found, otherwise at the first call of GetSystemTimeAsFileTime at which it is
    static inline void RegisterCallbackAtGetSystemTimeAsFileTime(std::function<void()>&& fn, hook::pattern pattern)
    {
        if (!pattern.empty())
        {
            return fn();
        }
        else
        {
            std::lock_guard<std::recursive_mutex> lock(GetCallbackParamsMutex());
            GetCallbackParamsList().emplace_back(fn, pattern);
            if (!shGetSystemTimeAsFileTime)
                shGetSystemTimeAsFileTime = safetyhook::create_inline(GetSystemTimeAsFileTime, GetSystemTimeAsFileTimeHook);
        }
    }

private:
    // The view's own length is used: the loader's UNICODE_STRINGs don't have to end with a zero
    static inline void invokeOnModuleLoad(std::wstring_view module_name)
    {
        auto it = GetOnModuleLoadCallbackList().find(std::wstring(module_name));
        if (it != GetOnModuleLoadCallbackList().end())
            it->second();
    }

    static inline void invokeOnUnload(std::wstring_view module_name)
    {
        auto it = GetOnModuleUnloadCallbackList().find(std::wstring(module_name));
        if (it != GetOnModuleUnloadCallbackList().end())
            it->second();
    }

    static inline void invokeOnAnyModuleLoad(HMODULE mod)
    {
        if (!GetOnAnyModuleLoadCallbackList().empty())
        {
            for (auto& f : GetOnAnyModuleLoadCallbackList())
                f(mod);
        }
    }

    static inline void invokeOnAnyModuleUnload(std::wstring_view module_name)
    {
        if (!GetOnAnyModuleUnloadCallbackList().empty())
        {
            for (auto& f : GetOnAnyModuleUnloadCallbackList())
                f(module_name);
        }
    }

    static inline void InvokeAll()
    {
        for (auto&& fn : GetOnModuleLoadCallbackList())
            fn.second();
    }

private:
    struct Comparator
    {
        bool operator() (const std::wstring& s1, const std::wstring& s2) const
        {
            std::wstring str1(s1.length(), ' ');
            std::wstring str2(s2.length(), ' ');
            std::transform(s1.begin(), s1.end(), str1.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            std::transform(s2.begin(), s2.end(), str2.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return  str1 < str2;
        }
    };

    struct ThreadParams
    {
        std::function<void()> fn;
        std::optional<hook::pattern> pattern;
        bool executed = false;
    };

    static inline auto& GetOnModuleLoadCallbackList()
    {
        static std::map<std::wstring, std::function<void()>, Comparator> onModuleLoad;
        return onModuleLoad;
    }

    static inline auto& GetOnModuleUnloadCallbackList()
    {
        static std::map<std::wstring, std::function<void()>, Comparator> onModuleUnload;
        return onModuleUnload;
    }

    static inline auto& GetOnAnyModuleLoadCallbackList()
    {
        static std::vector<std::function<void(HMODULE)>> onAnyModuleLoad;
        return onAnyModuleLoad;
    }

    static inline auto& GetOnAnyModuleUnloadCallbackList()
    {
        static std::vector<std::function<void(std::wstring_view)>> onAnyModuleUnload;
        return onAnyModuleUnload;
    }

    static inline auto& GetCallbackParamsList()
    {
        static std::vector<ThreadParams> callbackParams;
        return callbackParams;
    }

    // Recursive: a callback that registers another one already holds it
    static inline auto& GetCallbackParamsMutex()
    {
        static std::recursive_mutex callbackParamsMutex;
        return callbackParamsMutex;
    }

    static inline std::wstring_view View(PUNICODE_STRING string)
    {
        if (!string || !string->Buffer)
            return {};
        return std::wstring_view(string->Buffer, string->Length / sizeof(wchar_t));
    }

    typedef NTSTATUS(NTAPI* _LdrRegisterDllNotification) (ULONG, PVOID, PVOID, PVOID);
    typedef NTSTATUS(NTAPI* _LdrUnregisterDllNotification) (PVOID);

    typedef struct _LDR_DLL_LOADED_NOTIFICATION_DATA
    {
        ULONG Flags;                    //Reserved.
        PUNICODE_STRING FullDllName;    //The full path name of the DLL module.
        PUNICODE_STRING BaseDllName;    //The base file name of the DLL module.
        PVOID DllBase;                  //A pointer to the base address for the DLL in memory.
        ULONG SizeOfImage;              //The size of the DLL image, in bytes.
    } LDR_DLL_LOADED_NOTIFICATION_DATA, LDR_DLL_UNLOADED_NOTIFICATION_DATA, *PLDR_DLL_LOADED_NOTIFICATION_DATA, *PLDR_DLL_UNLOADED_NOTIFICATION_DATA;

    typedef union _LDR_DLL_NOTIFICATION_DATA
    {
        LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
        LDR_DLL_UNLOADED_NOTIFICATION_DATA Unloaded;
    } LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;

    typedef NTSTATUS(NTAPI* PLDR_MANIFEST_PROBER_ROUTINE)
    (
        IN HMODULE DllBase,
        IN PCWSTR FullDllPath,
        OUT PHANDLE ActivationContext
    );

    typedef NTSTATUS(NTAPI* PLDR_ACTX_LANGUAGE_ROURINE)
    (
        IN HANDLE Unk,
        IN USHORT LangID,
        OUT PHANDLE ActivationContext
    );

    typedef void(NTAPI* PLDR_RELEASE_ACT_ROUTINE)
    (
        IN HANDLE ActivationContext
    );

    typedef VOID(NTAPI* fnLdrSetDllManifestProber)
    (
        IN PLDR_MANIFEST_PROBER_ROUTINE ManifestProberRoutine,
        IN PLDR_ACTX_LANGUAGE_ROURINE CreateActCtxLanguageRoutine,
        IN PLDR_RELEASE_ACT_ROUTINE ReleaseActCtxRoutine
    );

    static inline void CALLBACK LdrDllNotification(ULONG NotificationReason, PLDR_DLL_NOTIFICATION_DATA NotificationData, PVOID Context)
    {
        static constexpr auto LDR_DLL_NOTIFICATION_REASON_LOADED = 1;
        static constexpr auto LDR_DLL_NOTIFICATION_REASON_UNLOADED = 2;
        if (NotificationReason == LDR_DLL_NOTIFICATION_REASON_LOADED)
        {
            invokeOnModuleLoad(View(NotificationData->Loaded.BaseDllName));
            invokeOnAnyModuleLoad((HMODULE)NotificationData->Loaded.DllBase);
        }
        else if (NotificationReason == LDR_DLL_NOTIFICATION_REASON_UNLOADED)
        {
            invokeOnUnload(View(NotificationData->Unloaded.BaseDllName));
            invokeOnAnyModuleUnload(View(NotificationData->Unloaded.BaseDllName));
        }
    }

    static inline NTSTATUS NTAPI ProbeCallback(IN HMODULE DllBase, IN PCWSTR FullDllPath, OUT PHANDLE ActivationContext)
    {
        //wprintf(L"ProbeCallback: Base %p, path '%ls', context %p\r\n", DllBase, FullDllPath, *ActivationContext);

        std::wstring str(FullDllPath);
        invokeOnModuleLoad(str.substr(str.find_last_of(L"/\\") + 1));
        invokeOnAnyModuleLoad(DllBase);

        //if (!*ActivationContext)
        //    return STATUS_INVALID_PARAMETER; // breaks on xp

        HANDLE actx = NULL;
        ACTCTXW act = { 0 };

        act.cbSize = sizeof(act);
        act.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_HMODULE_VALID;
        act.lpSource = FullDllPath;
        act.hModule = DllBase;
        act.lpResourceName = MAKEINTRESOURCEW(2); // ISOLATIONAWARE_MANIFEST_RESOURCE_ID, also without UNICODE

        // Reset pointer, crucial for x64 version
        *ActivationContext = 0;

        actx = CreateActCtxW(&act);

        // Report no manifest is present
        if (actx == INVALID_HANDLE_VALUE)
            return 0xC000008B; //STATUS_RESOURCE_NAME_NOT_FOUND;

        *ActivationContext = actx;

        return STATUS_SUCCESS;
    }

    static inline void RegisterDllNotification()
    {
        // The lists exist before the exit guard below, so they are destroyed after it: at exit the notification is
        // unregistered first, otherwise a DLL unloaded while the process ends would call into the destroyed lists.
        GetOnModuleLoadCallbackList();
        GetOnModuleUnloadCallbackList();
        GetOnAnyModuleLoadCallbackList();
        GetOnAnyModuleUnloadCallbackList();

        LdrRegisterDllNotification = (_LdrRegisterDllNotification)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
        if (LdrRegisterDllNotification)
        {
            if (!cookie)
            {
                LdrRegisterDllNotification(0, LdrDllNotification, 0, &cookie);
                static struct ExitGuard
                {
                    ~ExitGuard() { UnRegisterDllNotification(); }
                } exitGuard;
            }
        }
        else
        {
            LdrSetDllManifestProber = (fnLdrSetDllManifestProber)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrSetDllManifestProber");
            if (LdrSetDllManifestProber)
            {
                LdrSetDllManifestProber(&ProbeCallback, NULL, &ReleaseActCtx);
            }
        }
    }

    static inline void UnRegisterDllNotification()
    {
        LdrUnregisterDllNotification = (_LdrUnregisterDllNotification)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrUnregisterDllNotification");
        if (LdrUnregisterDllNotification && cookie)
            LdrUnregisterDllNotification(cookie);
        cookie = nullptr;
    }

    template<typename Callable>
    using ReturnType = typename decltype(std::function{ std::declval<Callable>() })::result_type;

    static inline SafetyHookInline shGetSystemTimeAsFileTime = {};
    static inline void WINAPI GetSystemTimeAsFileTimeHook(LPFILETIME lpSystemTimeAsFileTime)
    {
        // Initialization (and the CRT's mutex implementation) can query time.
        // A nested call must not lock the callback mutex or retire this hook.
        static thread_local bool dispatching = false;
        if (dispatching)
        {
            shGetSystemTimeAsFileTime.unsafe_stdcall<void>(lpSystemTimeAsFileTime);
            return;
        }
        struct DispatchGuard
        {
            bool& active;
            explicit DispatchGuard(bool& value) : active(value) { active = true; }
            ~DispatchGuard() { active = false; }
        } guard(dispatching);
        auto& threadParams = GetCallbackParamsList();

        std::lock_guard<std::recursive_mutex> lock(GetCallbackParamsMutex());

        // By index and with a copy of the function: a callback may register another one, which can move the list
        for (size_t i = 0, count = threadParams.size(); i < count; ++i)
        {
            if (!threadParams[i].executed)
            {
                if (!threadParams[i].pattern.has_value() || !threadParams[i].pattern.value().clear().empty())
                {
                    threadParams[i].executed = true;
                    auto fn = threadParams[i].fn;
                    fn();
                }
            }
        }

        shGetSystemTimeAsFileTime.unsafe_stdcall<void>(lpSystemTimeAsFileTime);

        if (std::all_of(threadParams.begin(), threadParams.end(), [](const auto& params) { return params.executed; }))
            shGetSystemTimeAsFileTime = {};
    }

    static inline DWORD WINAPI ThreadProc(LPVOID ptr)
    {
        auto paramsPtr = static_cast<CallbackHandler::ThreadParams*>(ptr);
        auto params = *paramsPtr;
        delete paramsPtr;

        HANDLE hTimer = NULL;
        LARGE_INTEGER liDueTime;
        liDueTime.QuadPart = -30 * 10000000LL;
        hTimer = CreateWaitableTimer(NULL, TRUE, NULL);
        SetWaitableTimer(hTimer, &liDueTime, 0, NULL, NULL, 0);

        while (params.pattern.value().clear().empty())
        {
            Sleep(0);

            if (WaitForSingleObject(hTimer, 0) == WAIT_OBJECT_0)
            {
                CloseHandle(hTimer);
                return 0;
            }
        };

        params.fn();

        return 0;
    }
private:
    static inline _LdrRegisterDllNotification   LdrRegisterDllNotification;
    static inline _LdrUnregisterDllNotification LdrUnregisterDllNotification;
    static inline void* cookie;
    static inline fnLdrSetDllManifestProber     LdrSetDllManifestProber;
public:
    static inline std::once_flag flag;
};
