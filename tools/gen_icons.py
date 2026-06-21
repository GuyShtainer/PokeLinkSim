#!/usr/bin/env python3
"""Convert the Gen-3 sprite-pack box icons into a committed C blob.

For each species PNG (Graphics/Pokemon/Icons/<NAME>.png, a 128x64 sheet = two
64x64 frames), take frame 0, downscale to 16x16, and store each pixel as a u16:
  0x0000          -> transparent
  0x8000 | RGB15  -> opaque
Emit source/mon_icons_data.h (GITIGNORED — never commit/redistribute) with:
  mon_icon_data[N][1024]     the 32x32 icons (static)
  mon_icon_index[412]        internal Gen-3 species id -> row in mon_icon_data
                             (0xFFFF if none)
Run from the repo root:  python3 tools/gen_icons.py

This output is derived from a third-party Gen-3 sprite pack (c) Game Freak / The
Pokemon Company and MUST NOT be committed to the source tree or redistributed as
source. The public REPOSITORY ships NO sprite art (this header is gitignored); the
committed source/mon_icons.c shim returns NULL icons unless this header is present,
so a from-source build uses text fallbacks. The downloadable RELEASE .gba bundles
the generated sprites (built locally with this script), the same way PKHeX ships art.
"""
import os, re, sys, urllib.request
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK = os.path.join(ROOT, "Gen 3 Sprite Pack V1")
ICONS = os.path.join(PACK, "Graphics", "Pokemon", "Icons")
PBS = os.path.join(PACK, "PBS", "pokemon_metrics.txt")
# Art data goes ONLY to a gitignored header (never committed). The committed
# source/mon_icons.{c,h} shim #includes it if present (personal icon build).
OUT_DATA = os.path.join(ROOT, "source", "mon_icons_data.h")

MAX_INTERNAL = 411   # Gen 3 internal species ceiling


def norm(s):
    return re.sub(r"[^A-Z0-9]", "", s.upper())


# True internal Gen-3 species order, derived from the SAME species_names.h the
# name table (gen_names.py) uses, so icons and names share ONE index space (the
# internal id the save stores). The previous hand-rolled "national + offset"
# model was WRONG wherever the real ROM order diverges — e.g. Ralts/Kirlia/
# Gardevoir are internal 392..394, not 305..307 — which mis-mapped many Hoenn
# icons (the "Gardevoir shows as a Taillow" bug). We map sprite -> national (PBS)
# -> internal, where national->internal is built by matching each species_names.h
# entry (its position == the internal id) back to its national number.
_SPECIES_URL = ("https://raw.githubusercontent.com/pret/pokeemerald/master/"
                "src/data/text/species_names.h")
CACHE = os.path.join(os.path.dirname(__file__), "_namecache")


def _clean_name(s):
    """Mirror gen_names.clean() enough to match species names by norm()."""
    s = s.replace("♀", " F").replace("♂", " M")  # Nidoran genders
    s = s.replace("é", "E").replace("É", "E")     # POKE BALL etc.
    s = re.sub(r"\{[^}]*\}", "", s).strip()
    return s


def _load_species_names():
    """Internal-ordered species names from species_names.h (index == internal id)."""
    local = os.path.join(CACHE, "species.h")
    if os.path.exists(local):
        with open(local, encoding="utf-8") as f:
            txt = f.read()
    else:
        os.makedirs(CACHE, exist_ok=True)
        txt = urllib.request.urlopen(_SPECIES_URL, timeout=30).read().decode("utf-8")
        with open(local, "w", encoding="utf-8") as f:
            f.write(txt)
    return [_clean_name(m) for m in re.findall(r'_\("([^"]*)"\)', txt)]


def build_nat_to_internal(name2nat):
    """national-dex no -> true internal id, from the internal-ordered name list."""
    names = _load_species_names()
    m = {}
    for intl, nm in enumerate(names):
        nat = name2nat.get(norm(nm))
        if nat is not None and nat not in m:
            m[nat] = intl
    # Anchors from the (correct) committed name table + real saves. They cover
    # species the OLD linear model got wrong, locking the fix in.
    for nat, intl, who in (
        (252, 277, "Treecko"),  (257, 282, "Blaziken"), (334, 359, "Altaria"),
        (379, 403, "Registeel"),(382, 404, "Kyogre"),   (358, 411, "Chimecho"),
        (276, 304, "Taillow"),  (286, 307, "Breloom"),  (290, 301, "Nincada"),
        (280, 392, "Ralts"),    (281, 393, "Kirlia"),   (282, 394, "Gardevoir"),
    ):
        assert m.get(nat) == intl, \
            "nat %d (%s) -> %r, expected %d" % (nat, who, m.get(nat), intl)
    return m


