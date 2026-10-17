#!/usr/bin/env python3
"""Extract exact sprite pixels/offsets and assemble authored material PK3 files.

No surface shape or materials are inferred. Extraction never changes the input
archives. AI/artist-produced atlas channels must exactly match atlas dimensions.
Requires Pillow. Outputs include copyrighted source pixels: keep them local.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import struct
import zipfile
from collections import Counter, defaultdict
from pathlib import Path

from PIL import Image, ImageDraw

CHANNELS = ("normalmaps", "metallic", "roughness", "ao")
SPRITE_NAME = re.compile(r"^[A-Z0-9_]{4}.[0-8](?:.[0-8])?$", re.I)
IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".pcx", ".dds"}


def wad_entries(data: bytes):
    if len(data) < 12 or data[:4] not in (b"IWAD", b"PWAD"):
        raise ValueError("Not a WAD")
    count, directory = struct.unpack_from("<II", data, 4)
    if directory + count * 16 > len(data):
        raise ValueError("WAD directory is outside archive")
    for i in range(count):
        offset, size, name = struct.unpack_from("<II8s", data, directory + i * 16)
        if offset + size > len(data):
            raise ValueError(f"WAD entry {i} is outside archive")
        yield name.split(b"\0", 1)[0].decode("ascii", "replace"), data[offset:offset + size], i


def palette_from_wad(data: bytes):
    for name, lump, _ in wad_entries(data):
        if name.upper() == "PLAYPAL" and len(lump) >= 768:
            return [tuple(lump[i:i + 3]) + (255,) for i in range(0, 768, 3)]
    return None


def decode_patch(data: bytes, palette):
    if len(data) < 8:
        raise ValueError("Doom patch header is truncated")
    width, height, left, top = struct.unpack_from("<HHhh", data)
    if not (0 < width <= 16384 and 0 < height <= 16384) or 8 + width * 4 > len(data):
        raise ValueError("Invalid Doom patch dimensions")
    result = Image.new("RGBA", (width, height))
    pixels = result.load()
    for x in range(width):
        offset = struct.unpack_from("<I", data, 8 + x * 4)[0]
        previous_top = -1
        while True:
            if offset >= len(data):
                raise ValueError("Doom patch column is truncated")
            top_delta = data[offset]
            if top_delta == 255:
                break
            if offset + 3 > len(data):
                raise ValueError("Doom patch post is truncated")
            length = data[offset + 1]
            if offset + length + 4 > len(data):
                raise ValueError("Doom patch pixels are truncated")
            post_top = previous_top + top_delta if top_delta <= previous_top else top_delta
            previous_top = post_top
            for y in range(length):
                if 0 <= post_top + y < height:
                    pixels[x, post_top + y] = palette[data[offset + 3 + y]]
            offset += length + 4
    return result, (left, top)


def image_offsets(data: bytes):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return (0, 0)
    p = 8
    while p + 12 <= len(data):
        size, kind = struct.unpack_from(">I4s", data, p)
        if p + size + 12 > len(data):
            break
        if kind == b"grAb" and size == 8:
            return struct.unpack_from(">ii", data, p + 8)
        p += size + 12
    return (0, 0)


def read_assets(data: bytes, origin: str, fallback_palette, errors: list, depth=0):
    if depth > 8:
        raise ValueError("Archive nesting exceeds eight levels")
    if data[:4] in (b"IWAD", b"PWAD"):
        palette = palette_from_wad(data) or fallback_palette
        in_sprites = False
        for name, lump, index in wad_entries(data):
            marker = name.upper()
            if marker in ("S_START", "SS_START"):
                in_sprites = True
                continue
            if marker in ("S_END", "SS_END"):
                in_sprites = False
                continue
            if not in_sprites or not lump or not SPRITE_NAME.fullmatch(name):
                continue
            try:
                if lump[:8] == b"\x89PNG\r\n\x1a\n":
                    image = Image.open(io.BytesIO(lump)).convert("RGBA")
                    offsets = image_offsets(lump)
                else:
                    if palette is None:
                        raise ValueError("Paletted sprite needs --palette-wad")
                    image, offsets = decode_patch(lump, palette)
                yield name.upper(), f"{origin}::{index}:{name}", image, offsets
            except Exception as exc:
                errors.append({"source": f"{origin}::{name}", "error": str(exc)})
        return
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for info in archive.infolist():
            name = info.filename.replace("\\", "/")
            suffix = Path(name).suffix.lower()
            if suffix in (".wad", ".pk3", ".zip"):
                yield from read_assets(archive.read(info), f"{origin}::{name}", fallback_palette, errors, depth + 1)
            elif name.lower().startswith("sprites/") and suffix in IMAGE_EXTENSIONS:
                try:
                    raw = archive.read(info)
                    image = Image.open(io.BytesIO(raw)).convert("RGBA")
                    # Short sprite names use engine auto/ lookup. Long-path names
                    # retain the full resource path for materials/<channel>/<path>.
                    stem = Path(name).stem
                    key = stem.upper() if SPRITE_NAME.fullmatch(stem) else str(Path(name).with_suffix(""))
                    yield key, f"{origin}::{name}", image, image_offsets(raw)
                except Exception as exc:
                    errors.append({"source": f"{origin}::{name}", "error": str(exc)})


def extract(args):
    root = Path(args.out)
    (root / "frames").mkdir(parents=True, exist_ok=True)
    (root / "atlases").mkdir(exist_ok=True)
    palette = palette_from_wad(Path(args.palette_wad).read_bytes()) if args.palette_wad else None
    assets, errors, occurrences = {}, [], 0
    for path in args.inputs:
        for key, origin, image, offsets in read_assets(Path(path).read_bytes(), str(Path(path).resolve()), palette, errors):
            occurrences += 1
            assets[key] = (origin, image, offsets)  # later resource overrides earlier one
    records = []
    families = defaultdict(list)
    for name, (origin, image, offsets) in sorted(assets.items()):
        family = Path(name).name[:4].upper()
        if args.family and family not in args.family:
            continue
        identifier = hashlib.sha256(name.encode()).hexdigest()[:16]
        filename = f"frames/{identifier}.png"
        image.save(root / filename)
        record = {"id": identifier, "name": name, "family": family, "source": origin,
                  "width": image.width, "height": image.height, "offsets": list(offsets), "file": filename,
                  "rgba_sha256": hashlib.sha256(image.tobytes()).hexdigest()}
        records.append(record)
        families[family].append((record, image))
    atlases = []
    for family, frames in sorted(families.items()):
        page, x, y, row_h, members = 0, 8, 24, 0, []
        canvas = Image.new("RGBA", (args.atlas_size, args.atlas_size))

        def flush():
            nonlocal page, canvas, x, y, row_h, members
            if not members:
                return
            filename = f"atlases/{family}_{page:03d}.png"
            canvas.save(root / filename)
            atlases.append({"file": filename, "width": canvas.width, "height": canvas.height, "frames": members})
            page += 1
            canvas = Image.new("RGBA", (args.atlas_size, args.atlas_size))
            x, y, row_h, members = 8, 24, 0, []

        for record, image in frames:
            if image.width + 16 > args.atlas_size or image.height + 32 > args.atlas_size:
                raise ValueError(f"{record['name']} exceeds atlas size; increase --atlas-size")
            if x + image.width + 8 > args.atlas_size:
                x, y, row_h = 8, y + row_h + 24, 0
            if y + image.height + 8 > args.atlas_size:
                flush()
            # Paste without an alpha mask: semi-transparent source pixels remain exact.
            canvas.paste(image, (x, y))
            ImageDraw.Draw(canvas).text((x, y - 16), record["name"], fill=(240, 240, 240, 255))
            members.append({"id": record["id"], "rect": [x, y, image.width, image.height]})
            x += max(image.width, len(record["name"]) * 7) + 12
            row_h = max(row_h, image.height)
        flush()
    manifest = {"version": 1, "inputs": [str(Path(p).resolve()) for p in args.inputs],
                "source_occurrences": occurrences, "active_sprite_count": len(records), "family_count": len(families),
                "frames": records, "atlases": atlases, "errors": errors,
                "note": "Exact local source pixels; no material identification or generation performed."}
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({k: manifest[k] for k in ("source_occurrences", "active_sprite_count", "family_count", "errors")}, indent=2))
    if errors:
        raise SystemExit(2)


def pack(args):
    root = Path(args.manifest).parent
    manifest = json.loads(Path(args.manifest).read_text())
    frames = {f["id"]: f for f in manifest["frames"]}
    channels = args.channel or list(CHANNELS)
    outputs = []
    missing = []
    authored_root = Path(args.authored)
    for atlas in manifest["atlases"]:
        for channel in channels:
            source = authored_root / channel / Path(atlas["file"]).name
            if not source.exists():
                missing.append(str(source))
                continue
            material = Image.open(source).convert("RGBA")
            if material.size != (atlas["width"], atlas["height"]):
                raise ValueError(f"{source}: expected exact atlas dimensions {(atlas['width'], atlas['height'])}, got {material.size}")
            for placement in atlas["frames"]:
                frame = frames[placement["id"]]
                x, y, w, h = placement["rect"]
                crop = material.crop((x, y, x + w, y + h))
                # Data is sampled only where the original sprite draws; preserve
                # normal alpha (optional height), use opaque scalar channels.
                if channel != "normalmaps":
                    crop.putalpha(255)
                else:
                    # The engine's shared PNG decoder discards RGB at alpha 0.
                    # Reserve 1/255 as the smallest encoded height so valid
                    # tangent normals survive decoding at zero-height pixels.
                    crop.putalpha(crop.getchannel("A").point(lambda value: max(1, value)))
                lookup = frame["name"] if "/" in frame["name"] else "auto/" + frame["name"]
                buffer = io.BytesIO()
                crop.save(buffer, format="PNG")
                outputs.append((f"materials/{channel}/{lookup}.png", buffer.getvalue()))
    if missing and not args.allow_partial:
        raise ValueError("Missing authored atlas channels:\n" + "\n".join(missing))
    if not outputs:
        raise ValueError("No authored materials found")
    destination = Path(args.output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in outputs:
            archive.writestr(name, data)
        receipt = {"material_images": len(outputs), "source_frames": len(frames), "channels": channels, "missing_atlases": missing}
        archive.writestr("sprite-material-receipt.json", json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    export = sub.add_parser("extract", help="Inventory and losslessly atlas active sprites")
    export.add_argument("inputs", nargs="+", help="WAD/PK3/ZIP inputs in engine load order")
    export.add_argument("--palette-wad", help="IWAD providing PLAYPAL for paletted mod sprites")
    export.add_argument("--out", required=True)
    export.add_argument("--atlas-size", type=int, default=1024)
    export.add_argument("--family", action="append", help="Limit to one or more four-character sprite families")
    export.set_defaults(func=extract)
    bundle = sub.add_parser("pack", help="Crop artist/AI-authored atlases into engine material maps")
    bundle.add_argument("--manifest", required=True)
    bundle.add_argument("--authored", required=True, help="Directory with channel/ATLAS_NAME.png files")
    bundle.add_argument("--output", required=True, help="Destination PK3")
    bundle.add_argument("--channel", action="append", choices=CHANNELS)
    bundle.add_argument("--allow-partial", action="store_true")
    bundle.set_defaults(func=pack)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
