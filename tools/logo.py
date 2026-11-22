#!/usr/bin/env python3
"""Writes docs/img/logo.svg: a valkyrie's helmet with wings of feathers, an angel's on one
side and a demon's on the other, an X across its face, over the name. The letters are drawn
from the grids below; the helmet and the wings are shapes put onto the same grid of pixels.

    python tools/logo.py
"""
import math
import pathlib

# Each letter with the row its first line is on, counted from the top of the capitals.
LETTERS = (
    ("X", 0, """
###...###
###...###
.###.###.
.###.###.
..#####..
...###...
..#####..
.###.###.
.###.###.
###...###
###...###
"""),
    ("V", 0, """
###...###
###...###
###...###
###...###
.###.###.
.###.###.
.###.###.
..#####..
..#####..
...###...
...###...
"""),
    ("a", 4, """
.####.
....##
.#####
##..##
##..##
##..##
.#####
"""),
    ("l", 0, "\n".join(["##"] * 11)),
    ("k", 0, """
##....
##....
##....
##....
##..##
##.##.
####..
###...
####..
##.##.
##..##
"""),
    ("y", 4, """
##..##
##..##
##..##
##..##
##..##
.#####
....##
....##
.####.
"""),
    ("r", 4, """
##.##
#####
###..
##...
##...
##...
##...
"""),
    ("i", 1, """
##
##
..
##
##
##
##
##
##
##
"""),
    ("e", 4, """
.####.
##..##
##..##
######
##....
##...#
.####.
"""),
)
# The feathers of a wing as (degrees above the horizontal, length), each a blade with a
# point, fanning from a stone on the side of the helmet. The first are drawn first, behind.
FEATHERS = ((8, 11), (88, 13), (28, 16), (68, 17), (48, 19))
# top and bottom colours of the letters and the helmet; for a wing, a feather's colour where
# it starts and at its tip, for the long ones and the short
LETTER = ((255, 122, 104), (176, 26, 38))
# a feather's spine, its vane and the shaded side of its vane
ANGEL = ((255, 255, 255), (255, 226, 220), (240, 160, 158))
DEMON = ((232, 70, 70), (92, 18, 34), (48, 10, 22))
EDGE, SHINE, DARK = "#1a090d", "#ffb3a3", "#22090f"


def rows(art):
    return art.strip("\n").split("\n")


def blend(top, bottom, share):
    share = min(max(share, 0), 1)
    return "#" + "".join(f"{round(a + (b - a) * share):02x}" for a, b in zip(top, bottom))


class Canvas:
    def __init__(self, wide, tall):
        self.wide, self.tall = wide, tall
        self.pixels = [[None] * wide for _ in range(tall)]

    def stamp(self, colour_at, edge=EDGE):
        """Fills the pixels `colour_at` gives a colour for, with an outline around them."""
        filled = {}
        for y in range(self.tall):
            for x in range(self.wide):
                colour = colour_at(x + .5, y + .5)
                if colour:
                    filled[x, y] = colour
        for x, y in filled:
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                if edge and (x + dx, y + dy) not in filled and 0 <= x + dx < self.wide and 0 <= y + dy < self.tall:
                    self.pixels[y + dy][x + dx] = edge
        for (x, y), colour in filled.items():
            self.pixels[y][x] = colour


def wing(canvas, root, side, colours):
    """Blades fanning out from `root` on `side` (-1 left), and the stone they start from."""
    for degrees, length in FEATHERS:
        dx, dy = side * math.cos(math.radians(degrees)), -math.sin(math.radians(degrees))

        def colour_at(x, y, dx=dx, dy=dy, length=length):
            along = ((x - root[0]) * dx + (y - root[1]) * dy) / length
            if not 0 < along < 1:
                return None
            across = (x - root[0]) * -dy + (y - root[1]) * dx
            half = 4.2 * along ** .55 * (1 - along) ** .8 + .5
            if abs(across) > half:
                return None
            if abs(across) < .55:
                return blend(colours[0], colours[1], along * .6)
            return blend(colours[1], colours[2], along * .5 + (.5 if across * side > 0 else 0))
        canvas.stamp(colour_at)

    def stone(x, y):
        away = math.hypot(x - root[0], y - root[1])
        if away > 2.6:
            return None
        return "#ffffff" if math.hypot(x - root[0] + .8, y - root[1] + .8) < 1 else blend((255, 120, 150), (190, 30, 70), away / 2.6)
    canvas.stamp(stone)


