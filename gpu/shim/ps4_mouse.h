// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace Ps4Mouse {
constexpr bool WantsCapture(bool enabled, bool released, bool blocked,
                            bool event_focus, bool native_focus) {
    return enabled && !released && !blocked && event_focus && native_focus;
}

struct Sample {
    std::uint8_t x{128}, y{128};
    std::uint32_t buttons{}; // bit 0: left mouse button; bit 1: right
};

/// Mouse-to-right-stick shaping. The game reads a stick, so a mouse can only drive a turn rate;
/// these undo what the game's stick handling does to small and diagonal motion.
struct Tuning {
    double window_ms = 16;  ///< box average of mouse speed (was a 25 ms exponential: input lag)
    double deadzone = 0;    ///< radial stick magnitude (0..0.9) any motion jumps past
    double curve = 1;       ///< inverse response exponent: magnitude^(1/curve) before the game's curve
    double gain = 1;        ///< multiplier on top of the launcher's sensitivity

    /// key=value lines (window_ms, deadzone, curve, gain); unknown keys and bad values are ignored.
    static Tuning Parse(const std::string& text) {
        Tuning t;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string s) {
                const auto a = s.find_first_not_of(" \t\r");
                const auto b = s.find_last_not_of(" \t\r");
                return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
            };
            const std::string key = trim(line.substr(0, eq));
            const std::string value = trim(line.substr(eq + 1));
            char* rest = nullptr;
            const double v = std::strtod(value.c_str(), &rest);
            if (value.empty() || !rest || *rest || !std::isfinite(v)) continue;
            if (key == "window_ms") t.window_ms = std::clamp(v, 1.0, 100.0);
            else if (key == "deadzone") t.deadzone = std::clamp(v, 0.0, 0.9);
            else if (key == "curve") t.curve = std::clamp(v, 0.25, 4.0);
            else if (key == "gain") t.gain = std::clamp(v, 0.1, 10.0);
        }
        return t;
    }
};

/// Maps a mouse speed (counts/s, both axes) to stick bytes: radial, so diagonals keep their angle.
inline void Shape(double vx, double vy, double gain, const Tuning& t, std::uint8_t& x,
                  std::uint8_t& y) {
    const double sx = vx * gain / 127, sy = vy * gain / 127; // 1.0 = stick at full deflection
    const double m = std::hypot(sx, sy);
    if (!(m > 1e-6)) {
        x = y = 128;
        return;
    }
    const double shaped = t.deadzone + (1 - t.deadzone) * std::pow(std::min(m, 1.0), 1 / t.curve);
    const auto axis = [](double v) {
        return std::uint8_t(std::clamp(std::lround(128 + v * 127), 0L, 255L));
    };
    x = axis(sx / m * shaped);
    y = axis(sy / m * shaped);
}

// SDL events and capture belong to the window thread. Guest pad reads only copy
// a published sample; polling more often must not consume or amplify mouse motion.
class Input {
public:
    void Capture(bool value, double now) {
        std::scoped_lock lock{mutex};
        if (captured == value) return;
        captured = value;
        last = now;
        dx = dy = 0;
        history.clear();
        sample = {};
    }
    void Motion(float x, float y) {
        std::scoped_lock lock{mutex};
        if (!captured || !std::isfinite(x) || !std::isfinite(y)) return;
        dx = std::clamp(dx + x, -100000.0, 100000.0);
        dy = std::clamp(dy + y, -100000.0, 100000.0);
    }
    void Button(std::uint32_t mask, bool down) {
        std::scoped_lock lock{mutex};
        if (!captured) return;
        if (down) sample.buttons |= mask;
        else sample.buttons &= ~mask;
    }
    void Update(double now, int sensitivity, bool invert_y) {
        std::scoped_lock lock{mutex};
        ReloadTuning(now);
        const double dt = now - last;
        if (!(dt > 0)) return;
        last = now;
        if (!captured || dt > 0.25) {
            dx = dy = 0;
            history.clear();
            sample.x = sample.y = 128;
            if (!captured) sample.buttons = 0;
            return;
        }
        // Speed over the last window_ms of real time (box average), not per rendered frame:
        // the same hand motion gives the same stick value at any frame rate.
        // Each entry is the motion of one update interval [start, end]. Average over the newest
        // intervals that reach back to the window edge, divided by the time they really cover
        // (a 33 ms frame with a 16 ms window counts as 33 ms, not 16).
        history.push_back({now - dt, now, dx, dy});
        dx = dy = 0;
        const double edge = now - tuning.window_ms / 1000;
        while (history.size() > 1 && history.front().end <= edge) history.pop_front();
        double sum_x = 0, sum_y = 0;
        for (const auto& h : history) {
            sum_x += h.dx;
            sum_y += h.dy;
        }
        const double span = std::max(now - history.front().start, 1e-6);
        const double vx = sum_x / span, vy = sum_y / span;
        const double gain = 0.08 * std::clamp(sensitivity, 10, 500) / 100 * tuning.gain;
        Shape(vx, invert_y ? -vy : vy, gain, tuning, sample.x, sample.y);
    }
    Ps4Mouse::Sample Sample() const {
        std::scoped_lock lock{mutex};
        return sample;
    }
    Tuning CurrentTuning() const {
        std::scoped_lock lock{mutex};
        return tuning;
    }
    void SetTuning(const Tuning& value) {
        std::scoped_lock lock{mutex};
        tuning = value;
        tuning_path.clear(); // tests: stop file reloads
        tuning_checked = true;
    }

private:
    /// <ORDER1886_USER_DIR>/mouse-tuning.ini, re-read when it changes (checked twice a second).
    void ReloadTuning(double now) {
        if (!tuning_checked) {
            tuning_checked = true;
            if (const char* dir = std::getenv("ORDER1886_USER_DIR"); dir && *dir) {
                tuning_path = std::string(dir) + "/mouse-tuning.ini";
            }
        }
        if (tuning_path.empty() || now - tuning_poll < 0.5) return;
        tuning_poll = now;
        struct stat st {};
        if (stat(tuning_path.c_str(), &st) != 0) {
            if (tuning_mtime != 0) {
                tuning = {};
                tuning_mtime = 0;
                std::printf("Mouse: tuning file removed, defaults restored\n");
            }
            return;
        }
        const double mtime = double(st.st_mtime) + double(st.st_size) * 1e-9;
        if (mtime == tuning_mtime) return;
        tuning_mtime = mtime;
        std::string text;
        if (FILE* f = std::fopen(tuning_path.c_str(), "rb")) {
            char buffer[4096];
            const size_t n = std::fread(buffer, 1, sizeof(buffer), f);
            std::fclose(f);
            text.assign(buffer, n);
        }
        tuning = Tuning::Parse(text);
        std::printf("Mouse: tuning window_ms=%g deadzone=%g curve=%g gain=%g\n", tuning.window_ms,
                    tuning.deadzone, tuning.curve, tuning.gain);
        std::fflush(stdout);
    }

    struct Entry {
        double start, end, dx, dy;
    };
    mutable std::mutex mutex;
    bool captured{};
    double last{}, dx{}, dy{};
    std::deque<Entry> history;
    Ps4Mouse::Sample sample{};
    Tuning tuning{};
    bool tuning_checked{};
    std::string tuning_path;
    double tuning_poll{-1}, tuning_mtime{};
};
inline Input input;
} // namespace Ps4Mouse
