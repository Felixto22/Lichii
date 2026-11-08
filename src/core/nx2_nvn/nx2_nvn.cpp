// SPDX-License-Identifier: GPL-3.0-or-later
// (nx2) NVN directo: the host-call bridge and the write watch. docs/nx2/plan-nvn-directo.md

#include "core/nx2_nvn/nx2_nvn.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
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

#ifndef _WIN32
// (the Hookshot layer is built on Windows only, where hookshot_vk.dll is)
extern "C" {
static std::uint8_t* nx2_guest_base = nullptr;
static std::uint8_t* a64_dirty = nullptr;
static std::uint32_t nx2_nvn_proc_index(const char*) { return 0; }
}
namespace Core::Nx2Nvn { Core::System* g_system = nullptr; }
#else
extern "C" {
extern std::uint8_t* nx2_guest_base;   // (nx2_hos.cpp: Hookshot's A64_MEM)
extern std::uint8_t* a64_dirty;
std::uint32_t nx2_nvn_proc_index(const char* name);   // (hookshot/host/nvn_native.c)
}
#endif

namespace Core::Nx2Nvn {

extern Core::System* g_system;   // (nx2_hos.cpp)

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
std::unordered_map<std::string, std::uint32_t> calls_by_name;

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

std::uint32_t FindOrRegisterHostCall(const char* name, HostFn fn, void* user) {
    std::scoped_lock lk{calls_mu};
    const std::string key = name ? name : "";
    if (const auto it = calls_by_name.find(key); it != calls_by_name.end()) {
        return it->second;
    }
    calls.push_back(Call{key, fn, user});
    const auto i = static_cast<std::uint32_t>(calls.size() - 1);
    calls_by_name.emplace(key, i);
    return i;
}

bool HostCallTarget(std::uint64_t guest, HostFn* fn, void** user) {
    std::uint32_t index = ~0u;
    {
        std::scoped_lock lk{stubs_mu};
        for (std::size_t p = 0; p < stub_pages.size(); ++p) {
            if (guest >= stub_pages[p] && guest < stub_pages[p] + 4096 && (guest & 7) == 0) {
                index = static_cast<std::uint32_t>(p * kStubsPerPage + (guest - stub_pages[p]) / 8);
                break;
            }
        }
    }
    std::scoped_lock lk{calls_mu};
    if (index >= calls.size()) {
        return false;
    }
    *fn = calls[index].fn;
    *user = calls[index].user;
    return true;
}

void Attach(Core::System& system) {
    g_system = &system;
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

    if (!nx2_guest_base) {
        nx2_guest_base = GuestBase(system);
        a64_dirty = DirtyPages();
        g_system = &system;
    }
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

namespace Core::Nx2Nvn {

namespace {
template <typename T>
T Read(const std::uint8_t* image, std::size_t size, std::size_t off) {
    T v{};
    if (off + sizeof(T) <= size) {
        std::memcpy(&v, image + off, sizeof(T));
    }
    return v;
}
void Write32(std::uint8_t* image, std::size_t off, std::uint32_t v) {
    std::memcpy(image + off, &v, 4);
}
} // namespace

std::size_t PatchSdkNvnExports(std::uint8_t* image, std::size_t size) {
    if (!Enabled() || size < 0x100) {
        return 0;
    }
    // MOD0 (its offset at +4 of the module) -> the dynamic section -> the symbol and string tables
    const std::uint32_t mod0 = Read<std::uint32_t>(image, size, 4);
    if (Read<std::uint32_t>(image, size, mod0) != 0x30444F4D) {   // "MOD0"
        LOG_ERROR(HW_GPU, "nx2 nvn: the sdk has no MOD0 header: its NVN exports stay");
        return 0;
    }
    const std::size_t dynamic = mod0 + static_cast<std::int32_t>(Read<std::uint32_t>(image, size, mod0 + 4));
    std::uint64_t symtab = 0, strtab = 0, strsz = 0, hash = 0, syment = 24;
    for (std::size_t d = dynamic; d + 16 <= size; d += 16) {
        const auto tag = Read<std::int64_t>(image, size, d);
        const auto val = Read<std::uint64_t>(image, size, d + 8);
        if (tag == 0) break;
        if (tag == 6) symtab = val;
        if (tag == 5) strtab = val;
        if (tag == 10) strsz = val;
        if (tag == 4) hash = val;
        if (tag == 11) syment = val;
    }
    if (!symtab || !strtab || syment != 24) {
        LOG_ERROR(HW_GPU, "nx2 nvn: the sdk's symbol table not found: its NVN exports stay");
        return 0;
    }
    const std::uint64_t count = hash ? Read<std::uint32_t>(image, size, hash + 4)   // (DT_HASH's nchain)
                                     : (strtab > symtab ? (strtab - symtab) / 24 : 0);
    struct Export {
        std::uint64_t at, size;
        std::string name;
    };
    std::vector<Export> exports;
    for (std::uint64_t i = 1; i < count; ++i) {
        const std::size_t e = symtab + i * 24;
        const auto name_off = Read<std::uint32_t>(image, size, e);
        const auto info = Read<std::uint8_t>(image, size, e + 4);
        const auto shndx = Read<std::uint16_t>(image, size, e + 6);
        const auto value = Read<std::uint64_t>(image, size, e + 8);
        const auto sym_size = Read<std::uint64_t>(image, size, e + 16);
        if ((info & 0xF) != 2 || shndx == 0 || value == 0 || value + 4 > size || name_off >= strsz) {
            continue;   // (functions defined here only)
        }
        const char* name = reinterpret_cast<const char*>(image + strtab + name_off);
        if (std::strncmp(name, "nvn", 3) != 0) {
            continue;
        }
        exports.push_back(Export{value, sym_size, name});
    }
    std::size_t patched = 0, thunks = 0, skipped = 0;
    for (const Export& x : exports) {
        std::uint64_t at = x.at, room = x.size;
        // a short export (a thunk "b impl"): the function it branches to
        for (int hop = 0; room < 8 && hop < 4; ++hop) {
            const std::uint32_t insn = Read<std::uint32_t>(image, size, at);
            if ((insn & 0xFC000000u) != 0x14000000u) {
                break;
            }
            const std::int64_t imm = static_cast<std::int64_t>(static_cast<std::int32_t>(insn << 6) >> 6) * 4;
            at = static_cast<std::uint64_t>(static_cast<std::int64_t>(at) + imm);
            room = 8;   // (an implementation: a function of its own, longer than two instructions)
            ++thunks;
        }
        if (room < 8 || at + 8 > size) {
            ++skipped;
            LOG_WARNING(HW_GPU, "nx2 nvn: sdk export {} not patched ({} bytes)", x.name, x.size);
            continue;
        }
        const std::uint32_t index = nx2_nvn_proc_index(x.name.c_str());
        Write32(image, at, SvcInstruction(kSvcBase + index));
        Write32(image, at + 4, kRet);
        ++patched;
    }
    LOG_INFO(HW_GPU, "nx2 nvn: {} sdk NVN exports made host calls ({} through thunks, {} left)", patched, thunks,
             skipped);
    return patched;
}

} // namespace Core::Nx2Nvn
