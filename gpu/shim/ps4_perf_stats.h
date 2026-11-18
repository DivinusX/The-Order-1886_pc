// SPDX-License-Identifier: GPL-2.0-or-later
// numbers for the performance overlay (ps4_overlay.cpp). A thread started once samples
// CPU, RAM, GPU use (Windows counters; /proc and amdgpu sysfs on Linux), VRAM
// (VK_EXT_memory_budget) and GPU/CPU temperatures (hwmon on Linux, NVML for NVIDIA cards)
// twice a second; a value that cannot be measured stays negative and shows as "n/a".

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace Vulkan {
class Instance;
}

namespace Ps4PerfStats {

/// Overlay items (Ps4Settings::Values::overlay_items).
enum Item : unsigned {
    Fps = 1u << 0,
    FrameTime = 1u << 1,
    Upscaler = 1u << 2,
    Gpu = 1u << 3,
    Cpu = 1u << 4,
    Ram = 1u << 5,
    Vram = 1u << 6,
    AllItems = (1u << 7) - 1,
};
inline constexpr unsigned DefaultItems = Fps | FrameTime | Cpu | Ram;

struct PresentationMetrics {
    float fps = -1.0f, interval_ms = -1.0f;
    bool loading = false;
    bool stale = false;
};

// Successful host presentation intervals, including stalls. UI visibility does not
// affect collection, and loading-screen presents do not contaminate game presents.
class PresentationCounter {
public:
    void Present(uint64_t now_ns, bool loading) {
        if (!seen || loading != metrics.loading || now_ns <= last_ns) {
            seen = true;
            last_ns = now_ns;
            elapsed_ns = intervals = 0;
            metrics = {.loading = loading};
            return;
        }
        elapsed_ns += now_ns - last_ns;
        last_ns = now_ns;
        ++intervals;
        if (elapsed_ns >= 500'000'000) {
            metrics.fps = float(double(intervals) * 1'000'000'000.0 / double(elapsed_ns));
            metrics.interval_ms = float(double(elapsed_ns) / (double(intervals) * 1'000'000.0));
            elapsed_ns = intervals = 0;
        }
    }

    PresentationMetrics Latest() const { return metrics; }
    // Query age without inserting synthetic frames. Before the first event stays n/a.
    PresentationMetrics Latest(uint64_t now_ns) const {
        if (seen && now_ns >= last_ns && now_ns - last_ns >= 1'000'000'000) {
            return {.fps = 0.0f, .interval_ms = -1.0f, .loading = metrics.loading, .stale = true};
        }
        return metrics;
    }

private:
    bool seen = false;
    uint64_t last_ns{}, elapsed_ns{}, intervals{};
    PresentationMetrics metrics;
};

struct Sample {
    float fps = -1.0f, frame_ms = -1.0f;
    float game_fps = -1.0f, game_frame_ms = -1.0f;
    bool game_stale = false;
    std::string upscaler;
    std::string gpu_name;
    float gpu_percent = -1.0f, cpu_percent = -1.0f;
    double ram_used_gb = -1.0, ram_total_gb = -1.0, vram_used_gb = -1.0, vram_total_gb = -1.0;
    // Hardware panel (the overlay card): whole-system values, negative when this PC cannot
    // report them, in which case the card leaves the field out.
    std::string cpu_name;
    float gpu_temp_c = -1.0f, cpu_temp_c = -1.0f;
    float system_cpu_percent = -1.0f;
    double system_ram_used_gb = -1.0;
};

/// Aggregate CPU jiffies from the first ("cpu ") line of /proc/stat.
struct CpuTimes {
    uint64_t idle = 0, total = 0;
};

inline bool ParseCpuLine(const std::string& line, CpuTimes& out) {
    if (line.compare(0, 4, "cpu ") != 0) {
        return false;
    }
    unsigned long long v[10]{};
    const int n = std::sscanf(line.c_str() + 4, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                              &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    if (n < 4) {
        return false;
    }
    // user nice system idle iowait irq softirq steal [guest guest_nice]; guest time is
    // already counted in user/nice, so only the first eight fields add up to the total.
    out = {};
    for (int i = 0; i < std::min(n, 8); ++i) {
        out.total += v[i];
    }
    out.idle = v[3] + (n > 4 ? v[4] : 0);
    return true;
}

/// Busy share of every core between two /proc/stat samples, 0 to 100; -1 without progress.
inline float CpuBusyPercent(const CpuTimes& before, const CpuTimes& after) {
    if (after.total <= before.total || after.idle < before.idle) {
        return -1.0f;
    }
    const double total = double(after.total - before.total);
    const double idle = double(after.idle - before.idle);
    return std::clamp(float((total - idle) * 100.0 / total), 0.0f, 100.0f);
}

/// MemTotal and MemAvailable from /proc/meminfo, in GiB.
inline bool ParseMeminfo(const std::string& text, double& total_gb, double& available_gb) {
    unsigned long long total = 0, available = 0;
    bool have_total = false, have_available = false;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = text.find('\n', at);
        const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        have_total = have_total || std::sscanf(line.c_str(), "MemTotal: %llu kB", &total) == 1;
        have_available =
            have_available || std::sscanf(line.c_str(), "MemAvailable: %llu kB", &available) == 1;
        if (end == std::string::npos) break;
        at = end + 1;
    }
    if (!have_total || !have_available || !total) {
        return false;
    }
    total_gb = double(total) / (1024.0 * 1024.0);
    available_gb = double(available) / (1024.0 * 1024.0);
    return true;
}

/// "AMD Ryzen 9 9900X 12-Core Processor" -> "Ryzen 9 9900X"; "Intel(R) Core(TM) i7-12700K CPU @
/// 3.60GHz" -> "Core i7-12700K". Falls back to the trimmed input.
inline std::string ShortCpuName(std::string name) {
    const auto erase_all = [&](const std::string& what) {
        for (size_t at; (at = name.find(what)) != std::string::npos;) name.erase(at, what.size());
    };
    for (const char* noise : {"(R)", "(TM)", "(tm)", "AMD ", "Intel ", " Processor", " CPU"}) {
        erase_all(noise);
    }
    for (const char* cut : {" @", " with ", " w/"}) {
        if (const size_t at = name.find(cut); at != std::string::npos) name.erase(at);
    }
    // "12-Core", "8-Core" and the like
    if (const size_t at = name.find("-Core"); at != std::string::npos) {
        size_t start = at;
        while (start > 0 && name[start - 1] >= '0' && name[start - 1] <= '9') --start;
        name.erase(start, at + 5 - start);
    }
    size_t a = name.find_first_not_of(' '), b = name.find_last_not_of(' ');
    std::string out = a == std::string::npos ? std::string{} : name.substr(a, b - a + 1);
    for (size_t at; (at = out.find("  ")) != std::string::npos;) out.erase(at, 1);
    return out;
}

/// "AMD Radeon RX 9070 XT (RADV GFX1201)" -> "Radeon RX 9070 XT"; "NVIDIA GeForce RTX 4070" ->
/// "RTX 4070"; "Intel(R) Arc(TM) A770 Graphics" -> "Arc A770 Graphics".
inline std::string ShortGpuName(std::string name) {
    if (const size_t at = name.find(" ("); at != std::string::npos) name.erase(at); // driver tag
    const auto erase_all = [&](const std::string& what) {
        for (size_t at; (at = name.find(what)) != std::string::npos;) name.erase(at, what.size());
    };
    for (const char* noise : {"(R)", "(TM)", "AMD ", "NVIDIA ", "GeForce ", "Intel "}) {
        erase_all(noise);
    }
    size_t a = name.find_first_not_of(' '), b = name.find_last_not_of(' ');
    return a == std::string::npos ? std::string{} : name.substr(a, b - a + 1);
}

/// Colour band of a reading in the overlay card.
enum class Tier { Good, Warn, Bad };

inline Tier TempTier(float celsius) {
    return celsius < 70.0f ? Tier::Good : celsius < 85.0f ? Tier::Warn : Tier::Bad;
}

/// Frame rate against the frame-rate limit (the game's target).
inline Tier FpsTier(float fps, float target) {
    if (target <= 0.0f) target = 60.0f;
    return fps >= target * 0.95f ? Tier::Good : fps >= target * 0.75f ? Tier::Warn : Tier::Bad;
}

/// Which hwmon temperature label to read for a sensor driver, best first. An empty list
/// means "the first temperature the driver has".
inline const char* const* PreferredTempLabels(const std::string& driver) {
    static const char* const amd_cpu[] = {"Tdie", "Tctl", nullptr};
    static const char* const intel_cpu[] = {"Package id 0", nullptr};
    static const char* const gpu[] = {"edge", nullptr};
    static const char* const none[] = {nullptr};
    if (driver == "k10temp" || driver == "zenpower") return amd_cpu;
    if (driver == "coretemp") return intel_cpu;
    if (driver == "amdgpu") return gpu;
    return none;
}

/// Process CPU time over wall time on every core, 0 to 100; -1 without a wall interval.
inline float CpuPercent(uint64_t process_delta, uint64_t wall_delta, unsigned cores) {
    if (!wall_delta || !cores) {
        return -1.0f;
    }
    return std::clamp(float(double(process_delta) * 100.0 / (double(wall_delta) * cores)), 0.0f, 100.0f);
}

/// The overlay text for the items in `mask`: one line each, or one line when `row`.
inline std::string Format(const Sample& s, unsigned mask, bool row) {
    std::string out;
    char part[160];
    const auto add = [&] {
        if (!out.empty()) {
            out += row ? "   " : "\n";
        }
        out += part;
    };
    const auto pair = [](char* to, size_t size, const char* label, double used, double total) {
        if (used < 0.0) {
            std::snprintf(to, size, "%s n/a", label);
        } else if (total < 0.0) {
            std::snprintf(to, size, "%s %.1f GB", label, used);
        } else {
            std::snprintf(to, size, "%s %.1f / %.1f GB", label, used, total);
        }
    };
    if (mask & Fps) {
        if (s.game_fps < 0.0f) std::snprintf(part, sizeof(part), "Game output FPS n/a");
        else std::snprintf(part, sizeof(part), "Game output %.1f FPS%s", s.game_fps,
                           s.game_stale ? " (stale)" : "");
        add();
        if (s.fps < 0.0f) std::snprintf(part, sizeof(part), "Presentation FPS n/a");
        else std::snprintf(part, sizeof(part), "Presentation %.1f FPS", s.fps);
        add();
    }
    if (mask & FrameTime) {
        if (s.game_stale) std::snprintf(part, sizeof(part), "Guest flip interval stale");
        else if (s.game_frame_ms < 0.0f) std::snprintf(part, sizeof(part), "Guest flip interval n/a");
        else std::snprintf(part, sizeof(part), "Guest flip interval %.1f ms", s.game_frame_ms);
        add();
        if (s.frame_ms < 0.0f) std::snprintf(part, sizeof(part), "Present interval n/a");
        else std::snprintf(part, sizeof(part), "Present interval %.1f ms", s.frame_ms);
        add();
    }
    if (mask & Upscaler) {
        std::snprintf(part, sizeof(part), "%s", s.upscaler.empty() ? "Native" : s.upscaler.c_str());
        add();
    }
    if (mask & Gpu) {
        const char* name = s.gpu_name.empty() ? "GPU" : s.gpu_name.c_str();
        if (s.gpu_percent < 0.0f) {
            std::snprintf(part, sizeof(part), "%s n/a", name);
        } else {
            std::snprintf(part, sizeof(part), "%s %.0f%%", name, s.gpu_percent);
        }
        add();
    }
    if (mask & Cpu) {
        if (s.cpu_percent < 0.0f) {
            std::snprintf(part, sizeof(part), "Process CPU n/a");
        } else {
            std::snprintf(part, sizeof(part), "Process CPU %.0f%%", s.cpu_percent);
        }
        add();
    }
    if (mask & Ram) {
        if (s.ram_used_gb < 0.0) std::snprintf(part, sizeof(part), "Process RAM n/a");
        else std::snprintf(part, sizeof(part), "Process RAM %.1f GiB", s.ram_used_gb);
        add();
    }
    if (mask & Vram) {
        pair(part, sizeof(part), "VRAM", s.vram_used_gb, s.vram_total_gb);
        add();
    }
    return out;
}

/// Starts the sampling thread the first time; later calls do nothing.
void Start(const Vulkan::Instance& instance, unsigned items = DefaultItems);
/// Completed guest flips, independently of overlay visibility or UI-only host presentations.
void OnGuestFlip(uint64_t render_serial = 0, uint64_t buffer_address = 0, bool blank = false);
/// Successful host presents, including UI-only loading frames, in the optional raw trace.
void OnHostPresented(uint64_t timestamp_ns, bool loading);
/// Latest process and guest-output values (caller fills host presentation and upscaler).
Sample Latest();

} // namespace Ps4PerfStats
