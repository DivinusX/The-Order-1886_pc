// SPDX-License-Identifier: GPL-2.0-or-later
// small read-only guest buffers (constants) copied by the GPU command thread (stage A of
// the draw pipeline, vk_draw_pipe.h) into this ring, so the draw recording thread only binds
// them. Positions are monotonic byte counts; a region is reused once the submission that
// recorded its last draw has completed on the GPU. Stage B stamps each packet it records with
// the submission tick (Stamp), stage A retires stamps whose tick the GPU has passed.

#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
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
    /// offset, or nullopt only when the space is still owned by work not yet submitted.
    ///
    /// Live data is [retired, position) in absolute ring positions. Stage B stamps each packet
    /// with the submission it was recorded into; stage A retires stamps the GPU has passed.
    /// Before v0.2.30 stage A drained the 1024-entry stamp queue only once the ring was already
    /// full, so long stretches of small submissions overflowed it, later stamps were dropped and
    /// the ring looked full while the GPU was long done: v0.2.26 skipped the upload (stale
    /// world-prompt glyphs), v0.2.27 allocated at `retired`, the *start* of live data (in-flight
    /// constants overwritten, GPU page fault, device lost / exit 23). Stamps are now retired as
    /// they accumulate, and a ring that is really full waits for the owning submission only.
    std::optional<u64> Allocate(u64 size, u64 alignment) {
        if (stamps_head.load(std::memory_order_acquire) -
                stamps_tail.load(std::memory_order_relaxed) >= stamps.size() / 2) {
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
                if (!WaitForStamp(at + size - Capacity)) {
                    if (std::has_single_bit(++skips)) {
                        LOG_WARNING(Render_Vulkan,
                                    "Constant ring: space owned by unsubmitted work, upload "
                                    "skipped ({} times)", skips);
                    }
                    return std::nullopt;
                }
            }
        }
        allocated += at + size - position;
        position = at + size;
        Stats();
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

    /// Stage B, after recording a packet whose allocations end at `end`. Stamps of one
    /// submission are merged; the previous submission's stamp is published when the tick moves.
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
            ++dropped; // stage A retires at half full, so this needs a GPU 512 submissions behind
            return;
        }
        stamps[at % stamps.size()] = {end, tick};
        stamps_head.store(at + 1, std::memory_order_release);
        last_published = end;
    }

    void Retire() {
        auto* semaphore = scheduler.GetWorkSemaphore();
        semaphore->Refresh();
        u64 at = stamps_tail.load(std::memory_order_relaxed);
        const u64 head = stamps_head.load(std::memory_order_acquire);
        while (at < head && semaphore->IsFree(stamps[at % stamps.size()].tick)) {
            retired = std::max(retired, stamps[at % stamps.size()].end);
            ++at;
        }
        stamps_tail.store(at, std::memory_order_release);
    }

    /// Waits for the oldest stamped submission that frees absolute position `need`; false if
    /// no published stamp covers it yet or its submission has not been submitted.
    bool WaitForStamp(u64 need) {
        const u64 head = stamps_head.load(std::memory_order_acquire);
        for (u64 at = stamps_tail.load(std::memory_order_relaxed); at < head; ++at) {
            const auto& stamp = stamps[at % stamps.size()];
            if (stamp.end < need) {
                continue;
            }
            if (stamp.tick >= scheduler.CurrentTick()) {
                return false;
            }
            scheduler.Wait(stamp.tick);
            if (std::has_single_bit(++gpu_waits)) {
                LOG_WARNING(Render_Vulkan, "Constant ring full: waited for the GPU ({} times)",
                            gpu_waits);
            }
            Retire();
            return true;
        }
        return false;
    }

    /// ORDER1886_PERF_DIAG=ring: every 5 s, bytes per second, submissions, waits, skips, drops.
    void Stats() {
        static const bool on = [] {
            const char* v = std::getenv("ORDER1886_PERF_DIAG");
            return v && std::strstr(v, "ring");
        }();
        if (!on) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (stats_at == std::chrono::steady_clock::time_point{}) {
            stats_at = now;
            stats_tick = scheduler.CurrentTick();
            return;
        }
        const double seconds = std::chrono::duration<double>(now - stats_at).count();
        if (seconds < 5) {
            return;
        }
        const u64 tick = scheduler.CurrentTick();
        LOG_WARNING(Render_Vulkan,
                    "RINGSTATS {:.1f} MB/s, {:.0f} submissions/s, live {:.1f} MB, waits={} "
                    "skips={} dropped_stamps={}",
                    allocated / seconds / 1048576.0, (tick - stats_tick) / seconds,
                    (position - retired) / 1048576.0, gpu_waits, skips, dropped);
        allocated = 0;
        stats_at = now;
        stats_tick = tick;
    }

    struct StampEntry {
        u64 end;
        u64 tick;
    };
    Scheduler& scheduler;
    VideoCore::Buffer buffer;
    u64 position = 0;     ///< stage A
    u64 retired = 0;      ///< stage A: the GPU is done with everything before this
    u64 gpu_waits = 0, skips = 0, allocated = 0; ///< stage A
    std::chrono::steady_clock::time_point stats_at{};
    u64 stats_tick = 0;
    u64 pending_end = 0, pending_tick = 0, last_published = 0, dropped = 0; ///< stage B
    std::array<StampEntry, 1024> stamps{};
    alignas(64) std::atomic<u64> stamps_head{0}; ///< stage B writes
    alignas(64) std::atomic<u64> stamps_tail{0}; ///< stage A reads
};

} // namespace Vulkan
