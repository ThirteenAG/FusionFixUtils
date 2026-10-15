// include/ModuleList.hpp: enumeration of the process's modules, names without extension, local (the exe's folder) and
// system modules.

#include "../include/ModuleList.hpp"
#include "check.h"
#include <string>

namespace
{
    std::wstring ExeName()
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring name = path;
        name = name.substr(name.find_last_of(L"\\/") + 1);
        return name.substr(0, name.find_last_of(L'.'));
    }
}

TEST_CASE("modulelist: finds the exe and kernel32, by name without extension, in any case")
{
    ModuleList list;
    list.Enumerate();
    CHECK(!list.m_moduleList.empty());
    CHECK(list.Get(ExeName().c_str()) == GetModuleHandleW(nullptr));
    CHECK(list.Get(L"kernel32") == GetModuleHandleW(L"kernel32.dll"));
    CHECK(list.Get(L"KERNEL32") == GetModuleHandleW(L"kernel32.dll"));
    CHECK(list.Get(L"kernel32.dll") == nullptr);
    CHECK(list.Get(L"no-such-module") == nullptr);
    CHECK(list.GetAll(L"ntdll").size() == 1);
}

TEST_CASE("modulelist: local and system modules")
{
    ModuleList local;
    local.Enumerate(ModuleList::SearchLocation::LocalOnly);
    CHECK(local.Get(ExeName().c_str()) == GetModuleHandleW(nullptr));
    CHECK(local.Get(L"kernel32") == nullptr);

    ModuleList system;
    system.Enumerate(ModuleList::SearchLocation::SystemOnly);
    CHECK(system.Get(L"kernel32") != nullptr);
    CHECK(system.Get(ExeName().c_str()) == nullptr);

    for (auto& [module, name, isLocal] : local.m_moduleList)
        CHECK(isLocal);
    for (auto& [module, name, isLocal] : system.m_moduleList)
        CHECK(!isLocal);
}

TEST_CASE("modulelist: ReEnumerate sees a module loaded later, Clear empties")
{
    ModuleList list;
    list.Enumerate();
    CHECK(list.Get(L"CallbackProbe") == nullptr);
    HMODULE probe = LoadLibraryW(L"CallbackProbe.dll");
    REQUIRE(probe != nullptr);
    list.ReEnumerate();
    CHECK(list.Get(L"CallbackProbe") == probe);
    FreeLibrary(probe);
    list.Clear();
    CHECK(list.m_moduleList.empty());
}
