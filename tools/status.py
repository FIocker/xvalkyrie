#!/usr/bin/env python3
"""Writes the status report of the project from the tree and the built xvboot.

    python tools/status.py            docs/STATUS.md, docs/img/status.svg, README.md
    python tools/status.py --check    fails if any of them would change

What is counted, and where it comes from:

  console modules   `xvboot modules durango|scarlett`: the import registry of each runtime
  Gaming Runtime    the method tables of src/runtime/modules/gameruntime/slots.cpp against
                    the implement("...") calls of that folder
  shader code       every instruction of the translator's table, encoded and given to
                    `xvboot shader-probe`: handled if the translator has something for it
  roadmap           the check boxes of ROADMAP.md

The README block is what lies between the two `status` comment marks.
"""
import argparse
import collections
import json
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rdna_ops  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
REPORT = ROOT / "docs/STATUS.md"
PICTURE = ROOT / "docs/img/status.svg"
README = ROOT / "README.md"
BEGIN, END = "<!-- status:begin -->", "<!-- status:end -->"
CONSOLES = (("durango", "Xbox One"), ("scarlett", "Xbox Series X|S"))
FORMATS = {
    "sop2": "scalar, two operands", "sopk": "scalar, immediate", "sop1": "scalar, one operand",
    "sopc": "scalar compare", "sopp": "program control", "smem": "scalar memory",
    "vop2": "vector, two operands", "vop1": "vector, one operand", "vopc": "vector compare",
    "vop3": "vector, three operands", "vop3p": "packed vector", "vintrp": "interpolation",
    "ds": "shared memory", "flat": "global memory", "mubuf": "untyped buffer",
    "mtbuf": "typed buffer", "mimg": "image", "exp": "export",
}


def modules(xvboot):
    """Per console: {module: {"symbols": [...], "host": name or None}}."""
    found = {}
    for console, _ in CONSOLES:
        text = subprocess.run([str(xvboot), "modules", console], capture_output=True, text=True,
                              check=True).stdout
        report = json.loads(text)
        entries = {name: {"symbols": sorted(symbols), "host": None}
                   for name, symbols in report["modules"].items()}
        for name, host in report["forwards"].items():
            entries.setdefault(name, {"symbols": [], "host": None})["host"] = host
        found[console] = {"modules": entries, "apis": sorted(report["apis"])}
    return found


def data_symbol(name):
    return name.startswith(("IID_", "WKPDID_", "DXGI_DEBUG_", "CLSID_"))


def gaming_runtime():
    """[(api, [(method, implemented)])] in the order of the tables."""
    folder = ROOT / "src/runtime/modules/gameruntime"
    slots = (folder / "slots.cpp").read_text(encoding="utf-8")
    tables = {name: re.findall(r'\{\s*\d+,\s*"(\w+)"\}', body)
              for name, body in re.findall(r"constexpr Method (\w+)\[\] = \{(.*?)\};", slots, re.S)}
    done = set()
    for source in folder.glob("*.cpp"):
        done.update(re.findall(r'implement\(\s*"(\w+)"', source.read_text(encoding="utf-8")))
    interfaces = slots[slots.index("constexpr Interface interfaces[]"):]
    apis = collections.OrderedDict()
    for api, table in re.findall(r'\{"(\w+)",.*?\n?\s*(\w+_methods)\}', interfaces, re.S):
        names = apis.setdefault(api, [])
        names += [name for name in tables.get(table, []) if name not in names]
    return [(api, [(name, name in done) for name in names]) for api, names in apis.items()]


def probe_words(fmt, op):
    """One encoding of an instruction for `xvboot shader-probe`, which has a resource
    descriptor in s[12:19] and a sampler in s[24:27]."""
    if fmt == "mimg":
        return [0x3C << 26 | (op & 0x7F) << 18 | (op >> 7) | 1 << 8 | 1 << 3, 2 | 4 << 8 | 3 << 16 | 6 << 21]
    if fmt == "mubuf":
        return [0x38 << 26 | op << 18, 1 | 4 << 8 | 3 << 16 | 0x80 << 24]
    if fmt == "smem":
        return [0x3D << 26 | op << 18 | 28 << 6 | (6 if op >= 8 else 2), 0x7D << 25]
    return next(iter(rdna_ops.candidates(fmt, op)))


def instructions(xvboot):
    """[(format, name, handled)] for the translator's table."""
    table = re.findall(r'\{F::(\w+), 0x([0-9a-f]+), "(\w+)"\}', rdna_ops.TABLE.read_text())
    end_program = 0xBF810000
    lines = "".join(" ".join(f"{word:08x}" for word in probe_words(fmt, int(op, 16)) + [end_program]) + "\n"
                    for fmt, op, _ in table)
    stopped = json.loads(subprocess.run([str(xvboot), "shader-probe"], input=lines, capture_output=True,
                                        text=True, check=True).stdout)
    # The translator names the instruction it has nothing for. Anything else that stops a
    # probe is about what the probe lacks (a pixel shader's inputs, an unordered access
    # view), not about the instruction.
    def handled(name, why):
        return not (why == name or why.startswith(("instruction ", "an atomic ", name + " ")) or
                    "reads another lane" in why)
    return [(fmt, name, handled(name, why)) for (fmt, _, name), why in zip(table, stopped)]


