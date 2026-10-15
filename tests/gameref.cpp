// include/gameref.hpp: resolution (now, deferred, failed), writes through the reference (ProtectedGameRef to a read-only
// page), operators and the assignment between references, which writes the value (it doesn't rebind the reference).

#include <windows.h>
#include "../include/gameref.hpp"
#include "check.h"

namespace
{
    struct Page
    {
        void* memory = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        ~Page() { VirtualFree(memory, 0, MEM_RELEASE); }
        template<typename T> T* At(size_t offset) { return reinterpret_cast<T*>(static_cast<uint8_t*>(memory) + offset); }
        void Protect(DWORD protection) { DWORD old; VirtualProtect(memory, 4096, protection, &old); }
        DWORD Protection()
        {
            MEMORY_BASIC_INFORMATION info{};
            VirtualQuery(memory, &info, sizeof(info));
            return info.Protect;
        }
    };

    void Write(GameRef<int>& ref, int value)
    {
        ref = value;
    }

    // Whether writing through a plain GameRef faults (no C++ objects in a function with __try)
    bool WriteFaults(GameRef<int>& ref, int value)
    {
        __try
        {
            Write(ref, value);
            return false;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            return true;
        }
    }

    struct Vec
    {
        float x, y;
    };
}

TEST_CASE("gameref: reads and writes the game's value")
{
    Page page;
    auto value = page.At<int>(0);
    *value = 10;
    GameRef<int> ref(value);
    CHECK(ref.is_initialized());
    CHECK(ref.get_ptr() == value);
    CHECK(ref == 10);
    int read = ref;
    CHECK(read == 10);

    ref = 5;
    CHECK(*value == 5);
    ref += 3;
    CHECK(*value == 8);
    ref -= 1;
    ref *= 4;
    ref /= 2;
    CHECK(*value == 14);
    ref %= 5;
    CHECK(*value == 4);
    ref |= 3;
    ref &= 6;
    ref ^= 1;
    CHECK(*value == 7);
    ref <<= 2;
    ref >>= 1;
    CHECK(*value == 14);
    CHECK(++ref == 15);
    CHECK(ref++ == 15);
    CHECK(*value == 16);
    CHECK(--ref == 15);
    CHECK(ref-- == 15);
    CHECK(*value == 14);
    CHECK(ref + 1 == 15);
    CHECK(-ref == -14);
    CHECK(ref > 13 && ref >= 14 && ref < 15 && ref <= 14 && ref != 13);
    // Mixed signedness compares by value
    CHECK(ref == 14u);
    CHECK(!(ref < 0u));
    CHECK(*ref == 14);
    CHECK(static_cast<bool>(ref));
}

TEST_CASE("gameref: ProtectedGameRef writes to a read-only page, a plain GameRef doesn't")
{
    Page page;
    auto value = page.At<int>(64);
    *value = 1;
    page.Protect(PAGE_READONLY);

    GameRef<int> plain(value);
    CHECK(plain == 1);
    CHECK(WriteFaults(plain, 2));
    CHECK(*value == 1);

    ProtectedGameRef<int> ref(value);
    ref = 3;
    CHECK(*value == 3);
    CHECK(page.Protection() == PAGE_EXECUTE_READWRITE);

    page.Protect(PAGE_READONLY);
    ref += 4;
    CHECK(*value == 7);
}

TEST_CASE("gameref: a reference that isn't found yet resolves at its first use")
{
    Page page;
    auto value = page.At<float>(128);
    *value = 2.5f;
    bool ready = false;
    int resolves = 0;
    GameRef<float> ref([&]() -> float* { ++resolves; return ready ? value : nullptr; });
    CHECK(resolves == 1);
    CHECK(ref.is_initialized());
    ready = true;
    CHECK(ref == 2.5f);
    CHECK(resolves == 2);
    ref = 4.0f;
    CHECK(*value == 4.0f);
    CHECK(resolves == 2);
}

TEST_CASE("gameref: a reference that is never found throws instead of writing to null")
{
    GameRef<int> ref([]() -> int* { return nullptr; });
    CHECK_THROWS(ref.get());
    CHECK_THROWS(ref = 1);
    CHECK_THROWS(GameRef<int>(static_cast<int*>(nullptr)));
    GameRef<int> unset;
    CHECK(!unset.is_initialized());
    CHECK_THROWS(unset.SetAddress(static_cast<int*>(nullptr)));
    CHECK_THROWS(unset.SetAddress(uintptr_t(0)));
#ifdef NDEBUG
    // (Debug builds stop at the assertion first)
    CHECK_THROWS(unset.get());
#endif
}

TEST_CASE("gameref: SetAddress takes pointers and integers")
{
    Page page;
    auto value = page.At<int>(256);
    *value = 9;
    GameRef<int> ref;
    ref.SetAddress(reinterpret_cast<uintptr_t>(value));
    CHECK(ref == 9);
    ref.SetAddress(value + 1);
    ref = 3;
    CHECK(value[1] == 3);
    CHECK(value[0] == 9);
}

TEST_CASE("gameref: assigning a reference writes its value, copies and moves keep the address")
{
    Page page;
    auto a = page.At<int>(0);
    auto b = page.At<int>(4);
    *a = 1;
    *b = 7;
    GameRef<int> ra(a), rb(b);
    ra = rb;
    CHECK(*a == 7);
    CHECK(ra.get_ptr() == a);
    *b = 8;
    ra = std::move(rb);
    CHECK(*a == 8);
    CHECK(ra.get_ptr() == a);

    GameRef<int> copy(ra);
    CHECK(copy.get_ptr() == a);
    GameRef<int> moved(std::move(copy));
    CHECK(moved.get_ptr() == a);
    moved = 11;
    CHECK(*a == 11);
}

TEST_CASE("gameref: members and pointers")
{
    Page page;
    auto v = page.At<Vec>(512);
    *v = { 1.0f, 2.0f };
    GameRef<Vec> ref(v);
    CHECK(ref->y == 2.0f);
    ref->x = 3.0f;
    CHECK(v->x == 3.0f);

    auto pointer = page.At<Vec*>(600);
    *pointer = v;
    GameRef<Vec*> refp(pointer);
    CHECK(refp->x == 3.0f);
}
