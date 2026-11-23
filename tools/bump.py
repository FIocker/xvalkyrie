#!/usr/bin/env python3
"""Bumps the version in CMakeLists.txt, commits it and tags the commit. Nothing is pushed:
the command that does it is printed at the end.

    python tools/bump.py              0.0.3 -> 0.0.4
    python tools/bump.py minor        0.0.3 -> 0.1.0
    python tools/bump.py major        0.0.3 -> 1.0.0
    python tools/bump.py 0.2.0        that version
    python tools/bump.py --dry-run    shows what would be done
"""
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CMAKE = ROOT / "CMakeLists.txt"
PROJECT = re.compile(r"(project\(\s*XValkyrie\s+VERSION\s+)(\d+)\.(\d+)\.(\d+)")
PARTS = ("major", "minor", "patch")


def git(*args, check=True):
    result = subprocess.run(("git", "-C", str(ROOT)) + args, capture_output=True, text=True)
    if check and result.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed:\n{result.stderr.strip()}")
    return result.stdout.strip()


def main():
    parser = argparse.ArgumentParser(description="Bump the version, commit it and tag it.")
    parser.add_argument("version", nargs="?", default="patch", help="major, minor, patch (default) or X.Y.Z")
    parser.add_argument("--dry-run", action="store_true", help="show what would be done and change nothing")
    args = parser.parse_args()

    with open(CMAKE, encoding="utf-8", newline="") as file:
        text = file.read()
    match = PROJECT.search(text)
    if not match:
        sys.exit(f"No project(XValkyrie VERSION X.Y.Z ...) in {CMAKE}")
    current = [int(part) for part in match.group(2, 3, 4)]

    if args.version in PARTS:
        index = PARTS.index(args.version)
        bumped = current[:index] + [current[index] + 1] + [0] * (2 - index)
    elif re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        bumped = [int(part) for part in args.version.split(".")]
        if bumped <= current:
            sys.exit(f"{args.version} is not after the current version, {'.'.join(map(str, current))}")
    else:
        parser.error(f"not major, minor, patch or X.Y.Z: {args.version}")
    old = ".".join(map(str, current))
    new = ".".join(map(str, bumped))

    # tags have no v before them; the release workflow takes either
    for tag in (new, "v" + new):
        if git("tag", "--list", tag):
            sys.exit(f"The tag {tag} already exists")
    if git("status", "--porcelain", "--", "CMakeLists.txt"):
        sys.exit("CMakeLists.txt has changes that are not committed")
    branch = git("branch", "--show-current")
    if not branch:
        sys.exit("Not on a branch")

    print(f"{old} -> {new}")
    if args.dry_run:
        print(f"Would commit CMakeLists.txt on {branch} and tag it {new}")
        return
    with open(CMAKE, "w", encoding="utf-8", newline="") as file:
        file.write(text[:match.start()] + match.group(1) + new + text[match.end():])
    # only this file: what else is staged stays out of the commit
    git("commit", "-m", f"chore: bump version to {new}", "--", "CMakeLists.txt")
    git("tag", new)
    print(f"Committed {git('rev-parse', '--short', 'HEAD')} and tagged it {new}. To publish:")
    print(f"\n    git push --atomic origin {branch} {new}\n")
    print(f"To undo before pushing: git tag -d {new} && git reset --hard HEAD~1")


if __name__ == "__main__":
    main()
