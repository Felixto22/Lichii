/* (Lichii nx2_nvn) The GPU memory map of Hookshot's native NVN layer, from host/nvdrv.c's native part: a GPU address is
   the guest address (virtual pools through nvn_native.c's page tables), the pools are tracked for the renderer's change
   watch and its imports. No nvhost devices: the game's NVN driver never runs. */
#include "hos.h"
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint64_t nvn_gpu_guest(uint64_t gpu);
uint64_t nvn_gpu_contiguous(uint64_t gpu, uint64_t n);
uint64_t hos_guest_size(void);

static SRWLOCK lock = SRWLOCK_INIT;
typedef struct { uint64_t va, size, guest; } Map;
static Map *maps;
static int nmaps, capmaps;
static volatile LONG maps_changed = 1;
volatile LONG maps_generation = 1;

static int map_floor(uint64_t va) {
    int lo = 0, hi = nmaps - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (maps[mid].va <= va) lo = mid + 1;
        else hi = mid - 1;
    }
    return hi;
}
static void map_add(uint64_t va, uint64_t size, uint64_t guest) {
    maps_changed = 1;
    InterlockedIncrement(&maps_generation);
    int i = map_floor(va);
    if (i >= 0 && maps[i].va == va) { maps[i].size = size; maps[i].guest = guest; return; }
    int at = i + 1, end = at;
    while (end < nmaps && maps[end].va < va + size) end++;
    if (end > at) { memmove(&maps[at], &maps[end], (nmaps - end) * sizeof *maps); nmaps -= end - at; }
    if (nmaps == capmaps) {
        capmaps = capmaps ? capmaps * 2 : 256;
        maps = (Map *)realloc(maps, capmaps * sizeof *maps);
    }
    memmove(&maps[at + 1], &maps[at], (nmaps - at) * sizeof *maps);
    maps[at].va = va, maps[at].size = size, maps[at].guest = guest;
    nmaps++;
}
static void map_remove(uint64_t va) {
    maps_changed = 1;
    InterlockedIncrement(&maps_generation);
    int i = map_floor(va);
    if (i >= 0 && maps[i].va == va) { memmove(&maps[i], &maps[i + 1], (nmaps - i - 1) * sizeof *maps); nmaps--; }
}

int hos_gpu_pools(uint64_t (*out)[2], int max) {
    AcquireSRWLockExclusive(&lock);
    int n = 0;
    for (int i = 0; i < nmaps && n < max; i++)
        if (maps[i].va == maps[i].guest) out[n][0] = maps[i].guest, out[n][1] = maps[i].size, n++;
    ReleaseSRWLockExclusive(&lock);
    return n;
}
uint32_t hos_gpu_maps_generation(void) { return (uint32_t)maps_generation; }

typedef struct { uint64_t lo, hi; int n; } Tracked;
static Tracked *tracked;
static int ntracked, captracked;
void hos_gpu_track(uint64_t guest, uint64_t size, int on) {
    const uint64_t lo = guest & ~0xFFFULL, hi = (guest + size + 0xFFF) & ~0xFFFULL;
    AcquireSRWLockExclusive(&lock);
    if (on) map_add(guest, size, guest);
    else map_remove(guest);
    int i = 0;
    while (i < ntracked && (tracked[i].lo != lo || tracked[i].hi != hi)) i++;
    if (on) {
        if (i < ntracked) tracked[i].n++;
        else {
            if (ntracked == captracked) {
                captracked = captracked ? captracked * 2 : 256;
                tracked = (Tracked *)realloc(tracked, (size_t)captracked * sizeof *tracked);
            }
            tracked[ntracked].lo = lo, tracked[ntracked].hi = hi, tracked[ntracked].n = 1;
            ntracked++;
        }
    } else if (i < ntracked && --tracked[i].n <= 0) {
        tracked[i] = tracked[--ntracked];
    }
    maps_changed = 1;
    InterlockedIncrement(&maps_generation);
    ReleaseSRWLockExclusive(&lock);
}

static int cmp_range(const void *a, const void *b) {
    const uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}
int hos_gpu_guest_ranges(uint64_t (*out)[2], int max) {
    static uint64_t (*merged)[2];
    static int nmerged, capmerged;
    AcquireSRWLockExclusive(&lock);
    if (maps_changed) {
        maps_changed = 0;
        const int nall = nmaps + ntracked;
        if (capmerged < nall) {
            capmerged = nall + 64;
            merged = (uint64_t (*)[2])realloc(merged, (size_t)capmerged * sizeof *merged);
        }
        for (int i = 0; i < nmaps; i++) {
            merged[i][0] = maps[i].guest & ~0xFFFULL;
            merged[i][1] = (maps[i].guest + maps[i].size + 0xFFF) & ~0xFFFULL;
        }
        for (int i = 0; i < ntracked; i++) merged[nmaps + i][0] = tracked[i].lo, merged[nmaps + i][1] = tracked[i].hi;
        qsort(merged, (size_t)nall, sizeof *merged, cmp_range);
        nmerged = 0;
        for (int i = 0; i < nall; i++) {
            if (nmerged && merged[i][0] <= merged[nmerged - 1][1] + (16ULL << 20)) {
                if (merged[i][1] > merged[nmerged - 1][1]) merged[nmerged - 1][1] = merged[i][1];
            } else {
                merged[nmerged][0] = merged[i][0];
                merged[nmerged][1] = merged[i][1];
                nmerged++;
            }
        }
    }
    const int n = nmerged < max ? nmerged : max;
    memcpy(out, merged, (size_t)n * sizeof *merged);
    ReleaseSRWLockExclusive(&lock);
    return n;
}

/* GPU address -> host pointer and the bytes that stay contiguous from it (the native layer's identity map) */
uint8_t *hos_gpu_ptr(uint64_t va, uint64_t *avail) {
    const uint64_t g = nvn_gpu_guest(va);
    if (!g || g >= hos_guest_size()) return NULL;
    *avail = va < hos_guest_size() ? hos_guest_size() - g : nvn_gpu_contiguous(va, 1ull << 30);
    return (uint8_t *)GP(g);
}
