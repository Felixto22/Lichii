// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

#include "core/file_sys/errors.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"

namespace Service::FileSystem {

namespace {

// The files of a RomFS image by data offset (to name what the game reads).
struct Nx2RomFsFile {
    u64 begin;
    u64 end;
    std::string name;
};

std::vector<Nx2RomFsFile> Nx2ParseRomFs(const FileSys::VirtualFile& storage) {
    struct Header {
        u64 header_size, dir_hash_ofs, dir_hash_size, dir_ofs, dir_size, file_hash_ofs,
            file_hash_size, file_ofs, file_size, data_ofs;
    } header{};
    std::vector<Nx2RomFsFile> files;
    if (storage->ReadObject(&header) != sizeof(header) || header.header_size != sizeof(header) ||
        header.file_size > (64ULL << 20) || header.dir_size > (64ULL << 20)) {
        return files;
    }
    std::vector<u8> dirs(header.dir_size);
    std::vector<u8> table(header.file_size);
    storage->Read(dirs.data(), dirs.size(), header.dir_ofs);
    storage->Read(table.data(), table.size(), header.file_ofs);
    const auto dir_path = [&](u32 dir) {
        std::string path;
        for (int depth = 0; depth < 32 && dir != 0 && dir + 0x18 <= dirs.size(); ++depth) {
            u32 entry[6];
            std::memcpy(entry, dirs.data() + dir, sizeof(entry));
            const u32 name_size = (std::min<u32>)(entry[5], static_cast<u32>(dirs.size() - dir - 0x18));
            path = std::string(reinterpret_cast<const char*>(dirs.data() + dir + 0x18), name_size) +
                   "/" + path;
            dir = entry[0];
        }
        return path;
    };
    for (size_t pos = 0; pos + 0x20 <= table.size();) {
        u32 parent, name_size;
        u64 offset, size;
        std::memcpy(&parent, table.data() + pos, 4);
        std::memcpy(&offset, table.data() + pos + 8, 8);
        std::memcpy(&size, table.data() + pos + 16, 8);
        std::memcpy(&name_size, table.data() + pos + 28, 4);
        name_size = (std::min<u32>)(name_size, static_cast<u32>(table.size() - pos - 0x20));
        files.push_back({header.data_ofs + offset, header.data_ofs + offset + size,
                         dir_path(parent) +
                             std::string(reinterpret_cast<const char*>(table.data() + pos + 0x20),
                                         name_size)});
        pos += 0x20 + ((name_size + 3) & ~3U);
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.begin < b.begin; });
    return files;
}

// Every 5 s: how much the game read, how much of it it had read before (a game that keeps
// re-reading and decompressing the same data spends its CPU there), and which files.
void Nx2NoteRead(const FileSys::VirtualFile& backend, s64 offset, s64 length) {
    static std::mutex stats_mutex;
    static std::map<std::tuple<const void*, s64, s64>, u32> seen;
    static std::map<const void*, std::vector<Nx2RomFsFile>> romfs;
    static std::map<std::string, std::pair<u32, u64>> window_files;
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
    auto [it, is_new] = romfs.try_emplace(backend.get());
    if (is_new && backend->GetSize() > (16ULL << 20)) {
        it->second = Nx2ParseRomFs(backend);
        LOG_INFO(Service_FS, "nx2 storage {} ({} MiB): {} RomFS files", backend->GetName(),
                 backend->GetSize() >> 20, it->second.size());
    }
    std::string name = fmt::format("{}@{:#x}", backend->GetName(), offset);
    const auto& files = it->second;
    auto file = std::upper_bound(files.begin(), files.end(), static_cast<u64>(offset),
                                 [](u64 value, const auto& f) { return value < f.begin; });
    if (file != files.begin() && static_cast<u64>(offset) < std::prev(file)->end) {
        name = std::prev(file)->name;
    }
    auto& [count, file_bytes] = window_files[name];
    ++count;
    file_bytes += static_cast<u64>(length);
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log > std::chrono::seconds(5)) {
        std::vector<std::pair<u64, std::string>> top;
        for (const auto& [file_name, stats] : window_files) {
            top.emplace_back(stats.second, fmt::format("{} ({}x, {} KiB)", file_name, stats.first,
                                                       stats.second >> 10));
        }
        std::sort(top.rbegin(), top.rend());
        std::string list;
        for (size_t i = 0; i < (std::min<size_t>)(top.size(), 6); ++i) {
            list += (i != 0 ? "; " : "") + top[i].second;
        }
        LOG_INFO(Service_FS,
                 "nx2 storage reads: {} in 5 s ({} KiB), {} of them read before ({} KiB), {} "
                 "files: {}",
                 reads, bytes >> 10, repeated_reads, repeated_bytes >> 10, window_files.size(),
                 list);
        reads = bytes = repeated_reads = repeated_bytes = 0;
        window_files.clear();
        last_log = now;
    }
}

} // namespace

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

    Nx2NoteRead(backend, offset, length);

    R_SUCCEED();
}

Result IStorage::GetSize(Out<u64> out_size) {
    *out_size = backend->GetSize();

    LOG_DEBUG(Service_FS, "called, size={}", *out_size);

    R_SUCCEED();
}

} // namespace Service::FileSystem
