"""Button prompt sets are a contract: issue #59 was a silently retired Xbox set.

The launcher UI (worker) offers whatever prompts.SETS lists; a set removed here
falls back to the game's own PlayStation prompts without failing, so a player
with 'xbox' saved gets PS glyphs on their Xbox pad (issue #59). These checks
keep the shipped data (plan roles, mark art, folder names) consistent with the
offered sets, without game files.
"""
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))

import order1886_glyph_mod as glyphs  # noqa: E402
import order1886_prompts as prompts  # noqa: E402

ASSETS = Path(__file__).resolve().parents[1] / 'scripts' / 'button_prompts'


class ButtonPromptSets(unittest.TestCase):
    def test_offered_sets(self):
        self.assertEqual(prompts.SETS, ('playstation', 'xbox', 'keyboard'))
        self.assertFalse(getattr(prompts, 'RETIRED', ()),
                         'a retired set makes the launcher offer a choice that no longer works')

    def test_glyph_mod_knows_every_painted_set(self):
        for name in prompts.SETS:
            if name == 'playstation':
                continue
            self.assertIn(name, glyphs.SETS, f'glyphs.SETS needs the {name} folder name')

    def test_plan_has_xbox_roles_for_every_bitmap(self):
        bitmaps = json.loads((ASSETS / 'plan.json').read_text(encoding='utf-8'))['bitmaps']
        for entry in bitmaps:
            self.assertIn(entry.get('xbox'), ('face', 'label', 'keep'),
                          f"{entry['key']} has no reviewed Xbox role")
        reviewed = prompts.plan('xbox')
        self.assertEqual(len(reviewed), len(bitmaps))

    def test_mark_art_exists_for_every_painted_marking(self):
        bitmaps = json.loads((ASSETS / 'plan.json').read_text(encoding='utf-8'))['bitmaps']
        for entry in bitmaps:
            if entry['xbox'] == 'keep':
                continue
            glyph = entry.get('xbox_glyph', entry['glyph'])
            token = prompts.XBOX.get(glyph)
            if token is None:  # the D-pad: the game's own art stays
                self.assertTrue(glyph.startswith('dpad'),
                                f'{entry["key"]}: {glyph} needs an XBOX mapping')
                continue
            self.assertTrue((ASSETS / 'marks' / f'{token}.png').is_file(),
                            f'marks/{token}.png is missing for {glyph}')


if __name__ == '__main__':
    unittest.main()