def roadmap():
    """[(heading, done, total)] for the sections of ROADMAP.md with check boxes."""
    sections, heading = [], None
    for line in (ROOT / "ROADMAP.md").read_text(encoding="utf-8").splitlines():
        if line.startswith("## "):
            heading = line[3:].strip()
            sections.append([heading, 0, 0])
        elif heading and re.match(r"- \[[ x]\]", line):
            sections[-1][2] += 1
            sections[-1][1] += line.startswith("- [x]")
    return [tuple(section) for section in sections if section[2]]


def bar(done, total, width=20):
    filled = round(width * done / total) if total else 0
    return "█" * filled + "░" * (width - filled)


def percent(done, total):
    return f"{100 * done / total:.0f}%" if total else "-"


def summary(data):
    rows = []
    for console, label in CONSOLES:
        entries = data["modules"][console]["modules"]
        own = {name: entry for name, entry in entries.items() if entry["symbols"]}
        functions = sum(not data_symbol(symbol) for entry in own.values() for symbol in entry["symbols"])
        hosted = sum(1 for entry in entries.values() if entry["host"])
        rows.append((f"Console modules, {label}",
                     f"{len(own)} modules, {functions} functions; {hosted} more answered by the host", ""))
    methods = [done for _, names in data["runtime"] for _, done in names]
    rows.append(("Gaming Runtime", f"{sum(methods)} of {len(methods)} functions",
                 f"`{bar(sum(methods), len(methods))}` {percent(sum(methods), len(methods))}"))
    handled = [done for _, _, done in data["instructions"]]
    rows.append(("Console shader code", f"{sum(handled)} of {len(handled)} instructions",
                 f"`{bar(sum(handled), len(handled))}` {percent(sum(handled), len(handled))}"))
    for heading, done, total in data["roadmap"]:
        rows.append((f"Roadmap: {heading}", f"{done} of {total} items",
                     f"`{bar(done, total)}` {percent(done, total)}"))
    text = "| Area | Covered | |\n|---|---|---|\n"
    return text + "".join(f"| {area.replace('|', '&#124;')} | {covered} | {progress} |\n"
                          for area, covered, progress in rows)


def report(data):
    out = ["# Status\n",
           "Written by `tools/status.py` from the tree and the built `xvboot`; do not edit. What each\n"
           "number counts is at the top of that file.\n",
           "![status map](img/status.svg)\n", summary(data)]
    out.append("## Console modules\n")
    out.append("A module is one a console title imports from. Functions with an implementation here\n"
               "are listed; a module with a host is forwarded to the host's DLL of that name for\n"
               "everything else.\n")
    for console, label in CONSOLES:
        entries = data["modules"][console]["modules"]
        out.append(f"### {label}\n")
        out.append("| Module | Functions | Data | Host |\n|---|---|---|---|\n" + "".join(
            f"| `{name}` | {sum(not data_symbol(s) for s in entry['symbols'])} | "
            f"{sum(data_symbol(s) for s in entry['symbols'])} | {entry['host'] or ''} |\n"
            for name, entry in sorted(entries.items())))
        apis = data["modules"][console]["apis"]
        out.append(f"{len(apis)} functions of the host's API are answered the console's way, whichever "
                   "API set a title imports them from:\n")
        out.append(", ".join(f"`{name}`" for name in apis) + "\n")
        for name, entry in sorted(entries.items()):
            functions = [s for s in entry["symbols"] if not data_symbol(s)]
            if functions:
                out.append(f"<details><summary><code>{name}</code>: {len(functions)} functions</summary>\n\n" +
                           ", ".join(f"`{s}`" for s in functions) + "\n\n</details>\n")
    out.append("## Gaming Runtime\n")
    out.append("The functions of `xgameruntime.dll` whose place in its tables is known. One without an\n"
               "implementation says so in the log when a title calls it, and fails.\n")
    out.append("| API | Implemented | Known | |\n|---|---|---|---|\n" + "".join(
        f"| {api} | {sum(done for _, done in names)} | {len(names)} | "
        f"`{bar(sum(done for _, done in names), len(names), 10)}` |\n" for api, names in data["runtime"]))
    for api, names in data["runtime"]:
        missing = [name for name, done in names if not done]
        if missing:
            out.append(f"<details><summary>{api}: {len(missing)} not implemented</summary>\n\n" +
                       ", ".join(f"`{name}`" for name in missing) + "\n\n</details>\n")
    out.append("## Console shader code\n")
    out.append("Shaders of Series titles are machine code for the console's GPU, translated to HLSL.\n"
               "An instruction counts when the translator has something for it; a shader is only\n"
               "translated when all of its instructions do, and its resources are followed.\n")
    by_format = collections.OrderedDict()
    for fmt, name, done in data["instructions"]:
        by_format.setdefault(fmt, []).append((name, done))
    out.append("| Format | Handled | Known | |\n|---|---|---|---|\n" + "".join(
        f"| {FORMATS.get(fmt, fmt)} | {sum(done for _, done in names)} | {len(names)} | "
        f"`{bar(sum(done for _, done in names), len(names), 10)}` |\n" for fmt, names in by_format.items()))
    for fmt, names in by_format.items():
        missing = [name for name, done in names if not done]
        if missing:
            out.append(f"<details><summary>{FORMATS.get(fmt, fmt)}: {len(missing)} not handled</summary>\n\n" +
                       ", ".join(f"`{name}`" for name in missing) + "\n\n</details>\n")
    return "\n".join(out)


