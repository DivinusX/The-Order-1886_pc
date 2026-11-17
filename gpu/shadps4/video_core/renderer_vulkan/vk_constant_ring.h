// SPDX-License-Identifier: GPL-2.0-or-later
// small read-only guest buffers (constants) copied by the GPU command thread (stage A of
// the draw pipeline, vk_draw_pipe.h) into this ring, so the draw recording thread only binds
// them. Positions are monotonic byte counts; a region is reused once the submission that
// recorded its last draw has completed on the GPU. Stage A records the owning submission as it
// allocates and retires regions whose submission the GPU has passed (see Allocate).

#pragma once

#include <array>
#include <deque>
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
    /// offset, or nullopt only if the space is still used by the submission being recorded.
    ///
    /// Live data is [retired, position) in absolute ring positions. Earlier versions learned
    /// which bytes the GPU had finished from stamps stage B queued (1024 entries), but the queue
    /// was only drained when the ring was already full, so it overflowed during long stretches
    /// of small submissions and later stamps were dropped: the ring then looked full while the
    /// GPU was long done. v0.2.26 skipped the upload (stale world-prompt glyphs), v0.2.27
    /// allocated at `retired`, the start of live data (overwritten in-flight constants, GPU
    /// page fault, device lost / exit 23). Stage A now records the submission each region
    /// belongs to as it allocates, retires as the GPU finishes, and when space is really still
    /// in flight waits for that submission only, never forcing a submit mid-frame.
    std::optional<u64> Allocate(u64 size, u64 alignment) {
        if (regions.size() >= RetireBacklog) {
            Retire();
        }
        u64 at = (position + alignment - 1) & ~(alignment - 1);
        if (at % Capacity + size > Capacity) {
            at += Capacity - at % Capacity; // wrap to the ring start
        }
        const auto in_use = [&] { return at + size > Capacity && at + size - Capacity > retired; };
        if (in_use()) {
            Retire();
            while (in_use()) {
                if (!WaitForRegion(at + size - Capacity)) {
                    if (std::has_single_bit(++skips)) {
                        LOG_WARNING(Render_Vulkan,
                                    "Constant ring: space still in the recording submission, "
                                    "upload skipped ({} times)", skips);
                    }
                    return std::nullopt;
                }
            }
        }
        position = at + size;
        // Conservative owner: the next submission. A packet allocated now is recorded into the
        // current command buffer, or into the next one if a submit lands in between.
        const u64 owner = scheduler.CurrentTick() + 1;
        if (!regions.empty() && regions.back().tick == owner) {
            regions.back().end = position;
        } else {
            regions.push_back({position, owner});
        }
        return at % Capacity;
    }

    [[nodiscard]] u8* Data(u64 offset) noexcept {
        return buffer.mapped_data.data() + offset;
    }

    void Flush(u64 offset, u64 size) {
        buffer.Flush(offset, size);
    }

    [[nodiscard]] u64 Position() const noexcept {
        return position;
    }

    [[nodiscard]] u64 GpuWaits() const noexcept {
        return gpu_waits;
    }

private:
    static constexpr size_t RetireBacklog = 64;

    /// Retires every region whose submission the GPU has finished.
    void Retire() {
        auto* semaphore = scheduler.GetWorkSemaphore();
        semaphore->Refresh();
        while (!regions.empty() && semaphore->IsFree(regions.front().tick)) {
            retired = std::max(retired, regions.front().end);
            regions.pop_front();
        }
    }

    /// Waits for the submission that owns absolute position `need`; false if that submission
    /// is the one being recorded (waiting would deadlock).
    bool WaitForRegion(u64 need) {
        for (const auto& region : regions) {
            if (region.end < need) {
                continue;
            }
            if (region.tick >= scheduler.CurrentTick()) {
                return false;
            }
            scheduler.Wait(region.tick);
            if (std::has_single_bit(++gpu_waits)) {
                LOG_WARNING(Render_Vulkan, "Constant ring full: waited for the GPU ({} times)",
                            gpu_waits);
            }
            Retire();
            return true;
        }
        return false;
    }

    struct Region {
        u64 end;  ///< absolute ring position after the region
        u64 tick; ///< submission whose completion frees it
    };
    Scheduler& scheduler;
    VideoCore::Buffer buffer;
    u64 position = 0;  ///< absolute write position
    u64 retired = 0;   ///< the GPU is done with everything before this
    u64 gpu_waits = 0; ///< times a full ring waited for a submitted command buffer
    u64 skips = 0;     ///< uploads skipped because the space is in the recording submission
    std::deque<Region> regions; ///< stage A only: oldest first
};

} // namespace Vulkan