def helmet(canvas, middle, top):
    """A light open helm: a low cap with the X on it, a thin band along its edge that dips to
    a point over the brow, a slender guard down each side that turns in towards the middle
    without reaching it, and a V between them. Nothing where the face would be."""
    def edge(dx):
        return 10 + max(4.5 - dx * .8, 0) + max(dx - 8, 0) * .5

    def cap(x, y):
        dx, down = abs(x - middle), y - top
        if down <= 10:
            return (dx / 11) ** 2 + ((down - 10) / 9.5) ** 2 <= 1
        return dx <= 11 and down <= edge(dx)

    def near(path, dx, down):
        """How far a point is from a line through `path`, and how far along it."""
        best = (99, 0)
        for n, ((ax, ay), (bx, by)) in enumerate(zip(path, path[1:])):
            along = ((dx - ax) * (bx - ax) + (down - ay) * (by - ay)) / ((bx - ax) ** 2 + (by - ay) ** 2)
            along = min(max(along, 0), 1)
            away = math.hypot(dx - ax - along * (bx - ax), down - ay - along * (by - ay))
            best = min(best, (away, (n + along) / (len(path) - 1)))
        return best

    strap = ((9.8, 10), (9.8, 17), (9, 20), (7.2, 22.3), (4.6, 23.6))
    letter = ((5.6, 15.6), (0, 21.2))

    def guard(x, y):
        away, along = near(strap, abs(x - middle), y - top)
        return away < 1.15 - along * .45

    # the metal in steps from where the light falls, upper left, to the far side; the trim
    # in silver with some blue in it, as a feather has
    metal = ("#ffc2ae", "#ff7c68", "#ec3630", "#c2202c", "#8a1226", "#5c0c20")
    silver = ("#ffffff", "#dcecfa", "#a9c6e4", "#6f8fb8")

    def step(ramp, share):
        return ramp[min(max(int(share * len(ramp)), 0), len(ramp) - 1)]

    def colour_at(x, y):
        dx, down, across = abs(x - middle), y - top, (x - middle) / 11
        if guard(x, y) and not cap(x, y):
            return step(silver, (down - 10) / 16 + (across + 1) * .22)
        if not cap(x, y):
            return None
        if down > edge(dx) - 2.1:
            return step(silver, (across + 1) * .36 + (down - edge(dx) + 2.1) / 6)
        if math.hypot(x - middle + 5.5, down - 4.5) < 1.3:
            return "#fff0e8"
        lit = math.hypot(x - middle + 5.5, down - 4) / 17
        return step(metal, lit + .08)
    canvas.stamp(colour_at)
    canvas.stamp(lambda x, y: step(silver, (x - middle + 6) / 16 + (y - top - 15) / 20)
                 if near(letter, abs(x - middle), y - top)[0] < .8 else None)
    # the X, white with a dark line around it so that it stands off the metal
    def cross(x, y):
        dx, down = abs(x - middle), y - top - 5.4
        return abs(dx - abs(down)) < 1.05 and dx < 3.6 and abs(down) < 3.6
    canvas.stamp(lambda x, y: "#ffffff" if cross(x, y) else None, edge="#4a0a18")


def build():
    name = sum(len(rows(art)[0]) + 1 for _, _, art in LETTERS) - 1
    wide, name_at = 76, 44
    canvas = Canvas(wide, name_at + 13)
    middle, top = wide / 2, 16
    helmet(canvas, middle, top)
    wing(canvas, (middle - 10.5, top + 8.5), -1, ANGEL)
    wing(canvas, (middle + 10.5, top + 8.5), 1, DEMON)
    letters = set()
    x = (wide - name) // 2
    for _, first, art in LETTERS:
        for y, line in enumerate(rows(art)):
            for n, pixel in enumerate(line):
                if pixel == "#":
                    canvas.pixels[name_at + first + y][x + n] = blend(*LETTER, (first + y) / 12)
                    letters.add((x + n, name_at + first + y))
        x += len(rows(art)[0]) + 1
    return canvas.pixels, letters


def svg(canvas, letters, scale=12):
    tall, wide = len(canvas), len(canvas[0])
    out = []
    for y in range(-1, tall + 1):
        for x in range(-1, wide + 1):
            if (x, y) in letters:
                colour = SHINE if (x, y - 1) not in letters else canvas[y][x]
            elif any((x + dx, y + dy) in letters for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                colour = EDGE
            elif 0 <= x < wide and 0 <= y < tall and canvas[y][x]:
                colour = canvas[y][x]
            else:
                continue
            out.append(f'<rect x="{(x + 1) * scale}" y="{(y + 1) * scale}" width="{scale}" height="{scale}" fill="{colour}"/>')
    w, h = (wide + 2) * scale, (tall + 2) * scale
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}" '
            f'shape-rendering="crispEdges" role="img" aria-label="XValkyrie">\n' + "\n".join(out) + "\n</svg>\n")


if __name__ == "__main__":
    target = pathlib.Path(__file__).resolve().parent.parent / "docs/img/logo.svg"
    target.write_text(svg(*build()), encoding="utf-8", newline="\n")
