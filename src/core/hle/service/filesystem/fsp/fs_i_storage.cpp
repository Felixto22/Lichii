// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

#include "core/file_sys/errors.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"

namespace Service::FileSystem {

IStorage::IStorage(Core::System& system_, FileSys::VirtualFile backend_)
    : ServiceFramework{system_, "IStorage"}, backend(std::move(backend_)) {
    static const FunctionInfo functions[] = {
        {0, D<&IStorage::Read>, "Read"},
        {1, nullptr, "Write"},
        {2, nullptr, "Flush"},
        {3, nullptr, "SetSize"},
        {4, D<&IStorage::GetSize>, "GetSize"},
        {5, nullptr, "OperateRange"},
    };
    RegisterHandlers(functions);
}

Result IStorage::Read(
    OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> out_bytes,
    s64 offset, s64 length) {
    LOG_DEBUG(Service_FS, "called, offset={:#x}, length={}", offset, length);

    R_UNLESS(length >= 0, FileSys::ResultInvalidSize);
    R_UNLESS(offset >= 0, FileSys::ResultInvalidOffset);

    const auto now = std::chrono::steady_clock::now();
    if (now < next_read_tick) {
        std::this_thread::sleep_for(next_read_tick - now);
    }
    next_read_tick = std::chrono::steady_clock::now() +
                     std::chrono::nanoseconds{std::min(length, MaxChargedReadSize) *
                                              1'000'000'000 / ReadBytesPerSecond};

    // Read the data from the Storage backend
    backend->Read(out_bytes.data(), length, offset);

    // Every 5 s: how much the game read, and how much of it it had read before (a game that
    // keeps re-reading and decompressing the same data spends its CPU there).
    {
        static std::mutex stats_mutex;
        static std::map<std::tuple<const void*, s64, s64>, u32> seen;
        static u64 reads, bytes, repeated_reads, repeated_bytes;
        static auto last_log = std::chrono::steady_clock::now();
        std::scoped_lock lock{stats_mutex};
        ++reads;
        bytes += static_cast<u64>(length);
        if (seen.size() < 200'000) {
            if (u32& count = seen[{backend.get(), offset, length}]; count++ != 0) {
                ++repeated_reads;
                repeated_bytes += static_cast<u64>(length);
            }
        }
        const auto log_now = std::chrono::steady_clock::now();
        if (log_now - last_log > std::chrono::seconds(5)) {
            LOG_INFO(Service_FS,
                     "nx2 storage reads: {} in 5 s ({} KiB), {} of them read before ({} KiB)",
                     reads, bytes >> 10, repeated_reads, repeated_bytes >> 10);
            reads = bytes = repeated_reads = repeated_bytes = 0;
            last_log = log_now;
        }
    }

    R_SUCCEED();
}

Result IStorage::GetSize(Out<u64> out_size) {
    *out_size = backend->GetSize();

    LOG_DEBUG(Service_FS, "called, size={}", *out_size);

    R_SUCCEED();
}

} // namespace Service::FileSystem
