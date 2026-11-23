// SPDX-License-Identifier: GPL-2.0-or-later
// #47 frame-generation groundwork: vertex/fragment constant-buffer capture.
//
// ORDER1886_CBCAPTURE=<file> appends deduplicated constant-buffer images with their stage,
// shader buffer index and guest address at the moment the ring copies them. This is the
// only place the guest VA, the byte size and the live contents are all available, and it
// runs on the single draw-recording thread. Records:
//   struct { u64 address; u32 stage; u32 index; u32 size; }  followed by `size` bytes
// Dedup key is (address, size, FNV-1a over the content), so a per-draw re-upload of an
// unchanged matrix lands once; the counter keeps scene changes visible.
// The offline scanner looks for perspective-projection signatures in vertex records:
// row-major m[3][2]==-1,m[3][3]==0 (GNM mul(vector,matrix)) or the column-major mirror,
// then decomposes fovY = 2*atan(1/|m11|), aspect = m11/m00, near/far from m34 (m[3][2]/[3][3]).
// Captures are developer data written outside any game tree; nothing is read back here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace OrderCapture {

inline FILE* Sink() {
    static FILE* out = [] {
        const char* path = std::getenv("ORDER1886_CBCAPTURE");
        return path && *path ? std::fopen(path, "wb") : nullptr;
    }();
    return out;
}

inline std::uint64_t Fnv1a(const void* data, std::size_t size) {
    auto p = static_cast<const std::uint8_t*>(data);
    std::uint64_t hash = 1469598103934665603ULL;
    // Sampling stride keeps the dedup cheap for multi-KB buffers.
    const std::size_t step = size > 4096 ? 16 : 1;
    for (std::size_t i = 0; i < size; i += step) {
        hash = (hash ^ p[i]) * 1099511628211ULL;
    }
    return hash;
}

struct Key {
    std::uint64_t hash;
    std::uint64_t address;
};

// Tiny open-addressed set: constant re-binds dominate a frame, and once the scene's
// matrices are known the capture becomes a few compares per draw.
inline bool Seen(Key key) {
    static Key table[1 << 15]{};
    std::size_t slot = static_cast<std::size_t>((key.hash ^ key.address) & (std::size(table) - 1));
    for (std::size_t probe = 0; probe < std::size(table); ++probe) {
        Key& cell = table[slot];
        if (cell.hash == 0 && cell.address == 0) {
            cell = key;
            return false;
        }
        if (cell.hash == key.hash && cell.address == key.address) {
            return true;
        }
        slot = (slot + 1) & (std::size(table) - 1);
    }
    return true; // table saturated: stop writing rather than grow unbounded
}

inline void ConstantBuffer(int stage, unsigned index, std::uint64_t address, const void* data,
                           std::size_t size) {
    FILE* out = Sink();
    if (!out || size == 0 || size > (1u << 20)) {
        return;
    }
    const Key key{Fnv1a(data, size), address};
    if (Seen(key)) {
        return;
    }
    const std::uint32_t head[3] = {static_cast<std::uint32_t>(stage), index,
                                   static_cast<std::uint32_t>(size)};
    std::fwrite(&address, sizeof(address), 1, out);
    std::fwrite(head, sizeof(head), 1, out);
    std::fwrite(data, size, 1, out);
}

} // namespace OrderCapture
