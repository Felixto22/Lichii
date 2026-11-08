/* (Lichii nx2_nvn) a64.h for Hookshot's host NVN layer built inside Lichii: guest memory is Lichii's fastmem arena */
#ifndef A64_H
#define A64_H
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct GuestContext {
    uint64_t x[32];
    uint64_t pc;
    uint8_t n, z, c, v;
    int halted;
    uint64_t vreg[32][2];
    uint64_t tpidr_el0;
    uint64_t tpidrro_el0;
    uint64_t fpcr, fpsr;
    uint64_t excl_addr, excl_lo, excl_hi;
    int excl_valid;
} GuestContext;
typedef GuestContext Ctx;
extern uint8_t *nx2_guest_base;
#define A64_MEM nx2_guest_base
#define A64_PTR(a) (nx2_guest_base + (uint64_t)(a))
#define A64_INLINE static inline
#define A64_NOESC
/* the write watch: Lichii's byte per guest page (core/nx2_nvn DirtyPages) */
extern uint8_t *a64_dirty;
#define A64_DIRTY_PAGES (1ull << 27)
#ifdef __cplusplus
}
#endif
#endif
