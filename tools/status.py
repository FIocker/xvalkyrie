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
    """Panels of blocks: a block for each module, API, instruction format and roadmap phase,
    with one square for each thing counted. Green is done, blue is the host's, grey not yet."""
    done, hosted, absent, ink, soft = "#2da44e", "#218bff", "#8c959f", "#8b949e", "#6e7681"
    width, gap, pad, size, step = 920, 14, 16, 9, 11
    parts = []

    def text(x, y, value, kind, anchor="start"):
        value = str(value).replace("&", "&amp;").replace("<", "&lt;")
        parts.append(f'<text x="{x:.0f}" y="{y:.0f}" class="{kind}" text-anchor="{anchor}">{value}</text>')

    def square(x, y, colour):
        faded = ' opacity=".28"' if colour == absent else ""
        parts.append(f'<rect x="{x:.0f}" y="{y:.0f}" width="{size}" height="{size}" rx="2" fill="{colour}"{faded}/>')

    def block_size(name, colours):
        named = int((6.1 * len(name) + 2) // step)
        across = min(len(colours), max(6, round((2.4 * len(colours)) ** 0.5), named))
        rows = (len(colours) - 1) // across + 1
        return across, max(across * step - 2, 6.1 * len(name)), 16 + rows * step

    def blocks(x, y, room, items, in_order=False):
        """Blocks in rows; returns where the last row ends. Unless they come in an order of
        their own, the tallest go first and each goes to the first row with room for it."""
        if not in_order:
            items = sorted(items, key=lambda item: (-block_size(*item)[2], -len(item[1]), item[0]))
        rows = []
        for item in items:
            wide = block_size(*item)[1]
            fits = [row for row in rows if row["used"] + wide <= room]
            if not fits or (in_order and fits[-1] is not rows[-1]):
                rows.append({"used": 0, "items": []})
                fits = rows[-1:]
            row = fits[-1] if in_order else fits[0]
            row["items"].append((row["used"], item))
            row["used"] += wide + 16
        for row in rows:
            for at, (name, colours) in row["items"]:
                across = block_size(name, colours)[0]
                text(x + at, y + 9, name, "n")
                for n, colour in enumerate(colours):
                    square(x + at + step * (n % across), y + 15 + step * (n // across), colour)
            y += max(block_size(*item)[2] for _, item in row["items"]) + 10
        return y - 10

    def frame(x, y, wide, bottom):
        parts.append(f'<rect x="{x + .5}" y="{y + .5}" width="{wide - 1}" height="{bottom - y - 1:.0f}" rx="10" '
                     f'fill="none" stroke="{absent}" stroke-opacity=".35"/>')

    def panel(x, y, wide, title, count, share, fill, framed=True):
        """A panel's head and what `fill` draws inside; returns where the panel ends."""
        text(x + pad, y + 26, title, "h")
        text(x + wide - pad, y + 26, count, "k", "end")
        bottom = fill(x + pad, y + 44, wide - 2 * pad) + pad
        if share is not None:
            parts.append(f'<rect x="{x + pad}" y="{y + 33}" width="{wide - 2 * pad}" height="3" rx="1.5" '
                         f'fill="{absent}" opacity=".28"/>')
            parts.append(f'<rect x="{x + pad}" y="{y + 33}" width="{(wide - 2 * pad) * share:.0f}" height="3" '
                         f'rx="1.5" fill="{done}"/>')
        if framed:
            frame(x, y, wide, bottom)
        return bottom

    def console_modules(console):
        entries = data["modules"][console]["modules"]
        own = [(name, [done] * sum(not data_symbol(s) for s in entry["symbols"]))
               for name, entry in entries.items() if entry["symbols"]]
        host = sorted(name for name, entry in entries.items() if entry["host"])

        def fill(x, y, room):
            y = blocks(x, y, room, [(name, colours) for name, colours in own if colours]) + 14
            at = x
            for name in host:
                wide = size + 5 + 6.1 * len(name)
                if at > x and at + wide > x + room:
                    at, y = x, y + 16
                square(at, y, hosted)
                text(at + size + 5, y + 8, name, "n")
                at += wide + 14
            return y + size
        return sum(len(colours) for _, colours in own), fill

    methods = [flag for _, names in data["runtime"] for _, flag in names]
    handled = [flag for _, _, flag in data["instructions"]]
    phases = data["roadmap"]
    finished, total = sum(d for _, d, _ in phases), sum(t for _, _, t in phases)
    functions = sum(not data_symbol(s) for console, _ in CONSOLES
                    for entry in data["modules"][console]["modules"].values() for s in entry["symbols"])

    # the whole of it in one bar: everything that is counted against a total
    parts_done, parts_all = sum(methods) + sum(handled) + finished, len(methods) + len(handled) + total
    areas = (("Gaming Runtime", sum(methods), len(methods)), ("Shader code", sum(handled), len(handled)),
             ("Roadmap", finished, total))
    text(pad, 30, "XValkyrie", "t")
    text(pad + 104, 30, f"{100 * parts_done / parts_all:.0f}% covered", "h")
    text(width - pad, 30, f"{functions} console functions implemented", "k", "end")
    bar_y, bar_wide, at = 42, width - 2 * pad, pad
    for n, (name, part, whole) in enumerate(areas):
        wide = (bar_wide - 4 * (len(areas) - 1)) * whole / parts_all
        parts.append(f'<rect x="{at:.0f}" y="{bar_y}" width="{wide:.0f}" height="10" rx="3" fill="{absent}" opacity=".28"/>')
        parts.append(f'<rect x="{at:.0f}" y="{bar_y}" width="{wide * part / whole:.0f}" height="10" rx="3" fill="{done}"/>')
        last = n == len(areas) - 1
        text(at + wide if last else at, bar_y + 26, f"{name} {100 * part / whole:.0f}%", "n", "end" if last else "start")
        at += wide + 4
    frame(0, 0, width, 84)
    y = 84 + gap

    half = (width - gap) // 2
    ends = []
    for n, (console, name) in enumerate(CONSOLES):
        count, fill = console_modules(console)
        ends.append(panel(n * (half + gap), y, half, name, f"{count} functions", None, fill, framed=False))
    for n in range(len(CONSOLES)):
        frame(n * (half + gap), y, half, max(ends))
    y = max(ends) + gap

    y = gap + panel(0, y, width, "Gaming Runtime", f"{sum(methods)} / {len(methods)} functions",
                    sum(methods) / len(methods),
                    lambda x, at, room: blocks(x, at, room, [(api, [done if flag else absent for _, flag in names])
                                                             for api, names in data["runtime"]]))
    by_format = collections.OrderedDict()
    for fmt, _, flag in data["instructions"]:
        by_format.setdefault(FORMATS.get(fmt, fmt), []).append(done if flag else absent)
    y = gap + panel(0, y, width, "Console shader code", f"{sum(handled)} / {len(handled)} instructions",
                    sum(handled) / len(handled),
                    lambda x, at, room: blocks(x, at, room, list(by_format.items())))
    y = panel(0, y, width, "Roadmap", f"{finished} / {total} items", finished / total,
              lambda x, at, room: blocks(x, at, room, [(title, [done] * d + [absent] * (t - d))
                                                       for title, d, t in phases], in_order=True))
    y += 22
    x = 2
    for colour, meaning in ((done, "implemented"), (hosted, "answered by the host's DLL"), (absent, "not yet")):
        square(x, y - size, colour)
        text(x + size + 6, y, meaning, "n")
        x += size + 6 + 6.1 * len(meaning) + 22
    y += 6
    style = ("text{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Helvetica,Arial,sans-serif}"
             f".t{{font-size:18px;font-weight:700;fill:{ink}}}.h{{font-size:15px;font-weight:600;fill:{ink}}}.k{{font-size:12px;font-weight:600;fill:{ink}}}"
             f".n{{font-size:11px;fill:{soft}}}")
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{y:.0f}" viewBox="0 0 {width} {y:.0f}" '
            f'role="img" aria-label="XValkyrie status">\n<style>{style}</style>\n' + "\n".join(parts) + "\n</svg>\n")


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
