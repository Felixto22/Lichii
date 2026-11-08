// SPDX-License-Identifier: GPL-3.0-or-later
// (nx2) NVN directo: what Hookshot's host NVN layer (hookshot/host/*.c) asks of its OS layer (host/hos_*.c), made
// from Lichii's: guest memory through the fastmem arena, host calls through the SVC bridge, a window of its own for
// hookshot_vk.dll, the frame pacing of nn_vi.c's native window. docs/nx2/plan-nvn-directo.md

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_memory_block.h"
#include "core/hle/kernel/k_process.h"
#include "core/nx2_nvn/nx2_nvn.h"

extern "C" {
#include "core/nx2_nvn/hookshot/host/a64.h"
}

namespace Core::Nx2Nvn {
Core::System* g_system = nullptr;   // (Attach)
}

extern "C" {

// ---- guest memory and the write watch ----
uint8_t* nx2_guest_base = nullptr;
uint8_t* a64_dirty = nullptr;
float hos_aspect_ratio = 0.0f;
volatile uint64_t hos_mkw_pacer = 0;
volatile long long hos_present_count = 0;

uint8_t* hos_guest_base(void) {
    return nx2_guest_base;
}
uint64_t hos_guest_size(void) {
    return 1ull << 39;
}
void hos_mark_written(const void* p, size_t n) {
    if (!nx2_guest_base || !p) {
        return;
    }
    Core::Nx2Nvn::MarkWritten(static_cast<uint64_t>(static_cast<const uint8_t*>(p) - nx2_guest_base), n);
}
uint64_t hos_reserved_cpus(void) {
    return 0;
}

// guest memory of the layer's own (its objects' storage, the renderer's guest_alloc): pages mapped read-write
static uint64_t AllocGuest(uint64_t size, uint64_t align) {
    auto* system = Core::Nx2Nvn::g_system;
    if (!system || !system->ApplicationProcess()) {
        return 0;
    }
    (void)align;   // (page-aligned at least; the layer asks for 4 KiB or 64 KiB)
    const size_t pages = static_cast<size_t>((size + 0xFFFF) & ~0xFFFFull) / 4096;
    Kernel::KProcessAddress addr{};
    const Result r = system->ApplicationProcess()->GetPageTable().MapPages(
        std::addressof(addr), pages, Kernel::KMemoryState::Static, Kernel::KMemoryPermission::UserReadWrite);
    if (r.IsError()) {
        LOG_ERROR(HW_GPU, "nx2 nvn: no guest memory of {:#x} bytes", size);
        return 0;
    }
    const uint64_t a = GetInteger(addr);
    std::memset(nx2_guest_base + a, 0, pages * 4096);
    return a;
}
uint64_t hos_region_alloc(uint64_t size, uint64_t align) {
    return AllocGuest(size, align);
}
uint64_t hos_sys_alloc(uint64_t size, uint64_t align) {
    return AllocGuest(size, align);
}

// ---- logging ----
void hos_log(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    LOG_INFO(HW_GPU, "nx2 nvn: {}", line);
}
void hos_log_flush(void) {}
void hos_fatal(Ctx* c, const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    LOG_CRITICAL(HW_GPU, "nx2 nvn: fatal: {} (lr {:#x})", line, c ? c->x[30] : 0);
    std::abort();
}
void hos_backtrace(void) {}
int hos_game_is_mkw(void) {
    return 0;
}

// ---- threads (Hookshot's virtual cores: none here) ----
void* hos_self(void) {
    return nullptr;
}
const char* hos_thread_name(void* t) {
    (void)t;
    return "guest";
}
int hos_vcore_away(void) {
    return 0;
}
void hos_vcore_back(void) {}

// ---- time: the guest tick at HOS_TICK_HZ (31.25 MHz) from the emulated clock, host nanoseconds ----
uint64_t hos_ticks(void) {
    auto* system = Core::Nx2Nvn::g_system;
    if (!system) {
        return 0;
    }
    return static_cast<uint64_t>(system->CoreTiming().GetGlobalTimeNs().count()) / 32;
}
int64_t hos_now_ns(void) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void hos_sleep_ns(int64_t ns) {
    if (ns > 0) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }
}

