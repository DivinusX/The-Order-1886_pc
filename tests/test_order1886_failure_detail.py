"""Issue #18: the launcher shows why the GPU library stopped, not only "see GPU log above"."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import order1886  # noqa: E402

DEVICE_LOST = """GPU [Render.Recompiler] <Error> flatten_extended_userdata_pass.cpp:749 VisitPointer: Failed
radv/amdgpu: The CS has been cancelled because the context is lost. This context is guilty of a hard recovery.
GPU [Debug] <Critical> vk_scheduler.cpp:514 SubmitExecution: Assertion Failed!
Device lost during submit
STOP: GPU library assertion failed (see GPU log above)
"""

FMASK = """Runtime: save data 'Profile' mounted at /savedata0 (existing)
GPU [Render.Recompiler] <Critical> resource_patching_pass.cpp:273 ReportFmaskFailureContext: FMask failure
GPU [Debug] <Critical> resource_patching_pass.cpp:366 PatchImageSharp: Assertion Failed!
Unsupported FMask payload selectors R=1 G=4 B=7 A=7
STOP: GPU library assertion failed (see GPU log above)
"""

HOST_FAULT = """GPU [Common.Filesystem] <Error> io_file.cpp:202 Open: Failed to open the file
Host fault (signal 11) in /usr/lib/libvulkan_radeon.so+0x177830 (?), address 0x196
  #2 /usr/lib/libvulkan_radeon.so+0x177830 (?)
  #3 /usr/lib/libvulkan_radeon.so+0x180f79 (?)
  #4 /usr/lib/libvulkan_radeon.so+0x181e3f (?)
  #5 bin/gpu/libps4gpu.so+0x1b70ae (_ZN6Vulkan28CreateCachedGraphicsPipeline)
  #6 bin/gpu/libps4gpu.so+0x16c955 (_ZN6Vulkan16GraphicsPipelineC2)
"""


class FailureDetailTests(unittest.TestCase):
    def test_device_lost_keeps_the_driver_line_and_the_message(self):
        self.assertEqual(order1886.failure_detail(DEVICE_LOST), [
            'radv/amdgpu: The CS has been cancelled because the context is lost. This context is guilty of a hard recovery.',
            'GPU [Debug] <Critical> vk_scheduler.cpp:514 SubmitExecution: Assertion Failed!',
            'Device lost during submit'])

    def test_assertion_message_without_the_stop_line(self):
        self.assertEqual(order1886.failure_detail(FMASK), [
            'GPU [Debug] <Critical> resource_patching_pass.cpp:366 PatchImageSharp: Assertion Failed!',
            'Unsupported FMask payload selectors R=1 G=4 B=7 A=7'])

    def test_host_fault_with_its_first_frames(self):
        detail = order1886.failure_detail(HOST_FAULT)
        self.assertEqual(len(detail), 5)
        self.assertTrue(detail[0].startswith('Host fault (signal 11)'))
        self.assertTrue(detail[1].startswith('#2 '))

    def test_clean_logs_and_long_lines(self):
        self.assertEqual(order1886.failure_detail('Window: closed by user\n'), [])
        long = 'GPU [Debug] <Critical> x.cpp:1 F: Assertion Failed!\n' + 'y' * 1000 + '\n'
        self.assertEqual(len(order1886.failure_detail(long)[1]), 300)


if __name__ == '__main__':
    unittest.main()
