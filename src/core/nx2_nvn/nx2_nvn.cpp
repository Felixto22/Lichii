// SPDX-License-Identifier: GPL-3.0-or-later
// (nx2) NVN directo: the host-call bridge and the write watch. docs/nx2/plan-nvn-directo.md

#include "core/nx2_nvn/nx2_nvn.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/logging.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc_types.h"
#include "core/memory.h"

namespace Core::Nx2Nvn {

namespace {

constexpr std::uint64_t kDirtyPages = std::uint64_t{1} << 27;   // (39-bit guest addresses, 4 KiB pages)

std::uint8_t* AllocDirty() {
#ifdef _WIN32
    // (committed as touched: the pages of the array the game's stores reach)
    return static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kDirtyPages, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    return static_cast<std::uint8_t*>(std::calloc(kDirtyPages, 1));
#endif
}

struct Call {
    std::string name;
    HostFn fn;
    void* user;
};
std::mutex calls_mu;
std::vector<Call> calls;

// The stub pages: "svc #N; ret" (8 bytes) a call, in executable guest memory of the path's own
constexpr std::size_t kStubsPerPage = 4096 / 8;
std::mutex stubs_mu;
std::vector<std::uint64_t> stub_pages;   // (guest address of each page)

std::uint32_t SvcInstruction(std::uint32_t imm16) {
    return 0xD4000001u | ((imm16 & 0xFFFFu) << 5);   // SVC #imm16
}
constexpr std::uint32_t kRet = 0xD65F03C0u;

} // namespace

bool Enabled() {
    static const bool on = [] {
        const char* e = std::getenv("EDEN_NX2_NVN_DIRECT");
        return e != nullptr && std::atoi(e) != 0;
    }();
    return on;
}

std::uint8_t* DirtyPages() {
    static std::uint8_t* const pages = Enabled() ? AllocDirty() : nullptr;
    return pages;
}

std::uint64_t DirtyPageCount() {
    return DirtyPages() ? kDirtyPages : 0;
}

void MarkWritten(std::uint64_t guest, std::uint64_t size) {
    std::uint8_t* const d = DirtyPages();
    if (!d || size == 0) {
        return;
    }
    const std::uint64_t first = guest >> 12, last = (guest + size - 1) >> 12;
    for (std::uint64_t p = first; p <= last; ++p) {
        d[p & (kDirtyPages - 1)] = 1;
    }
}

std::uint32_t RegisterHostCall(const char* name, HostFn fn, void* user) {
    std::scoped_lock lk{calls_mu};
    calls.push_back(Call{name ? name : "", fn, user});
    return static_cast<std::uint32_t>(calls.size() - 1);
}

std::uint8_t* GuestBase(Core::System& system) {
    auto* process = system.ApplicationProcess();
    if (!process) {
        return nullptr;
    }
    return process->GetPageTable().GetImpl().fastmem_arena;
}

std::uint64_t HostCallStub(Core::System& system, std::uint32_t index) {
    std::scoped_lock lk{stubs_mu};
    const std::size_t page = index / kStubsPerPage;
    auto* process = system.ApplicationProcess();
    if (!process) {
        return 0;
    }
    while (stub_pages.size() <= page) {
        Kernel::KProcessAddress addr{};
        const Result r = process->GetPageTable().MapPages(std::addressof(addr), 1, Kernel::KMemoryState::Static,
                                                          Kernel::KMemoryPermission::UserReadExecute);
        if (r.IsError()) {
            LOG_ERROR(HW_GPU, "nx2 nvn: no page for host-call stubs");
            return 0;
        }
        const std::uint64_t base = GetInteger(addr);
        // every stub of the page written at once: its calls registered later still land on "svc; ret"
        std::uint32_t words[1024];
        for (std::size_t i = 0; i < kStubsPerPage; ++i) {
            const std::uint32_t n = static_cast<std::uint32_t>(stub_pages.size() * kStubsPerPage + i);
            words[i * 2] = SvcInstruction(kSvcBase + n);
            words[i * 2 + 1] = kRet;
        }
        system.ApplicationMemory().WriteBlock(base, words, sizeof words);
        stub_pages.push_back(base);
    }
    return stub_pages[page] + (index % kStubsPerPage) * 8;
}

bool HostCall(Core::System& system, std::uint32_t index) {
    Call call;
    {
        std::scoped_lock lk{calls_mu};
        if (index >= calls.size()) {
            return false;
        }
        call = calls[index];
    }
    auto& kernel = system.Kernel();
    auto& core = kernel.CurrentPhysicalCore();
    auto* process = GetCurrentProcessPointer(kernel);
    auto* arm = process->GetArmInterface(core.CoreIndex());

    Kernel::Svc::ThreadContext tc{};
    arm->GetContext(tc);
    GuestContext ctx{};
    for (int i = 0; i < 29; ++i) {
        ctx.x[i] = tc.r[i];
    }
    ctx.x[29] = tc.fp;
    ctx.x[30] = tc.lr;
    ctx.x[31] = tc.sp;
    ctx.pc = tc.pc;
    for (int i = 0; i < 32; ++i) {
        std::memcpy(ctx.vreg[i], &tc.v[i], 16);
    }
    ctx.tpidr_el0 = tc.tpidr;
    ctx.fpcr = tc.fpcr;
    ctx.fpsr = tc.fpsr;

    call.fn(&ctx, call.user);

    for (int i = 0; i < 29; ++i) {
        tc.r[i] = ctx.x[i];
    }
    tc.fp = ctx.x[29];
    tc.lr = ctx.x[30];
    tc.sp = ctx.x[31];
    for (int i = 0; i < 32; ++i) {
        std::memcpy(&tc.v[i], ctx.vreg[i], 16);
    }
    arm->SetContext(tc);
    return true;
}

} // namespace Core::Nx2Nvn
