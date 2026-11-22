"""Issue #40: the SRT offset-failure detail budget is per shader for the whole
process, not per compile — fallback variants recompile every session and the old
per-compile cap reset with each one, letting one family write gigabyte logs.

The production ledger globals and TakeSrtFailureLog body are extracted verbatim and
exercised directly (same style as test_order1886_srt_codegen.py)."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp"


def function_body(source, signature):
    start = source.index(signature)
    # The opening brace of the body, not a brace inside a default argument.
    opening = source.index(") {", start) + 2
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def ledger_fixture(source):
    budget = [line for line in source.splitlines()
              if line.startswith("constexpr u32 SrtFailureDetailBudget")]
    assert len(budget) == 1, "one detail-budget constant is expected"
    enum = [line for line in source.splitlines()
            if line.startswith("enum class SrtFailureLogChoice")]
    assert len(enum) == 1, "one log-choice enum is expected"
    take = function_body(source, "static SrtFailureLogChoice TakeSrtFailureLog(")
    return r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
using u32 = uint32_t;
using u64 = uint64_t;
''' + budget[0] + "\n" + enum[0] + r'''
std::mutex g_srt_failure_log_mutex;
std::unordered_map<u64, u32> g_srt_failure_log_counts;
''' + take + r'''
int main() {
    const u64 glyph = 0x804aeeadULL;
    int logged = 0, announced = 0, silent = 0;
    for (int i = 0; i < 200; ++i) {
        const auto choice = TakeSrtFailureLog(glyph);
        if (choice == SrtFailureLogChoice::Log) ++logged;
        else if (choice == SrtFailureLogChoice::AnnounceCap) ++announced;
        else ++silent;
    }
    assert(logged == SrtFailureDetailBudget);
    // The cap is announced exactly once; a budget reset (the old bug) would loop.
    assert(announced == 1);
    assert(silent == 200 - (int)SrtFailureDetailBudget - 1);

    // The ledger is per shader, not global: a fresh hash still logs.
    assert(TakeSrtFailureLog(0xf5c17eecULL) == SrtFailureLogChoice::Log);

    // Compilation threads (warm-up pool, compiler, draw thread) share one ledger.
    const u64 shared = 0xc0ffeeULL;
    std::atomic<int> t_logged{0}, t_announced{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 100; ++i) {
                const auto choice = TakeSrtFailureLog(shared);
                if (choice == SrtFailureLogChoice::Log) ++t_logged;
                else if (choice == SrtFailureLogChoice::AnnounceCap) ++t_announced;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    assert(t_logged == (int)SrtFailureDetailBudget);
    assert(t_announced == 1);
}
'''


class SrtFailureLogBudgetTests(unittest.TestCase):
    def compile_and_run(self, fixture):
        compiler = shutil.which(os.environ.get("CXX", "clang++"))
        environment = os.environ.copy()
        if os.name == "nt" and compiler is None:
            candidate = Path(environment.get("MSYS2_ROOT", r"C:\msys64")) / "clang64/bin/clang++.exe"
            if candidate.is_file():
                compiler = str(candidate)
                environment["PATH"] = str(candidate.parent) + os.pathsep + environment["PATH"]
        self.assertIsNotNone(compiler, "A C++ compiler is required for the log-budget check")
        with tempfile.TemporaryDirectory(prefix="order1886-srt-budget-") as directory:
            cpp = Path(directory) / "ledger.cpp"
            exe = Path(directory) / ("ledger.exe" if os.name == "nt" else "ledger")
            cpp.write_text(fixture, encoding="utf-8")
            build = subprocess.run([compiler, "-std=c++23", "-UNDEBUG", "-pthread", str(cpp),
                                    "-o", str(exe)], capture_output=True, text=True, env=environment)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, env=environment)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_budget_is_per_shader_per_process_with_single_announcement(self):
        source = SOURCE.read_text(encoding="utf-8")
        self.compile_and_run(ledger_fixture(source))

    def test_failure_reports_route_companions_through_the_budget(self):
        source = SOURCE.read_text(encoding="utf-8")
        report = function_body(source, "static void ReportOffsetFailure(")
        # The report consults the ledger and prints the companion line itself.
        self.assertIn("TakeSrtFailureLog(pass_info.shader_info->pgm_hash)", report)
        self.assertIn("LOG_ERROR(Render_Recompiler, \"{}\", companion)", report)
        # No failure site emits its companion outside the budgeted report.
        for site in ["ReportOffsetFailure(pass_info, off_dw, \"pointer-base\"",
                     "ReportOffsetFailure(pass_info, src_off_dw, \"table-element\"",
                     "ReportOffsetFailure(pass_info, off_dw, \"unsupported-opcode\""]:
            self.assertIn(site, source)
        self.assertNotIn("LOG_ERROR(Render_Recompiler, \"Failed to compute offset for SRT walker\");",
                         source)


if __name__ == "__main__":
    unittest.main()
