// Replaces global operator new/delete in the engine test executable so AT-RT-1 can
// count heap use on the audio thread (ARCHITECTURE §3, debug enforcement).

#include "weft/rt/AudioThread.h"

#include <cstdlib>
#include <new>

namespace
{
void* allocate (std::size_t size)
{
    weft::rt::noteAllocation();
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void release (void* p) noexcept
{
    if (p != nullptr)
        weft::rt::noteAllocation(); // freeing on the audio thread is also a violation
    std::free (p);
}
} // namespace

void* operator new (std::size_t size) { return allocate (size); }
void* operator new[] (std::size_t size) { return allocate (size); }
void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    weft::rt::noteAllocation();
    return std::malloc (size == 0 ? 1 : size);
}
void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept
{
    weft::rt::noteAllocation();
    return std::malloc (size == 0 ? 1 : size);
}
void operator delete (void* p) noexcept { release (p); }
void operator delete[] (void* p) noexcept { release (p); }
void operator delete (void* p, std::size_t) noexcept { release (p); }
void operator delete[] (void* p, std::size_t) noexcept { release (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept { release (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { release (p); }
