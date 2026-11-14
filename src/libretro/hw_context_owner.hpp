#pragma once

// The frontend's hardware context belongs to one core instance per process. RetroArch's run-ahead second instance
// ("Use Second Instance") is another copy of this library loaded into the same process (a copy of the file, so the
// copies share no globals). RetroArch shows that copy's frames, and it never gets a context_reset: 1.22.2 even accepts
// its SET_HW_RENDER and hands its callbacks to the main instance's context. A copy that would render through the
// hardware context therefore declines to load while another copy holds it, and RetroArch falls back to
// single-instance run-ahead.
//
// The claim lives in a small block of memory that every copy in the process finds: the first claim allocates it from
// the operating system (never freed, so it outlives the copy that made it) and publishes its address in the process
// environment variable kHwContextRegistryVariable as "<process id>:<address>:<nonce>". The environment is written once
// per process image (later claims and releases are atomic operations on the block), so setenv's lack of thread safety
// on POSIX matters only for that first write. A child process inherits the variable, not the block: the process id
// tells them apart. exec keeps both the id and the environment (RetroArch's "Restart RetroArch" on Linux): the address
// is only read through the kernel (no fault on an unmapped page) and taken only if it holds the magic and the
// variable's random nonce, so a stale value means no registry. The block's layout is versioned in the variable name.

#include <cstdint>

namespace brimir::libretro {

inline constexpr const char* kHwContextRegistryVariable = "BRIMIR_HW_CONTEXT_V1";

// self: a token unique to this loaded copy of the library (the address of one of its globals).
// Another copy holds the hardware context.
bool HwContextHeldByOther(const void* self);
// This copy holds it (its SET_HW_RENDER was accepted). Creates the process's registry on first use.
void ClaimHwContext(const void* self);
// Drops the claim only when self holds it (a declined copy unloading leaves the main instance's claim alone).
void ReleaseHwContext(const void* self);

} // namespace brimir::libretro
