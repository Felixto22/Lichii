// SPDX-License-Identifier: GPL-3.0-or-later
// (nx2) NVN directo: the game's NVN calls served on the host and replayed on Vulkan by Hookshot's renderer
// (hookshot_vk.dll), as Hookshot does, instead of through the emulated Switch 2 GPU. docs/nx2/plan-nvn-directo.md
#pragma once

#include <cstddef>
#include <cstdint>

namespace Core {
class System;
}

namespace Core::Nx2Nvn {

/// EDEN_NX2_NVN_DIRECT=1 (off by default while the path is built)
bool Enabled();

/// The write watch: a byte per 4 KiB guest page, set by every guest store (Dynarmic's EmitMemoryWrite) and by the
/// host's writes into guest memory. nullptr when the path is off.
std::uint8_t* DirtyPages();
std::uint64_t DirtyPageCount();   ///< a power of two
void MarkWritten(std::uint64_t guest, std::uint64_t size);

/// The registers a host call sees and changes, laid out as Hookshot's GuestContext (third_party/a64-kit/rt/a64.h), so
/// its NVN layer (nvn_native.c) runs on them unchanged: x[31] is SP.
struct GuestContext {
    std::uint64_t x[32];
    std::uint64_t pc;
    std::uint8_t n, z, c, v;
    int halted;
    std::uint64_t vreg[32][2];
    std::uint64_t tpidr_el0;
    std::uint64_t tpidrro_el0;
    std::uint64_t fpcr, fpsr;
    std::uint64_t excl_addr, excl_lo, excl_hi;
    int excl_valid;
};

using HostFn = void (*)(GuestContext* ctx, void* user);

/// SVC immediates from here on are host calls: svc #(kSvcBase + index)
constexpr std::uint32_t kSvcBase = 0x1000;

/// A host function the guest can call: its index (svc #(kSvcBase + index))
std::uint32_t RegisterHostCall(const char* name, HostFn fn, void* user);
/// The host call of that name (Hookshot's hos_hostcall: one per name), registered on first request
std::uint32_t FindOrRegisterHostCall(const char* name, HostFn fn, void* user);
/// The host call behind a stub address: false when it is not one
bool HostCallTarget(std::uint64_t guest, HostFn* fn, void** user);

/// The game's process is loaded: the layer's guest memory, stubs and clock come from this system
void Attach(Core::System& system);

/// (core/loader/nso.cpp) The sdk module's image before it is mapped: every exported nvn* function made a host call
/// ("svc #N; ret" at its entry), as Hookshot hooks the sdk's NVN exports, so the console's NVN driver never runs.
/// image: the module's bytes from its load base. Returns how many were patched.
std::size_t PatchSdkNvnExports(std::uint8_t* image, std::size_t size);

/// The guest address of a stub that makes that host call ("svc #N; ret"), in a page of the path's own
std::uint64_t HostCallStub(Core::System& system, std::uint32_t index);

/// (Kernel::Svc::Call) runs host call `index` on the current thread's registers; false: no such call
bool HostCall(Core::System& system, std::uint32_t index);

/// The guest base the host sees memory through (the fastmem arena): host pointer = base + guest address
std::uint8_t* GuestBase(Core::System& system);

} // namespace Core::Nx2Nvn
