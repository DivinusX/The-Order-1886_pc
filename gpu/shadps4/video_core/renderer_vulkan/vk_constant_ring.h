// SPDX-License-Identifier: GPL-2.0-or-later
// small read-only guest buffers (constants) copied by the GPU command thread (stage A of
// the draw pipeline, vk_draw_pipe.h) into this ring, so the draw recording thread only binds
// them. Positions are monotonic byte counts; a region is reused once the submission that
// recorded its last draw has completed on the GPU. Stage B stamps each packet it records with
// the submission tick (Stamp), stage A retires stamps whose tick the GPU has passed.

#pragma once

#include <array>
#include <bit>
#include <atomic>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

class ConstantRing {
public:
    static constexpr u64 Capacity = 32ull << 20;

    ConstantRing(const Instance& instance, Scheduler& scheduler_)
        : scheduler{scheduler_},
          buffer{instance, 0, Capacity, VideoCore::MemoryType::Stream, "PS4 constant ring"} {
        ASSERT(!buffer.mapped_data.empty());
    }

    [[nodiscard]] vk::Buffer Handle() const noexcept {
        return buffer.Handle();
    }

    /// Stage A, before the draw packet is begun: `size` bytes at `alignment`; returns the ring
    /// offset. Live data is [retired, position) in absolute ring positions. When the next lap
    /// would overtake it, wait for the GPU instead of either skipping the upload (v0.2.26: the
    /// descriptor kept the previous draw's table, stale world-prompt glyphs) or allocating at
    /// `retired` (v0.2.27: that is the *start* of the live data, so in-flight draws read
    /// overwritten constants, followed garbage pointers and faulted the GPU: gfxhub page
    /// fault, ring timeout, device lost / exit 23 within minutes).
    std::optional<u64> Allocate(u64 size, u64 alignment) {
        u64 at = (position + alignment - 1) & ~(alignment - 1);
        if (at % Capacity + size > Capacity) {
            at += Capacity - at % Capacity; // wrap to the ring start
        }
        const auto in_use = [&] { return at + size > Capacity && at + size - Capacity > retired; };
        if (in_use()) {
            Retire();
            if (in_use()) {
                WaitForGpu();
                if (in_use()) {
                    return std::nullopt; // larger than the ring: cannot happen for constants
                }
            }
        }
        position = at + size;
        return at % Capacity;
    }

    [[nodiscard]] u64 GpuWaits() const noexcept {
        return gpu_waits;
    }

    [[nodiscard]] u8* Data(u64 offset) noexcept {
        return buffer.mapped_data.data() + offset;
    }

    /// Stage A, after writing: makes the bytes visible to the GPU.
    void Flush(u64 offset, u64 size) {
        buffer.Flush(offset, size);
    }

    /// Stage A: the position the next packet's allocations end at.
    [[nodiscard]] u64 Position() const noexcept {
        return position;
    }

    /// Stage B, after recording a packet whose allocations end at `end`. Stamps of one
    /// submission tick are merged: only the last end per tick is published.
    void Stamp(u64 end) {
        if (end == pending_end) {
            return;
        }
        const u64 tick = scheduler.CurrentTick();
        if (tick != pending_tick && pending_end != last_published) {
            Publish(pending_end, pending_tick);
        }
        pending_end = end;
        pending_tick = tick;
    }

private:
    void Publish(u64 end, u64 tick) {
        const u64 at = stamps_head.load(std::memory_order_relaxed);
        if (at - stamps_tail.load(std::memory_order_acquire) >= stamps.size()) {
            return; // full: this region retires with a later stamp
        }
        stamps[at % stamps.size()] = {end, tick};
        stamps_head.store(at + 1, std::memory_order_release);
        last_published = end;
    }

    /// Submits what is recorded and waits until the GPU is idle (Scheduler::Finish drains the
    /// draw recording thread first), so every ring byte written so far is consumed. The stamps
    /// stage B queued describe regions that are now all retired.
    void WaitForGpu() {
        scheduler.Finish();
        retired = position;
        stamps_tail.store(stamps_head.load(std::memory_order_acquire), std::memory_order_release);
        ++gpu_waits;
        if (std::has_single_bit(gpu_waits)) {
            LOG_WARNING(Render_Vulkan, "Constant ring full: waited for the GPU ({} times)",
                        gpu_waits);
        }
    }

    void Retire() {
        auto* semaphore = scheduler.GetWorkSemaphore();
        semaphore->Refresh();
        u64 at = stamps_tail.load(std::memory_order_relaxed);
        const u64 head = stamps_head.load(std::memory_order_acquire);
        while (at < head && semaphore->IsFree(stamps[at % stamps.size()].tick)) {
            retired = stamps[at % stamps.size()].end;
            ++at;
        }
        stamps_tail.store(at, std::memory_order_release);
    }

    struct StampEntry {
        u64 end;
        u64 tick;
    };
    Scheduler& scheduler;
    VideoCore::Buffer buffer;
    u64 position = 0;     ///< stage A
    u64 retired = 0;      ///< stage A: the GPU is done with everything before this
    u64 gpu_waits = 0;    ///< stage A: times a full ring waited for the GPU
    u64 pending_end = 0, pending_tick = 0, last_published = 0; ///< stage B
    std::array<StampEntry, 1024> stamps{};
    alignas(64) std::atomic<u64> stamps_head{0}; ///< stage B writes
    alignas(64) std::atomic<u64> stamps_tail{0}; ///< stage A reads
};

} // namespace Vulkan
