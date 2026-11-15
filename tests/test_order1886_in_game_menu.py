"""The in-game menu (F1) is on by default through the launcher's game_menu setting.

The backend still takes the opt-in --in-game-menu switch (in_game_menu in its settings); the
launcher decides it from game_menu, a new key because every saved settings.json from before
carries the old opt-in in_game_menu=False, which a new default on that key could never beat.
"""
from pathlib import Path
import unittest

import order1886
from launcher import order1886_common as launcher
from launcher import order1886_options as options
from launcher import order1886_worker as worker

ROOT = Path(__file__).resolve().parents[1]


def environment(settings):
    return order1886.runtime_environment(ROOT / 'out' / 'runtime.exe', ROOT / 'out' / 'state', settings, {})


def launch_values(**overrides):
    base = {'game_dir': 'game', 'display_mode': 'windowed', 'vsync': True, 'show_fps': False,
            'frame_rate': 30, 'aspect_ratio': 'original', 'mouse_look': True,
            'mouse_sensitivity': 100, 'mouse_invert_y': False, 'readbacks': 'relaxed',
            'performance_capture': False, 'fps_cap': 120, 'htile_unqualified': False,
            'synchronous_recording': False, 'texture_update_locking': False}
    base.update(overrides)
    return base


class InGameMenuTests(unittest.TestCase):
    def test_the_backend_switch_is_off_unless_asked_for(self):
        self.assertEqual(environment({})['ORDER1886_SETTINGS_MENU'], '0')
        self.assertEqual(environment({'in_game_menu': False})['ORDER1886_SETTINGS_MENU'], '0')

    def test_the_backend_switch_reaches_the_runtime(self):
        self.assertEqual(environment({'in_game_menu': True})['ORDER1886_SETTINGS_MENU'], '1')

    def test_only_a_real_boolean_is_accepted(self):
        for invalid in ('1', 1, None, [True]):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                environment({'in_game_menu': invalid})

    def test_the_catalog_exposes_the_menu_on_by_default(self):
        field = next(item for item in options.FIELDS if item['key'] == 'game_menu')
        self.assertEqual(field['default'], True)
        self.assertEqual(field['kind'], 'boolean')
        self.assertEqual(field['page'], 'performance')
        self.assertTrue(field['available'])
        self.assertIn('F1', field['description'])
        self.assertFalse([item for item in options.FIELDS if item['key'] == 'in_game_menu'],
                         'the old opt-in key must not come back as a control')

    def test_the_launch_command_follows_game_menu_and_ignores_the_old_key(self):
        self.assertIn('--in-game-menu', launcher.backend_command(launch_values()))
        self.assertIn('--in-game-menu', launcher.backend_command(launch_values(game_menu=True)))
        self.assertNotIn('--in-game-menu', launcher.backend_command(launch_values(game_menu=False)))
        # A settings.json saved by an earlier launcher: the stored opt-in is not consulted.
        self.assertIn('--in-game-menu', launcher.backend_command(launch_values(in_game_menu=False)))
        self.assertNotIn('--in-game-menu',
                         launcher.backend_command(launch_values(in_game_menu=True, game_menu=False)))

    def test_the_setting_round_trips_through_the_worker(self):
        with worker_temp_state() as state:
            app = worker.Worker(state, ROOT)
            values = app.dispatch('settings.set', {'values': {'game_menu': False}})['settings']
            self.assertIs(values['game_menu'], False)
            values = app.dispatch('settings.set', {'values': {'game_menu': True}})['settings']
            self.assertIs(values['game_menu'], True)
            self.assertIs(launcher.load_launch_settings(state, {})['game_menu'], True)
            self.assertIs(launcher.load_launch_settings(state, {'in_game_menu': False})['game_menu'], True)
            self.assertIs(launcher.load_launch_settings(state, {'game_menu': False})['game_menu'], False)


class worker_temp_state:
    def __enter__(self):
        import tempfile
        self._folder = tempfile.TemporaryDirectory()
        return Path(self._folder.name)

    def __exit__(self, *_exc):
        self._folder.cleanup()


if __name__ == '__main__':
    unittest.main()