// ---- host calls (hos_hostcall.c): one per name, a guest stub each ----
uint32_t nx2_hostcall_index(const char* name, void (*fn)(Ctx*, void*), void* user) {
    return Core::Nx2Nvn::FindOrRegisterHostCall(name, reinterpret_cast<Core::Nx2Nvn::HostFn>(fn), user);
}
uint64_t hos_hostcall(const char* name, void (*fn)(Ctx*, void*), void* user) {
    const uint32_t i = nx2_hostcall_index(name, fn, user);
    return Core::Nx2Nvn::g_system ? Core::Nx2Nvn::HostCallStub(*Core::Nx2Nvn::g_system, i) : 0;
}
uint64_t hos_hostcall_target(uint64_t addr, void (**fn)(Ctx*, void*), void** user) {
    Core::Nx2Nvn::HostFn f = nullptr;
    if (!Core::Nx2Nvn::HostCallTarget(addr, &f, user)) {
        return 0;
    }
    *fn = reinterpret_cast<void (*)(Ctx*, void*)>(f);
    return addr;
}

// ---- the Insert menu's controls (not in Lichii) ----
uint32_t hos_pad_raw(void) {
    return 0;
}
int hos_binding(int set, int button, int slot) {
    (void)set, (void)button, (void)slot;
    return 0;
}
void hos_bind(int set, int button, int slot, int code) {
    (void)set, (void)button, (void)slot, (void)code;
}

// ---- the native window's pacing (nn_vi.c): a frame shown one present interval after the last ----
static std::atomic<int> present_interval{1};
static std::atomic<int64_t> next_frame_at{0};
int hos_vi_native_interval(Ctx* c, int requested) {
    (void)c;
    present_interval = requested < 1 ? 1 : requested > 4 ? 4 : requested;
    return requested;
}
void hos_vi_native_acquire(int active) {
    (void)active;
    static const bool no_pacing = std::getenv("HOS_NO_PACING") != nullptr;
    const int64_t until = next_frame_at.load();
    const int64_t now = hos_now_ns();
    if (!no_pacing && until > now) {
        hos_sleep_ns(until - now);
    }
}
void hos_vi_native_presented(void) {
    const int64_t period = 1000000000LL / 60 * present_interval.load(), now = hos_now_ns();
    const int64_t at = next_frame_at.load();
    next_frame_at = at && now - at < period ? at + period : now + period;
    Core::g_nx2_present_count.fetch_add(1, std::memory_order_relaxed);
}

// ---- the renderer's window (display.c): its own, the replay presents there ----
int rx_start(void* hwnd);
void rx_resize(int width, int height);
void hos_watch_guest_writes(const uint8_t* data, uint64_t size) {
    (void)data, (void)size;
}

#ifdef _WIN32
static LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        rx_resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_CLOSE:
        ShowWindow(w, SW_MINIMIZE);   // (the emulator stops the game; this window only shows it)
        return 0;
    default:
        return DefWindowProcW(w, msg, wp, lp);
    }
}
#endif

void hos_renderer_wait(void) {
    static std::once_flag once;
    static std::mutex mu;
    static std::condition_variable cv;
    static bool done = false;
    std::call_once(once, [] {
#ifdef _WIN32
        std::thread([] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = WndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = L"LichiiNvnDirecto";
            RegisterClassW(&wc);
            HWND w = CreateWindowExW(0, wc.lpszClassName, L"Lichii - NVN directo", WS_OVERLAPPEDWINDOW,
                                     CW_USEDEFAULT, CW_USEDEFAULT, 1280 + 16, 720 + 39, nullptr, nullptr,
                                     wc.hInstance, nullptr);
            ShowWindow(w, SW_SHOW);
            const int ok = rx_start(w);
            LOG_INFO(HW_GPU, "nx2 nvn: renderer {} on its window", ok ? "started" : "did not start");
            {
                std::scoped_lock lk{mu};
                done = true;
            }
            cv.notify_all();
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }).detach();
#else
        done = true;
#endif
    });
    std::unique_lock lk{mu};
    cv.wait(lk, [] { return done; });
}

} // extern "C"
