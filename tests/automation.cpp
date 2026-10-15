// automation/asi_automation_frames.h: the frame-export contract between a fix and the dev-only automation plugin. The
// fix exports AsiAutomation_SetFrameCallback (ASI_AUTOMATION_FRAMES_EXPORT), the plugin finds it by name and registers
// a callback per stage, Fire() calls it with the device and the plugin's context, nothing happens without one.
// Automation.hpp and AutomationCapture.hpp need a game and a device: the standalone header check compiles them.

#include <windows.h>
#include "../automation/asi_automation_frames.h"
#include "check.h"

// This exe plays the fix
ASI_AUTOMATION_FRAMES_EXPORT()

namespace
{
    struct Seen
    {
        int calls = 0;
        int stage = -1;
        void* device = nullptr;
    };

    void __cdecl Record(int stage, void* device, void* context)
    {
        auto seen = static_cast<Seen*>(context);
        ++seen->calls;
        seen->stage = stage;
        seen->device = device;
    }

    void Reset()
    {
        for (int stage = 0; stage < ASI_AUTOMATION_STAGE_COUNT; ++stage)
            asi_automation_frames::Set(stage, nullptr, nullptr);
    }
}

TEST_CASE("automation: the stages and the export name are the contract")
{
    CHECK(ASI_AUTOMATION_STAGE_BEFORE_UI == 0);
    CHECK(ASI_AUTOMATION_STAGE_AFTER_UI == 1);
    CHECK(ASI_AUTOMATION_STAGE_COUNT == 2);
    CHECK(std::string(ASI_AUTOMATION_FRAMES_EXPORT_NAME) == "AsiAutomation_SetFrameCallback");
}

TEST_CASE("automation: nothing listens until a callback is set, Fire does nothing then")
{
    Reset();
    for (int stage = 0; stage < ASI_AUTOMATION_STAGE_COUNT; ++stage)
        CHECK(!asi_automation_frames::Wanted(stage));
    asi_automation_frames::Fire(ASI_AUTOMATION_STAGE_BEFORE_UI, nullptr);
    CHECK(!asi_automation_frames::Wanted(-1));
    CHECK(!asi_automation_frames::Wanted(ASI_AUTOMATION_STAGE_COUNT));
}

TEST_CASE("automation: the plugin registers through the export it finds by name")
{
    Reset();
    auto set = reinterpret_cast<AsiAutomationSetFrameCallbackFn>(GetProcAddress(GetModuleHandleW(nullptr), ASI_AUTOMATION_FRAMES_EXPORT_NAME));
    REQUIRE(set != nullptr);

    Seen before, after;
    CHECK(set(ASI_AUTOMATION_STAGE_BEFORE_UI, Record, &before) == 1);
    CHECK(set(ASI_AUTOMATION_STAGE_AFTER_UI, Record, &after) == 1);
    CHECK(asi_automation_frames::Wanted(ASI_AUTOMATION_STAGE_BEFORE_UI));
    CHECK(asi_automation_frames::Wanted(ASI_AUTOMATION_STAGE_AFTER_UI));

    int device = 0;
    asi_automation_frames::Fire(ASI_AUTOMATION_STAGE_BEFORE_UI, &device);
    CHECK(before.calls == 1 && before.stage == ASI_AUTOMATION_STAGE_BEFORE_UI && before.device == &device);
    CHECK(after.calls == 0);
    asi_automation_frames::Fire(ASI_AUTOMATION_STAGE_AFTER_UI, &device);
    CHECK(after.calls == 1 && after.stage == ASI_AUTOMATION_STAGE_AFTER_UI);

    // Unknown stages: refused, ignored
    CHECK(set(ASI_AUTOMATION_STAGE_COUNT, Record, &before) == 0);
    CHECK(set(-1, Record, &before) == 0);
    asi_automation_frames::Fire(ASI_AUTOMATION_STAGE_COUNT, &device);
    asi_automation_frames::Fire(-1, &device);
    CHECK(before.calls == 1);

    // A null callback unregisters
    CHECK(set(ASI_AUTOMATION_STAGE_BEFORE_UI, nullptr, nullptr) == 1);
    CHECK(!asi_automation_frames::Wanted(ASI_AUTOMATION_STAGE_BEFORE_UI));
    asi_automation_frames::Fire(ASI_AUTOMATION_STAGE_BEFORE_UI, &device);
    CHECK(before.calls == 1);

    // A new registration replaces the old one with its context
    Seen other;
    set(ASI_AUTOMATION_STAGE_AFTER_UI, Record, &other);
    asi_automation_frames::Fire(ASI_AUTOMATION_STAGE_AFTER_UI, nullptr);
    CHECK(other.calls == 1 && after.calls == 1);
    Reset();
}
