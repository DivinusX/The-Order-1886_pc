"""Catalogue recipes are applied inside the derived prepared image, never to the game."""
import json
from pathlib import Path
import tempfile
import unittest

import order1886
from scripts import order1886_patches as patches
from scripts import order1886_frame_rate as timing

ROOT = Path(__file__).resolve().parents[1]
METADATA = {'TITLE_ID': 'CUSA00076', 'APP_VER': '01.02', 'VERSION': '01.00'}


def container(image):
    """A minimal PS4BOOT2 container holding `image`, shaped like prepare.prepare's output."""
    header = timing.HEADER.size + 24 + 128 + 32
    body = timing.HEADER.pack(b'PS4BOOT2', len(image), 0xa0, 1, 1, 1, 1) + bytes(24 + 128 + 32)
    return body + bytes(image)


def third_party(offset=0x1C0, expected=b'ORIGINAL', replacement=b'PATCHED!'):
    return {'id': 'order1886-test-third-party',
            'title': 'Test third-party recipe',
            'description': 'Not shipped; proves the preparation pipeline applies catalogued recipes.',
            'title_ids': (METADATA['TITLE_ID'],),
            'guest_executable': 'eboot.bin', 'image': patches.IMAGE_KIND,
            'edits': ({'offset': offset, 'expected': expected, 'replacement': replacement},),
            'experimental': True, 'warning': 'test only'}


class SelectionTests(unittest.TestCase):
    def test_the_builtin_recipes_cannot_be_applied_twice(self):
        for identifier in patches.BUILTIN_IDS:
            with self.subTest(identifier=identifier), self.assertRaises(ValueError) as caught:
                patches.select([identifier], METADATA)
            self.assertIn('its own preparation profile', str(caught.exception))

    def test_an_unknown_identifier_is_an_error_not_a_default(self):
        with self.assertRaises(ValueError):
            patches.select(['order1886-does-not-exist'], METADATA)

    def test_a_title_the_recipe_does_not_list_is_refused(self):
        original = patches.RECIPES
        try:
            patches.RECIPES = original + (third_party(),)
            with self.assertRaisesRegex(ValueError, 'CUSA00785'):
                patches.select([third_party()['id']], {**METADATA, 'TITLE_ID': 'CUSA00785'})
        finally:
            patches.RECIPES = original


class PreparedContainerTests(unittest.TestCase):
    def test_no_selection_leaves_the_image_untouched(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'boot.bin'
            data = container(bytearray(0x400))
            path.write_bytes(data)
            self.assertEqual(patches.apply_to_prepared(path, (), METADATA), [])
            self.assertEqual(path.read_bytes(), data)

    def test_a_catalogued_recipe_edits_the_prepared_image_and_reports_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'boot.bin'
            image = bytearray(0x400)
            image[0x1C0:0x1C8] = b'ORIGINAL'
            path.write_bytes(container(image))
            applied = patches.apply_to_prepared(path, (third_party(),), METADATA)
            self.assertEqual(len(applied), 1)
            self.assertEqual(applied[0]['id'], third_party()['id'])
            self.assertEqual(applied[0]['edits'][0]['before'], b'ORIGINAL'.hex())
            self.assertEqual(applied[0]['edits'][0]['after'], b'PATCHED!'.hex())
            patched = path.read_bytes()
            self.assertIn(b'PATCHED!', patched)
            self.assertNotIn(b'ORIGINAL', patched)

    def test_the_guest_executable_is_never_the_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            game = Path(temporary) / 'boot.bin'
            game.write_bytes(container(bytearray(0x400)))
            before = game.read_bytes()
            # The caller is expected to pass the prepared artifact; a container without the
            # expected bytes must fail rather than "succeed" on a different build.
            with self.assertRaises(ValueError) as caught:
                patches.apply_to_prepared(game, (third_party(),), METADATA)
            self.assertIn('expected bytes are absent', str(caught.exception))
            self.assertEqual(game.read_bytes(), before)

    def test_a_non_container_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'boot.bin'
            path.write_bytes(b'not a container at all' * 8)
            with self.assertRaises(ValueError):
                patches.apply_to_prepared(path, (third_party(),), METADATA)

    def test_an_edit_outside_the_image_is_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'boot.bin'
            path.write_bytes(container(bytearray(0x200)))
            with self.assertRaises(ValueError) as caught:
                patches.apply_to_prepared(path, (third_party(offset=0x1FF),), METADATA)
            self.assertIn('outside the prepared image', str(caught.exception))

    def test_qte_mash_requires_the_native_frame_duration_before_editing(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'boot.bin'
            compare, constants = 0xb106d7, 0x143e01c
            instruction = bytes.fromhex('c5f82e054dd99200 c5fa100d49d99200 7608 c5fa590d43d99200')
            floats = bytes.fromhex('2f88083d 0000803f 000000b5 bd378635 00000034 0000803f 9d00f041')
            image = bytearray(constants + len(floats))
            image[compare:compare + len(instruction)] = instruction
            image[constants:constants + len(floats)] = floats
            image[constants] ^= 1
            unrecognized = container(image)
            path.write_bytes(unrecognized)
            profile = patches.apply_qte_mash(path, METADATA)
            self.assertFalse(profile['applied'])
            self.assertIn(hex(constants), profile['reason'])
            self.assertEqual(path.read_bytes(), unrecognized)

            image[constants] ^= 1
            path.write_bytes(container(image))
            profile = patches.apply_qte_mash(path, METADATA)
            self.assertTrue(profile['applied'], profile.get('reason'))
            # The comparison now references the 30 FPS frame duration; the rest is unchanged.
            image[compare + 4:compare + 8] = (constants - compare - 8).to_bytes(4, 'little')
            self.assertEqual(path.read_bytes(), container(image))


class PreparationPipelineTests(unittest.TestCase):
    def test_the_selection_is_part_of_the_preparation_fingerprint(self):
        # A different patch selection must not reuse the cached prepared image.
        source = (ROOT / 'order1886.py').read_text(encoding='utf-8')
        self.assertIn('patches=', source)
        self.assertIn('patch_catalogue.patch_fingerprint', source)
        self.assertIn('apply_to_prepared', source)

    def test_the_fingerprint_helper_is_stable_and_order_independent(self):
        one = patches.patch_fingerprint((third_party(),))
        two = patches.patch_fingerprint((third_party(),))
        self.assertEqual(one, two)
        self.assertNotEqual(one, patches.patch_fingerprint(()))

    def test_fingerprints_sort_several_recipes_by_id(self):
        # Two selections are hashed together (a cheat plus another recipe): the helper must
        # order the entries itself, because Python cannot compare the dicts against each
        # other, and the same pair in reverse order must give the same fingerprint.
        first = dict(third_party(), id='aaa-test-recipe')
        second = dict(third_party(), id='bbb-test-recipe',
                      edits=({'offset': 0x200, 'expected': b'ORIGINAL', 'replacement': b'PATCHED!'},))
        forward = patches.patch_fingerprint((first, second))
        reverse = patches.patch_fingerprint((second, first))
        self.assertEqual(forward, reverse)
        self.assertEqual([entry['id'] for entry in forward], ['aaa-test-recipe', 'bbb-test-recipe'])


if __name__ == '__main__':
    unittest.main()
