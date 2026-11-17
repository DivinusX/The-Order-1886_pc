// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include "ps4_mouse.h"

int main() {
    const Ps4Mouse::Tuning neutral{16, 0, 1, 1};
    assert(Ps4Mouse::WantsCapture(true, false, false, true, true));
    assert(!Ps4Mouse::WantsCapture(true, false, false, false, true));
    assert(!Ps4Mouse::WantsCapture(true, false, false, true, false));
    assert(!Ps4Mouse::WantsCapture(true, true, false, true, true));
    assert(!Ps4Mouse::WantsCapture(true, false, true, true, true));
    assert(!Ps4Mouse::WantsCapture(false, false, false, true, true));
    Ps4Mouse::Input focus_race;
    focus_race.SetTuning(neutral);
    focus_race.Capture(Ps4Mouse::WantsCapture(true, false, false, true, true), 0);
    focus_race.Motion(100, 100);
    focus_race.Button(3, true);
    // A focus gain is already queued, so native flags remain true at the lost event.
    focus_race.Capture(Ps4Mouse::WantsCapture(true, false, false, false, true), 0.01);
    assert(focus_race.Sample().x == 128 && focus_race.Sample().buttons == 0);
    focus_race.Capture(Ps4Mouse::WantsCapture(true, false, false, true, true), 0.02);
    focus_race.Update(0.03, 100, false);
    assert(focus_race.Sample().x == 128 && focus_race.Sample().y == 128);
    Ps4Mouse::Input input;
    input.SetTuning(neutral);
    auto sample = input.Sample();
    assert(sample.x == 128 && sample.y == 128 && sample.buttons == 0);
    input.Motion(1000, 1000);
    input.Button(1, true);
    input.Update(1, 100, false);
    assert(input.Sample().x == 128 && input.Sample().buttons == 0);
    input.Capture(true, 1);
    input.Motion(10, -10);
    input.Button(1, true);
    input.Update(1.01, 100, false);
    sample = input.Sample();
    assert(sample.x > 128 && sample.y < 128 && sample.buttons == 1);
    assert(input.Sample().x == sample.x); // several guest pad reads do not consume motion
    input.Update(1.02, 200, true);
    assert(input.Sample().x > 128 && input.Sample().y > 128);
    input.Update(1.5, 100, false);
    assert(input.Sample().x == 128 && input.Sample().buttons == 1);
    input.Capture(false, 1.03);
    assert(input.Sample().x == 128 && input.Sample().y == 128 && input.Sample().buttons == 0);
    input.Motion(100000, 100000);
    input.Capture(true, 2);
    input.Update(2.01, 100, false);
    assert(input.Sample().x == 128 && input.Sample().y == 128);
    input.Motion(std::numeric_limits<float>::quiet_NaN(), 0);
    input.Update(2.02, 100, false);
    assert(input.Sample().x == 128);
    for (int fps : {30, 60, 120, 240}) {
        Ps4Mouse::Input moving;
        moving.SetTuning(neutral);
        moving.Capture(true, 0);
        for (int i = 1; i <= fps; ++i) {
            moving.Motion(600.0f / fps, -600.0f / fps);
            moving.Update(double(i) / fps, 100, false);
        }
        assert(std::abs(int(moving.Sample().x) - 176) <= 1);
        assert(std::abs(int(moving.Sample().y) - 80) <= 1);
        for (int i = 1; i <= fps; ++i) moving.Update(1 + double(i) / fps, 100, false);
        assert(moving.Sample().x == 128 && moving.Sample().y == 128);
    }
    // Tuning file parsing: known keys, clamping, comments, junk ignored.
    {
        const auto t = Ps4Mouse::Tuning::Parse(
            "# comment\nwindow_ms = 8\ndeadzone=0.25 # trailing\ncurve=2\ngain=1.5\nbogus=3\n"
            "deadzone_x=9\ncurve=abc\n");
        assert(t.window_ms == 8 && t.deadzone == 0.25 && t.curve == 2 && t.gain == 1.5);
        const auto c = Ps4Mouse::Tuning::Parse("deadzone=5\ngain=-1\nwindow_ms=0\n");
        assert(c.deadzone == 0.9 && c.gain == 0.1 && c.window_ms == 1);
        const auto d = Ps4Mouse::Tuning::Parse("");
        assert(d.window_ms == 6 && d.deadzone == 0.18 && d.curve == 1.4 && d.gain == 2.5);
    }
    // Shaping: still is neutral; any motion jumps past the dead zone; diagonals keep their angle.
    {
        Ps4Mouse::Tuning t = neutral;
        t.deadzone = 0.25;
        std::uint8_t x, y;
        Ps4Mouse::Shape(0, 0, 0.08, t, x, y);
        assert(x == 128 && y == 128);
        Ps4Mouse::Shape(1, 0, 0.08, t, x, y); // a crawl: just past the dead zone
        assert(x >= 128 + 31 && x <= 128 + 33 && y == 128);
        Ps4Mouse::Shape(-1e6, 0, 0.08, t, x, y); // saturates at full deflection
        assert(x <= 1 && y == 128);
        Ps4Mouse::Shape(500, 500, 0.08, t, x, y);
        assert(int(x) - 128 == int(y) - 128); // 45 degrees in, 45 degrees out
        Ps4Mouse::Tuning curved = neutral;
        curved.curve = 2;
        Ps4Mouse::Shape(127 * 0.25 / 0.08, 0, 0.08, curved, x, y); // 25% in -> sqrt -> 50% out
        assert(std::abs(int(x) - (128 + 64)) <= 1);
    }
    // Box average: a 30 fps frame (33 ms) longer than the 16 ms window does not inflate speed.
    {
        Ps4Mouse::Input slow;
        slow.SetTuning(neutral);
        slow.Capture(true, 0);
        for (int i = 1; i <= 30; ++i) {
            slow.Motion(600.0f / 30, 0);
            slow.Update(double(i) / 30, 100, false);
        }
        assert(std::abs(int(slow.Sample().x) - 176) <= 1);
    }
    // Lower latency than the old 25 ms exponential: one window after motion stops, it is neutral.
    {
        Ps4Mouse::Input quick;
        quick.SetTuning(neutral);
        quick.Capture(true, 0);
        for (int i = 1; i <= 120; ++i) {
            quick.Motion(600.0f / 120, 0);
            quick.Update(double(i) / 120, 100, false);
        }
        quick.Update(1 + 1.0 / 120, 100, false);
        quick.Update(1 + 2.0 / 120, 100, false);
        quick.Update(1 + 3.0 / 120, 100, false);
        assert(quick.Sample().x == 128);
    }
    // Shipped defaults: a slow crawl already turns (past the dead zone), and nothing moves at rest.
    {
        Ps4Mouse::Input tuned;
        tuned.SetTuning(Ps4Mouse::Tuning{});
        tuned.Capture(true, 0);
        tuned.Motion(1, 0);
        tuned.Update(0.004, 100, false);
        assert(tuned.Sample().x >= 128 + 23);
        tuned.Update(0.05, 100, false);
        assert(tuned.Sample().x == 128 && tuned.Sample().y == 128);
    }
    std::puts("PASS: mouse motion, focus reset, inversion, buttons, poll-rate independence, "
              "tuning, dead zone, curve, diagonals and latency");
}
