/* (Lichii nx2_nvn) What Hookshot's host C needs from GCC/Clang, for MSVC's C compiler (force-included there) */
#pragma once
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#include <stdint.h>
#define _Thread_local __declspec(thread)
#define __ATOMIC_RELAXED 0
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_SEQ_CST 5
/* x86-64: aligned loads and stores are atomic; the barrier keeps the compiler from moving them */
#define __atomic_load_n(p, m) (_ReadWriteBarrier(), *(p))
/* (only on byte maps here: rx_backend.c's write-watch collection) */
#define __atomic_exchange_n(p, v, m) ((uint8_t)_InterlockedExchange8((char volatile *)(p), (char)(v)))
#define __atomic_store_n(p, v, m) do { _ReadWriteBarrier(); *(p) = (v); _ReadWriteBarrier(); } while (0)
#endif