def picture(data):
    """One square for each thing counted, in sections."""
    done, absent, hosted = "#2ea043", "#8b949e", "#388bfd"

    def faded(colour):
        return ' opacity=".35"' if colour == absent else ""

    size, gap, columns, left = 9, 2, 96, 0
    sections = []
    for console, label in CONSOLES:
        squares = []
        for name, entry in sorted(data["modules"][console]["modules"].items()):
            squares += [done] * sum(not data_symbol(s) for s in entry["symbols"])
            squares += [hosted] if entry["host"] and not entry["symbols"] else []
        sections.append((f"Console modules, {label}: {squares.count(done)} functions", squares))
    methods = [flag for _, names in data["runtime"] for _, flag in names]
    sections.append((f"Gaming Runtime: {sum(methods)} of {len(methods)} functions",
                     [done if flag else absent for flag in methods]))
    handled = [flag for _, _, flag in data["instructions"]]
    sections.append((f"Console shader code: {sum(handled)} of {len(handled)} instructions",
                     [done if flag else absent for flag in handled]))
    items = [(heading, [done] * finished + [absent] * (total - finished))
             for heading, finished, total in data["roadmap"]]
    sections.append((f"Roadmap: {sum(s.count(done) for _, s in items)} of {sum(len(s) for _, s in items)} items",
                     [colour for _, squares in items for colour in squares]))
    width = left + columns * (size + gap)
    parts, y = [], 0
    for title, squares in sections:
        y += 16
        parts.append(f'<text x="{left}" y="{y}" class="t">{title.replace("&", "&amp;").replace("|", "/")}</text>')
        y += 8
        for n, colour in enumerate(squares):
            x = left + (n % columns) * (size + gap)
            parts.append(f'<rect x="{x}" y="{y + (n // columns) * (size + gap)}" width="{size}" '
                         f'height="{size}" rx="2" fill="{colour}"{faded(colour)}/>')
        y += ((len(squares) - 1) // columns + 1) * (size + gap) + 8
    legend = [(done, "implemented"), (hosted, "answered by the host"), (absent, "not yet")]
    y += 12
    x = left
    for colour, text in legend:
        parts.append(f'<rect x="{x}" y="{y - 9}" width="{size}" height="{size}" rx="2" fill="{colour}"'
                     f'{faded(colour)}/>')
        parts.append(f'<text x="{x + size + 5}" y="{y}" class="l">{text}</text>')
        x += size + 5 + 7 * len(text) + 18
    y += 8
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{y}" viewBox="0 0 {width} {y}">\n'
            '<style>.t{font:600 12px -apple-system,Segoe UI,Helvetica,Arial,sans-serif;fill:#8b949e}'
            '.l{font:11px -apple-system,Segoe UI,Helvetica,Arial,sans-serif;fill:#8b949e}</style>\n' +
            "\n".join(parts) + "\n</svg>\n")


def readme_block(data):
    return (f"{BEGIN}\n[![status map](docs/img/status.svg)](docs/STATUS.md)\n\n{summary(data)}\n"
            f"Every function and instruction behind these numbers is listed in [docs/STATUS.md](docs/STATUS.md).\n{END}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--xvboot", type=pathlib.Path, default=ROOT / "build/win/Release/xvboot.exe")
    parser.add_argument("--check", action="store_true", help="write nothing; fail if something is out of date")
    arguments = parser.parse_args()
    if not arguments.xvboot.exists():
        sys.exit(f"{arguments.xvboot} is not built: run build.cmd first, or pass --xvboot")
    data = {"modules": modules(arguments.xvboot), "runtime": gaming_runtime(),
            "instructions": instructions(arguments.xvboot), "roadmap": roadmap()}
    readme = README.read_text(encoding="utf-8")
    if BEGIN not in readme or END not in readme:
        sys.exit(f"README.md has no {BEGIN} ... {END} block")
    start, end = readme.index(BEGIN), readme.index(END) + len(END)
    wanted = {REPORT: report(data), PICTURE: picture(data),
              README: readme[:start] + readme_block(data) + readme[end:]}
    stale = [path for path, text in wanted.items()
             if not path.exists() or path.read_text(encoding="utf-8") != text]
    if arguments.check:
        for path in stale:
            print(f"out of date: {path.relative_to(ROOT)}")
        return 1 if stale else 0
    for path in stale:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text := wanted[path], encoding="utf-8", newline="\n")
        print(f"wrote {path.relative_to(ROOT)} ({len(text)} characters)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
