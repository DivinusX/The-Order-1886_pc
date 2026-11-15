"""The frozen worker must not hand PyInstaller's LD_LIBRARY_PATH to the runtime or its wrapper."""
import os
import sys
import unittest
from unittest.mock import patch

import order1886

RUNTIME = os.path.join(os.sep, 'pkg', 'bin', 'order1886-runtime')


@unittest.skipIf(os.name == 'nt', 'LD_LIBRARY_PATH is a POSIX concern')
class FrozenLibraryPathTests(unittest.TestCase):
    def environment(self, inherited, frozen=True, bundle='/pkg/bin/order1886-worker/_internal'):
        with patch.object(sys, 'frozen', frozen, create=True), patch.object(sys, '_MEIPASS', bundle, create=True):
            return order1886.runtime_process_environment(RUNTIME, inherited)

    def test_pyinstaller_original_value_is_restored(self):
        env = self.environment({'LD_LIBRARY_PATH': '/pkg/bin/order1886-worker/_internal:/opt/lib',
                                'LD_LIBRARY_PATH_ORIG': '/opt/lib', 'PATH': '/usr/bin'})
        self.assertEqual(env['LD_LIBRARY_PATH'], '/opt/lib')
        self.assertNotIn('LD_LIBRARY_PATH_ORIG', env)

    def test_bundle_folder_alone_is_removed_entirely(self):
        env = self.environment({'LD_LIBRARY_PATH': '/pkg/bin/order1886-worker/_internal', 'PATH': '/usr/bin'})
        self.assertNotIn('LD_LIBRARY_PATH', env)
        env = self.environment({'LD_LIBRARY_PATH_ORIG': '', 'LD_LIBRARY_PATH': '/pkg/bin/order1886-worker/_internal'})
        self.assertNotIn('LD_LIBRARY_PATH', env)

    def test_other_entries_survive_without_the_original_marker(self):
        env = self.environment({'LD_LIBRARY_PATH': '/pkg/bin/order1886-worker/_internal:/opt/lib'})
        self.assertEqual(env['LD_LIBRARY_PATH'], '/opt/lib')

    def test_a_source_checkout_leaves_the_variable_alone(self):
        env = self.environment({'LD_LIBRARY_PATH': '/opt/lib', 'LD_LIBRARY_PATH_ORIG': '/x'}, frozen=False)
        self.assertEqual(env['LD_LIBRARY_PATH'], '/opt/lib')
        self.assertEqual(env['LD_LIBRARY_PATH_ORIG'], '/x')


if __name__ == '__main__':
    unittest.main()
