// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/container/unordered_map.h"
#include "video_core/memory_manager.h"

namespace VideoCommon {

struct VirtualSegment {
    GPUVAddr gpu_addr;
    DAddr device_addr;
    u32 size;
};

using VirtualSegments = boost::container::small_vector<VirtualSegment, 8>;

class VirtualRangeCache {
public:
    static constexpr size_t MAX_ENTRIES = 8192;
    static constexpr size_t MAX_DEFERRED = 4096;

    const VirtualSegments* Query(Tegra::MemoryManager& memory, GPUVAddr gpu_addr, u32 size) {
        if (has_deferred.load(std::memory_order_acquire)) {
            ApplyDeferred();
        }
        if (entries.size() > MAX_ENTRIES) {
            entries.clear();
            ++stats.clears;
        }
        ++stats.queries;
        if (stats.queries % 8192 == 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now - stats.last_log > std::chrono::seconds(5)) {
                // How the range cache fares (each miss walks the GPU page table for the size).
                LOG_INFO(HW_GPU,
                         "nx2 range cache: {} queries in 5 s, {} misses ({} same address, other "
                         "size), {} clears, {} entries, {} pages walked, {} prefix hits",
                         stats.queries, stats.misses, stats.size_misses, stats.clears,
                         entries.size(), stats.pages_walked, stats.prefix_hits);
                stats = {};
                stats.last_log = now;
            }
        }
        const size_t as_id = memory.GetID();
        const u64 key = MakeKey(as_id, gpu_addr);
        const auto it = entries.find(key);
        if (it != entries.end() && it->second.as_id == as_id &&
            it->second.gpu_addr == gpu_addr && it->second.size == size) {
            return &it->second.segments;
        }
        // The same buffer bound with a smaller size (Ocarina binds one address with ~3000
        // different sizes a second, each walking ~8 MiB of page table): a prefix of the larger
        // contiguous range is contiguous too. EDEN_NX2_RANGE_PREFIX=0 walks every size.
        static const bool prefix_hits = [] {
            const char* value = std::getenv("EDEN_NX2_RANGE_PREFIX");
            return value == nullptr || value[0] != '0';
        }();
        if (prefix_hits && it != entries.end() && it->second.as_id == as_id &&
            it->second.gpu_addr == gpu_addr && it->second.size > size &&
            !it->second.segments.empty()) {
            trimmed.clear();
            u64 left = size;
            for (const VirtualSegment& segment : it->second.segments) {
                if (left == 0) {
                    break;
                }
                VirtualSegment part = segment;
                part.size = static_cast<u32>((std::min<u64>)(left, segment.size));
                trimmed.push_back(part);
                left -= part.size;
            }
            if (left == 0) {
                ++stats.prefix_hits;
                return &trimmed;
            }
        }
        ++stats.misses;
        stats.size_misses += it != entries.end() && it->second.gpu_addr == gpu_addr;
        stats.pages_walked += (u64{size} + 0xfff) >> 12;
        Entry entry{};
        entry.as_id = as_id;
        entry.gpu_addr = gpu_addr;
        entry.size = size;
        const auto ranges = memory.GetSubmappedRange(gpu_addr, size);
        GPUVAddr expected = gpu_addr;
        bool contiguous = true;
        for (const auto& [range_addr, range_size] : ranges) {
            if (range_addr != expected || range_size == 0) {
                contiguous = false;
                break;
            }
            const std::optional<DAddr> device_addr = memory.GpuToCpuAddress(range_addr);
            if (!device_addr || *device_addr == 0) {
                contiguous = false;
                break;
            }
            if (range_size > static_cast<size_t>((std::numeric_limits<u32>::max)())) {
                contiguous = false;
                break;
            }
            entry.segments.push_back(VirtualSegment{
                .gpu_addr = range_addr,
                .device_addr = *device_addr,
                .size = static_cast<u32>(range_size),
            });
            expected += range_size;
        }
        if (!contiguous || expected != gpu_addr + size) {
            entry.segments.clear();
        }
        if (prefix_hits && it != entries.end() && it->second.as_id == as_id &&
            it->second.gpu_addr == gpu_addr && it->second.size > size &&
            it->second.segments.empty() && !entry.segments.empty()) {
            // The larger size isn't contiguous but this one is: keep the larger miss cached
            // and hand this result out once.
            trimmed = std::move(entry.segments);
            return &trimmed;
        }
        const auto result = entries.insert_or_assign(key, std::move(entry));
        return &result.first->second.segments;
    }

    void Unmap(size_t as_id, GPUVAddr gpu_addr, u64 size) {
        if (size == 0) {
            return;
        }
        {
            std::scoped_lock lock{deferred_mutex};
            if (!deferred.empty()) {
                DeferredUnmap& last = deferred.back();
                if (last.as_id == as_id && last.gpu_addr + last.size == gpu_addr) {
                    last.size += size;
                    has_deferred.store(true, std::memory_order_release);
                    return;
                }
            }
            if (deferred.size() >= MAX_DEFERRED) {
                deferred.clear();
                deferred_overflow = true;
            } else {
                deferred.push_back(DeferredUnmap{
                    .as_id = as_id,
                    .gpu_addr = gpu_addr,
                    .size = size,
                });
            }
        }
        has_deferred.store(true, std::memory_order_release);
    }

private:
    struct Stats {
        u64 queries{};
        u64 misses{};
        u64 size_misses{};
        u64 clears{};
        u64 pages_walked{};
        u64 prefix_hits{};
        std::chrono::steady_clock::time_point last_log{std::chrono::steady_clock::now()};
    } stats;

    struct Entry {
        VirtualSegments segments;
        size_t as_id{};
        GPUVAddr gpu_addr{};
        u32 size{};
    };
    VirtualSegments trimmed; ///< A prefix of a cached entry (valid until the next Query)

    struct DeferredUnmap {
        size_t as_id;
        GPUVAddr gpu_addr;
        u64 size;
    };

    static u64 MakeKey(size_t as_id, GPUVAddr gpu_addr) {
        return (static_cast<u64>(as_id) << 48) ^ gpu_addr;
    }

    void ApplyDeferred() {
        std::vector<DeferredUnmap> pending;
        bool overflow = false;
        {
            std::scoped_lock lock{deferred_mutex};
            has_deferred.store(false, std::memory_order_release);
            pending.swap(deferred);
            overflow = deferred_overflow;
            deferred_overflow = false;
        }
        if (overflow) {
            entries.clear();
            return;
        }
        if (pending.empty() || entries.empty()) {
            return;
        }
        for (auto it = entries.begin(); it != entries.end();) {
            const Entry& entry = it->second;
            const GPUVAddr entry_end = entry.gpu_addr + entry.size;
            bool overlaps = false;
            for (const DeferredUnmap& unmap : pending) {
                if (unmap.as_id != entry.as_id) {
                    continue;
                }
                if (entry.gpu_addr < unmap.gpu_addr + unmap.size && unmap.gpu_addr < entry_end) {
                    overlaps = true;
                    break;
                }
            }
            if (overlaps) {
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    ::Common::unordered_map<u64, Entry> entries;
    std::vector<DeferredUnmap> deferred;
    std::mutex deferred_mutex;
    std::atomic<bool> has_deferred{false};
    bool deferred_overflow{};
};

} // namespace VideoCommon
