#pragma once

// Frame stages for the dev-only automation plugin: points of a frame where it can grab the image.
//
// A fix calls Fire() at the points of a frame it already hooks:
//   ASI_AUTOMATION_STAGE_BEFORE_UI  the finished 3D image after the fix's post processing and anti-aliasing, before the HUD
//   ASI_AUTOMATION_STAGE_AFTER_UI   the final image, right before Present / EndScene
// and exports the registration once (ASI_AUTOMATION_FRAMES_EXPORT() in one .cpp of the fix). Nothing happens unless
// the automation plugin registers a callback, which it only does when it was enabled on a developer's machine: Fire()
// is one atomic load otherwise. The fix's behaviour doesn't change either way.
//
// device: what the stage draws with, as IUnknown*: an IDXGISwapChain (Direct3D 10/11) or an IDirect3DDevice9. The
// callback runs on the thread that draws (the render thread), with the device's state as the fix left it.

#include <atomic>

#define ASI_AUTOMATION_FRAMES_EXPORT_NAME "AsiAutomation_SetFrameCallback"

enum
{
    ASI_AUTOMATION_STAGE_BEFORE_UI = 0,
    ASI_AUTOMATION_STAGE_AFTER_UI = 1,
    ASI_AUTOMATION_STAGE_COUNT = 2,
};

typedef void(__cdecl* AsiAutomationFrameCallback)(int stage, void* device, void* context);
// Returns 1 when the stage is known; a null callback unregisters
typedef int(__cdecl* AsiAutomationSetFrameCallbackFn)(int stage, AsiAutomationFrameCallback callback, void* context);

namespace asi_automation_frames
{
    struct Slot
    {
        std::atomic<AsiAutomationFrameCallback> callback{ nullptr };
        std::atomic<void*> context{ nullptr };
    };

    inline Slot slots[ASI_AUTOMATION_STAGE_COUNT];

    // Whether someone listens, for fixes that have to fetch the device first
    inline bool Wanted(int stage)
    {
        return stage >= 0 && stage < ASI_AUTOMATION_STAGE_COUNT && slots[stage].callback.load(std::memory_order_relaxed) != nullptr;
    }

    inline void Fire(int stage, void* device)
    {
        if (stage < 0 || stage >= ASI_AUTOMATION_STAGE_COUNT)
            return;
        auto callback = slots[stage].callback.load(std::memory_order_acquire);
        if (callback)
            callback(stage, device, slots[stage].context.load(std::memory_order_relaxed));
    }

    inline int Set(int stage, AsiAutomationFrameCallback callback, void* context)
    {
        if (stage < 0 || stage >= ASI_AUTOMATION_STAGE_COUNT)
            return 0;
        slots[stage].callback.store(nullptr, std::memory_order_release);
        slots[stage].context.store(context, std::memory_order_relaxed);
        slots[stage].callback.store(callback, std::memory_order_release);
        return 1;
    }
}

// The export, in exactly one .cpp of the fix
#define ASI_AUTOMATION_FRAMES_EXPORT() \
    extern "C" __declspec(dllexport) int __cdecl AsiAutomation_SetFrameCallback(int stage, AsiAutomationFrameCallback callback, void* context) \
    { \
        return asi_automation_frames::Set(stage, callback, context); \
    }
