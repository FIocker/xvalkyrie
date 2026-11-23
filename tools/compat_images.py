#!/usr/bin/env python3
"""Keeps the screenshots of the compatibility table alike.

    python tools/compat_images.py check     fails if a picture is missing or needs fixing
    python tools/compat_images.py fixall    fixes the pictures that need it, in place

The pictures are all the ones in docs/img/compat, plus any that README.md or
COMPATIBILITY.md shows from there and is missing. Each has to be a WebP of 640x360 without
transparency and of at most 100 KiB: a 16:9 frame, small in the repository and large enough
for the 170 pixels it is shown at on a dense display. A picture of another shape is cropped
around its middle, not stretched; other formats (png, jpg, bmp, gif) are converted, the
original is removed and the documents that show it are pointed at the new file. Needs Pillow.
"""
import argparse
import io
import pathlib
import re
import sys

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
FOLDER = ROOT / "docs/img/compat"
DOCUMENTS = (ROOT / "README.md", ROOT / "COMPATIBILITY.md")
SIZE = (640, 360)
MOST_BYTES = 100 * 1024
SUFFIX = ".webp"
OTHER_FORMATS = (".png", ".jpg", ".jpeg", ".bmp", ".gif")
SHOWN = re.compile(r"docs/img/compat/([\w-]+)\.(\w+)")


def expected():
    """Every picture of the folder, whatever its format, and the ones a document shows from
    it that are not there, as the paths of their WebP files."""
    found = {path.with_suffix(SUFFIX) for path in FOLDER.glob("*")
             if path.suffix.lower() in (SUFFIX, *OTHER_FORMATS)}
    for document in DOCUMENTS:
        text = document.read_text(encoding="utf-8") if document.exists() else ""
        found.update(FOLDER / (name + SUFFIX) for name, _ in SHOWN.findall(text))
    return sorted(found)


def source_of(path):
    """The file a picture is to be made from: itself, or one of the same name in another format."""
    if path.exists():
        return path
    for suffix in OTHER_FORMATS:
        if (other := path.with_suffix(suffix)).exists():
            return other
    return None


def stale_references(path):
    """The documents that show the picture under another format's name."""
    stale = []
    for document in DOCUMENTS:
        text = document.read_text(encoding="utf-8") if document.exists() else ""
        if any(name == path.stem and "." + suffix.lower() != SUFFIX
               for name, suffix in SHOWN.findall(text)):
            stale.append(document)
    return stale


def problems(path):
    """What is wrong with a picture, as a list of short texts."""
    source = source_of(path)
    if source is None:
        return ["missing"]
    found = []
    if source != path:
        found.append(f"is {source.suffix}, not {SUFFIX}")
    with Image.open(source) as image:
        if image.format != "WEBP":
            found.append(f"format {image.format}")
        if image.size != SIZE:
            found.append(f"{image.width}x{image.height}, not {SIZE[0]}x{SIZE[1]}")
        if image.mode != "RGB":
            found.append(f"mode {image.mode}")
    if source.stat().st_size > MOST_BYTES:
        found.append(f"{source.stat().st_size // 1024} KiB, more than {MOST_BYTES // 1024}")
    for document in stale_references(path):
        found.append(f"{document.name} shows it under another format")
    return found


def fitted(image):
    """The picture as 640x360 RGB: scaled to cover the frame, the overhang cut evenly."""
    image = image.convert("RGB")
    scale = max(SIZE[0] / image.width, SIZE[1] / image.height)
    width, height = max(round(image.width * scale), SIZE[0]), max(round(image.height * scale), SIZE[1])
    image = image.resize((width, height), Image.LANCZOS)
    left, top = (width - SIZE[0]) // 2, (height - SIZE[1]) // 2
    return image.crop((left, top, left + SIZE[0], top + SIZE[1]))


def encoded(image):
    """WebP bytes within the limit: at a quality nobody tells from the original, else lower."""
    for quality in (88, 80, 70, 60, 50, 40):
        data = io.BytesIO()
        image.save(data, "WEBP", quality=quality, method=6)
        if data.tell() <= MOST_BYTES or quality == 40:
            return data.getvalue()
    raise AssertionError


def fix(path):
    source = source_of(path)
    with Image.open(source) as image:
        image.load()
        result = fitted(image)
    path.write_bytes(encoded(result))
    if source != path:
        source.unlink()
    for document in stale_references(path):
        text = document.read_bytes().decode("utf-8")  # line endings as they are
        text = SHOWN.sub(lambda shown: f"docs/img/compat/{shown[1]}{SUFFIX}"
                         if shown[1] == path.stem else shown[0], text)
        document.write_bytes(text.encode("utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=("check", "fixall"))
    command = parser.parse_args().command
    failed = False
    pictures = expected()
    if not pictures:
        print(f"no pictures in {FOLDER.relative_to(ROOT).as_posix()}")
    for path in pictures:
        name = path.relative_to(ROOT).as_posix()
        found = problems(path)
        if not found:
            print(f"ok       {name}")
        elif command == "check" or found == ["missing"]:
            print(f"{'MISSING' if found == ['missing'] else 'FIX':8} {name}: {', '.join(found)}")
            failed = True
        else:
            fix(path)
            left = problems(path)
            print(f"{'fixed' if not left else 'FIX':8} {name}: {', '.join(found)}")
            failed |= bool(left)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
