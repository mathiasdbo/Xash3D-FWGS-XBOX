"""
Failure-path tests for the two menu-art bake tools.

Run:  python -m unittest tools/test_menu_art_bakes.py

Why these exist: Codex review of the tools' first version found both
verification paths could report success without proving anything. The title
bake's `--check` compared each layer's dimensions against a manifest that
recorded only dimensions, so blanking every layer while keeping its size
reached the success message; and the art bake's `--check`, documented as
writing nothing, wrote a `duke.tga.check` beside the asset and deleted it,
destroying any pre-existing file of that name and failing outright against a
read-only directory. Both are the false-clean measurement this repository's
own rules exist to prevent, so every test here asserts that a BROKEN input is
REPORTED - not that the happy path works.
"""
import importlib.util
import os
import shutil
import struct
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name, filename):
    spec = importlib.util.spec_from_file_location(name, os.path.join(_HERE, filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


art = _load("xbox_menu_art", "xbox-menu-art.py")
title = _load("xbox_title_bake", "xbox-title-bake.py")


def _tiny_master(path, width=64, height=48):
    """A master with real content and a transparent margin, as an export has."""
    px = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            o = (y * width + x) * 4
            inside = 8 <= x < width - 8 and 6 <= y < height - 6
            px[o] = 200 if inside else 0
            px[o + 1] = 120 if inside else 0
            px[o + 2] = 40 if inside else 0
            px[o + 3] = 255 if inside else 0
    art.write_png(path, px, width, height)


def _tiny_layer(path, width=16, height=8):
    px = bytearray(width * height * 4)
    for i in range(0, len(px), 4):
        px[i], px[i + 1], px[i + 2], px[i + 3] = 220, 180, 30, 255
    title.write_tga(path, px, width, height)


def _blank_in_place(path):
    """Replace a TGA's pixels with transparent black, keeping its dimensions."""
    with open(path, "rb") as f:
        data = bytearray(f.read())
    for i in range(18, len(data)):
        data[i] = 0
    with open(path, "wb") as f:
        f.write(data)


class LegendGlyphShape(unittest.TestCase):
    """The tracked glyph must pass, and a nearly-empty one must not.

    Codex review round 2 found the first validator blessed a single opaque
    centre texel on an otherwise empty image: square, clear corners, opaque
    centre, radial maxima never climbing. These two tests are the pair that
    keeps that closed - one proving the predicate still accepts the real
    asset, so it cannot be satisfied by rejecting everything.
    """

    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def _write(self, name, px, size):
        path = os.path.join(self.dir, name)
        with open(path, "wb") as f:
            f.write(art.encode_tga(px, size, size, True))
        return path

    def test_the_tracked_glyph_passes(self):
        tracked = os.path.join(os.path.dirname(_HERE), "assets", "gfx", "shell", "legend_button.tga")
        self.assertTrue(os.path.exists(tracked), "the shipped legend glyph is missing from the tree")
        self.assertEqual([], art.check_legend_button(tracked))

    def test_a_single_opaque_centre_texel_is_rejected(self):
        size = 32
        px = bytearray(size * size * 4)
        o = ((size // 2) * size + size // 2) * 4
        px[o] = px[o + 1] = px[o + 2] = px[o + 3] = 255
        self.assertNotEqual([], art.check_legend_button(self._write("dot.tga", px, size)))

    def test_a_disc_too_small_for_its_box_is_rejected(self):
        size = 32
        px = bytearray(size * size * 4)
        c = (size - 1) / 2.0
        for y in range(size):
            for x in range(size):
                if ((x - c) ** 2 + (y - c) ** 2) ** 0.5 <= 3.0:
                    o = (y * size + x) * 4
                    px[o] = px[o + 1] = px[o + 2] = px[o + 3] = 255
        self.assertNotEqual([], art.check_legend_button(self._write("small.tga", px, size)))


class MenuArtCheckMode(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.out = os.path.join(self.dir, "shell")
        os.makedirs(self.out)
        self.master = os.path.join(self.dir, "Master.png")
        _tiny_master(self.master)
        # An intact output directory carries both assets - --check reports a
        # missing shipped one as a failure, which is correct for the real
        # assets/gfx/shell and would otherwise make every "intact" assertion
        # here model an incomplete tree instead.
        shutil.copy(os.path.join(os.path.dirname(_HERE), "assets", "gfx", "shell", "legend_button.tga"),
                    os.path.join(self.out, "legend_button.tga"))
        self.common = ["--output", self.out, "--master", self.master, "--width", "24"]
        self.assertEqual(0, art.main(self.common))

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def _snapshot(self):
        return {n: os.stat(os.path.join(self.out, n)).st_size for n in sorted(os.listdir(self.out))}

    def test_an_intact_bake_passes_its_own_check(self):
        # Without this, a checker that always failed would satisfy every other
        # test in this file.
        self.assertEqual(0, art.main(self.common + ["--check"]))

    def test_check_writes_nothing_into_the_output_directory(self):
        before = self._snapshot()
        art.main(self.common + ["--check"])
        self.assertEqual(before, self._snapshot())

    def test_check_does_not_disturb_a_file_named_like_its_old_scratch_file(self):
        # The first version wrote and deleted exactly this path.
        bystander = os.path.join(self.out, "duke.tga.check")
        with open(bystander, "wb") as f:
            f.write(b"someone else's file")
        art.main(self.common + ["--check"])
        self.assertTrue(os.path.exists(bystander), "--check deleted an unrelated file")
        with open(bystander, "rb") as f:
            self.assertEqual(b"someone else's file", f.read())

    def test_an_edited_asset_fails_the_check(self):
        duke = os.path.join(self.out, "duke.tga")
        with open(duke, "r+b") as f:
            f.seek(18)
            f.write(b"\x00\x00\x00\x00")
        self.assertEqual(1, art.main(self.common + ["--check"]))

    def test_a_missing_master_is_reported_as_unverifiable_not_as_a_mismatch(self):
        os.remove(self.master)
        self.assertEqual(1, art.main(self.common + ["--check"]))

    def test_baking_larger_than_the_master_is_refused(self):
        with self.assertRaises(ValueError):
            art.bake_from_master(self.master, 4096, False)


class TitleBakeCheckMode(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.game = os.path.join(self.dir, "valve")
        os.makedirs(os.path.join(self.game, "resource"))
        for rel in title.LAYERS:
            _tiny_layer(os.path.join(self.game, rel))
        self.out = os.path.join(self.dir, "bake")
        self.assertIsNotNone(title.bake(self.game, self.out, 640))
        self.assertEqual(0, title.check(self.out))

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_blanking_a_layer_in_place_fails_the_check(self):
        # Same dimensions, no pixels: what the dimensions-only validator passed.
        _blank_in_place(os.path.join(self.out, title.LAYERS[0]))
        self.assertEqual(1, title.check(self.out))

    def test_blanking_a_layer_fails_even_when_its_digest_is_refreshed(self):
        import hashlib
        import json
        path = os.path.join(self.out, title.LAYERS[0])
        _blank_in_place(path)
        manifest_path = os.path.join(self.out, "title-bake.json")
        with open(manifest_path) as f:
            manifest = json.load(f)
        with open(path, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        for entry in manifest["layers"]:
            if entry["path"] == title.LAYERS[0]:
                entry["sha256"] = digest
        with open(manifest_path, "w") as f:
            json.dump(manifest, f)
        self.assertEqual(1, title.check(self.out), "a digest that agrees with a blank layer still passed")

    def test_a_layer_reduced_to_one_pixel_fails_even_with_a_refreshed_digest(self):
        # One byte away from the blank-layer case, which is exactly how far the
        # first content check reached.
        import hashlib
        import json
        rel = title.LAYERS[0]
        path = os.path.join(self.out, rel)
        rgba, w, h = title.read_tga(path)
        blank = bytearray(len(rgba))
        blank[0] = blank[1] = blank[2] = blank[3] = 255
        title.write_tga(path, blank, w, h)
        manifest_path = os.path.join(self.out, "title-bake.json")
        with open(manifest_path) as f:
            manifest = json.load(f)
        with open(path, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        for entry in manifest["layers"]:
            if entry["path"] == rel:
                entry["sha256"] = digest
        with open(manifest_path, "w") as f:
            json.dump(manifest, f)
        self.assertEqual(1, title.check(self.out), "a layer with one visible pixel passed")

    def test_a_manifest_without_digests_is_refused(self):
        import json
        manifest_path = os.path.join(self.out, "title-bake.json")
        with open(manifest_path) as f:
            manifest = json.load(f)
        for entry in manifest["layers"]:
            entry.pop("sha256", None)
        with open(manifest_path, "w") as f:
            json.dump(manifest, f)
        self.assertEqual(1, title.check(self.out))

    def test_a_missing_layer_fails_the_check(self):
        os.remove(os.path.join(self.out, title.LAYERS[3]))
        self.assertEqual(1, title.check(self.out))

    def test_a_marker_disagreeing_with_the_manifest_fails_the_check(self):
        marker = os.path.join(self.out, "resource", "title_bake.txt")
        with open(marker, "w") as f:
            f.write("TITLE_BAKE_SCALE\n0.123456\n")
        self.assertEqual(1, title.check(self.out))


if __name__ == "__main__":
    unittest.main()
