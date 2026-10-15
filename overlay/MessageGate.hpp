#pragma once
// The window procedure's way into the overlay (Overlay::Message). The window procedure runs on the game's window
// thread, also from inside the D3D9 runtime: on a focus change in exclusive fullscreen the runtime calls the window
// procedure from a Present or Reset of another thread's device, and waits for it. When that other thread (the render
// thread in Overlay::Present) holds the overlay's lock, a window procedure that waits for the lock deadlocks the game
// (seen with Alt+Tab on a loading screen). So the window procedure only tries the lock: a message it can't take now
// is left to the game, and a focus loss is remembered and closes the overlay with the next frame (TakePendingClose).
// No ImGui here: the tests use it as it is.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace asi_overlay
{
    class MessageGate
    {
    public:
        explicit MessageGate(std::recursive_mutex& mutex) : mutex(mutex) {}

        // Tries the overlay's lock for one window message, never waits. True: lock owns it, the message is handled.
        // False: the overlay is busy on another thread, the message isn't the overlay's now; a lost focus is kept.
        bool Enter(std::unique_lock<std::recursive_mutex>& lock, UINT msg, WPARAM wp)
        {
            lock = std::unique_lock<std::recursive_mutex>(mutex, std::try_to_lock);
            if (lock.owns_lock())
                return true;
            if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATEAPP && !wp))
                pendingClose = true;
            ++dropped;
            return false;
        }

        // A focus loss seen while the lock was busy: once, by the frame that follows
        bool TakePendingClose() { return pendingClose.exchange(false); }

        // Messages left to the game because the lock was busy
        uint32_t Dropped() const { return dropped.load(); }

    private:
        std::recursive_mutex& mutex;
        std::atomic<bool> pendingClose = false;
        std::atomic<uint32_t> dropped = 0;
    };
}