def parse_pbs():
    name2nat = {}
    cur = None
    with open(PBS, encoding="utf-8-sig") as f:
        for line in f:
            line = line.strip()
            m = re.match(r"^\[(.+)\]$", line)
            if m:
                cur = norm(m.group(1))
                continue
            m = re.match(r"^#(\d+)", line)
            if m and cur:
                name2nat[cur] = int(m.group(1))
                cur = None
    # a few aliases the icon filenames use
    aliases = {"NIDORANFE": 29, "NIDORANF": 29, "NIDORANMA": 32, "NIDORANM": 32}
    for k, v in aliases.items():
        name2nat.setdefault(k, v)
    return name2nat


def rgb15(r, g, b):
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


ICON = 32   # output icon size (px)

def conv_icon(path):
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    fw = w // 2 if w >= 64 else w          # frame 0 = left half of the sheet
    frame = im.crop((0, 0, fw, h))
    frame = frame.resize((ICON, ICON), Image.LANCZOS)
    px = frame.load()
    out = []
    for y in range(ICON):
        for x in range(ICON):
            r, g, b, a = px[x, y]
            out.append(0 if a < 128 else (0x8000 | rgb15(r, g, b)))
    return out


def main():
    name2nat = parse_pbs()
    nat2int = build_nat_to_internal(name2nat)
    index = [0xFFFF] * (MAX_INTERNAL + 1)
    data = []                # list of 256-int icons
    matched, skipped = 0, []

    for fn in sorted(os.listdir(ICONS)):
        if not fn.lower().endswith(".png"):
            continue
        stem = fn[:-4]
        if stem == "000":
            continue
        base = re.sub(r"_\d+$", "", stem)   # drop form variants (DEOXYS_1 ...)
        key = norm(base)
        nat = name2nat.get(key)
        if nat is None:
            skipped.append(stem)
            continue
        intl = nat2int.get(nat)
        if intl is None or intl > MAX_INTERNAL or index[intl] != 0xFFFF:
            continue
        index[intl] = len(data)
        data.append(conv_icon(os.path.join(ICONS, fn)))
        matched += 1

    # --- emit gitignored data header (static arrays, #included by the shim) ---
    with open(OUT_DATA, "w") as d:
        d.write("/* GENERATED by tools/gen_icons.py - DO NOT COMMIT or redistribute.\n")
        d.write("   Derived from a third-party Gen-3 sprite pack (c) Game Freak /\n")
        d.write("   The Pokemon Company. For a personal local build only. */\n")
        d.write("#ifndef MON_ICONS_DATA_H\n#define MON_ICONS_DATA_H\n#include <stdint.h>\n\n")
        d.write("static const uint16_t mon_icon_data[%d][%d] = {\n" % (len(data), ICON * ICON))
        for icon in data:
            d.write("{" + ",".join("0x%04x" % v for v in icon) + "},\n")
        d.write("};\n\n")
        d.write("static const uint16_t mon_icon_index[%d] = {\n" % (MAX_INTERNAL + 1))
        for i in range(0, MAX_INTERNAL + 1, 16):
            d.write(" " + ",".join("0x%04x" % index[j]
                    for j in range(i, min(i + 16, MAX_INTERNAL + 1))) + ",\n")
        d.write("};\n\n#endif\n")

    print("icons matched: %d, data rows: %d" % (matched, len(data)))
    if skipped:
        print("unmatched (%d):" % len(skipped), ", ".join(skipped[:20]),
              "..." if len(skipped) > 20 else "")
    kb = len(data) * ICON * ICON * 2 / 1024
    print("wrote %s (%.1f KiB) - GITIGNORED, do not commit" % (OUT_DATA, kb))


if __name__ == "__main__":
    main()
