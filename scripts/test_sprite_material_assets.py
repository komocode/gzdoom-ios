#!/usr/bin/env python3
"""Regression tests for lossless sprite material authoring I/O (no game assets)."""
import argparse
import io
import json
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from PIL import Image
import sprite_material_assets as assets


def make_wad(entries):
    payload, directory = bytearray(), bytearray()
    for name, value in entries:
        directory.extend(struct.pack("<II8s", 12 + len(payload), len(value), name.encode()))
        payload.extend(value)
    return b"PWAD" + struct.pack("<II", len(entries), 12 + len(payload)) + payload + directory


def make_patch():
    # Two columns, one opaque post each; transparent pixels must stay transparent.
    header = struct.pack("<HHhhII", 2, 3, -4, 7, 16, 22)
    return header + bytes((1, 1, 0, 5, 0, 255, 0, 2, 0, 8, 9, 0, 255))


def png(image):
    stream = io.BytesIO()
    image.save(stream, format="PNG")
    return stream.getvalue()


class SpriteAssetTests(unittest.TestCase):
    def test_patch_offsets_and_coverage(self):
        palette = [(i, 255 - i, 0, 255) for i in range(256)]
        image, offsets = assets.decode_patch(make_patch(), palette)
        self.assertEqual(offsets, (-4, 7))
        self.assertEqual(image.getpixel((0, 0)), (0, 0, 0, 0))
        self.assertEqual(image.getpixel((0, 1)), palette[5])
        self.assertEqual(image.getpixel((1, 1)), palette[9])

    def test_truncated_wad_rejected(self):
        with self.assertRaises(ValueError):
            list(assets.wad_entries(b"PWAD" + struct.pack("<II", 1, 12)))

    def test_grab_offsets(self):
        image = png(Image.new("RGBA", (1, 1)))
        chunk = struct.pack(">I4siiI", 8, b"grAb", -10, 25, 0)
        self.assertEqual(assets.image_offsets(image[:8] + chunk + image[8:]), (-10, 25))

    def test_nested_wad_and_override(self):
        palette = bytes([v for i in range(256) for v in (i, i, i)])
        wad = make_wad([("PLAYPAL", palette), ("S_START", b""), ("TESTA1", make_patch()), ("S_END", b"")])
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr("nested.wad", wad)
            z.writestr("sprites/TESTA1.png", png(Image.new("RGBA", (2, 2), (11, 22, 33, 127))))
        errors = []
        results = list(assets.read_assets(archive.getvalue(), "fixture.pk3", None, errors))
        self.assertEqual(errors, [])
        self.assertEqual([r[0] for r in results], ["TESTA1", "TESTA1"])
        self.assertEqual(results[-1][2].getpixel((0, 0)), (11, 22, 33, 127))

    def test_exact_atlas_roundtrip_and_pack(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "fixture.pk3"
            image = Image.new("RGBA", (3, 4), (37, 91, 150, 127))
            image.putpixel((0, 0), (99, 32, 12, 0))
            with zipfile.ZipFile(source, "w") as z:
                z.writestr("sprites/TESTA1.png", png(image))
            output = root / "extract"
            with patch("builtins.print"):
                assets.extract(argparse.Namespace(inputs=[str(source)], out=str(output), palette_wad=None, atlas_size=128, family=None))
            manifest = json.loads((output / "manifest.json").read_text())
            atlas = manifest["atlases"][0]
            x, y, w, h = atlas["frames"][0]["rect"]
            exported = Image.open(output / atlas["file"]).crop((x, y, x + w, y + h))
            self.assertEqual(image.tobytes(), exported.tobytes())
            authored = root / "authored"
            (authored / "normalmaps").mkdir(parents=True)
            normal = Image.new("RGBA", (128, 128), (128, 128, 255, 73))
            normal.putpixel((x + 1, y), (180, 128, 240, 0))
            normal.save(authored / "normalmaps" / Path(atlas["file"]).name)
            bundle = root / "materials.pk3"
            args = argparse.Namespace(manifest=str(output / "manifest.json"), authored=str(authored), output=str(bundle), channel=["normalmaps"], allow_partial=False)
            with patch("builtins.print"):
                assets.pack(args)
            with zipfile.ZipFile(bundle) as z:
                final = Image.open(io.BytesIO(z.read("materials/normalmaps/auto/TESTA1.png")))
                self.assertEqual(final.size, (3, 4))
                self.assertEqual(final.getpixel((0, 0)), (128, 128, 255, 73))
                self.assertEqual(final.getpixel((1, 0)), (180, 128, 240, 1))
            args.channel = None
            with self.assertRaises(ValueError):
                assets.pack(args)

    def test_wrong_atlas_dimensions_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "normalmaps").mkdir()
            Image.new("RGBA", (10, 10)).save(root / "normalmaps/TEST_000.png")
            manifest = {"frames": [], "atlases": [{"file": "atlases/TEST_000.png", "width": 32, "height": 32, "frames": []}]}
            (root / "manifest.json").write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                assets.pack(argparse.Namespace(manifest=str(root / "manifest.json"), authored=str(root), output=str(root / "out.pk3"), channel=["normalmaps"], allow_partial=False))


if __name__ == "__main__":
    unittest.main()
