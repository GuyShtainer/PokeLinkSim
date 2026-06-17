#!/usr/bin/env python3
"""Generate source/data_tables.c (Gen-3 lookup tables BY NUMBER) from the
pokeemerald decomp data files under reference/pokeemerald_data/ (git-ignored,
fetched separately). Matches the API in source/data_tables.h.

Output source/data_tables.c is git-ignored (generate-locally policy). Run from
the repo root:  python3 tools/gen_data.py
"""
import os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RD = os.path.join(ROOT, "reference", "pokeemerald_data")
OUT = os.path.join(ROOT, "source", "data_tables.c")

MAX_SPECIES = 411


def rd(path):
    with open(os.path.join(RD, path), encoding="utf-8", errors="replace") as f:
        return f.read()


def eval_int(expr, env):
    expr = re.sub(r"//.*$", "", expr).strip().rstrip(",").strip()
    try:
        return int(expr, 0)
    except ValueError:
        pass
    # substitute known identifiers, then evaluate a simple arithmetic expression
    def sub(m):
        k = m.group(0)
        return str(env[k]) if k in env else k
    e = re.sub(r"[A-Za-z_]\w*", sub, expr)
    if re.fullmatch(r"[0-9xXa-fA-F+\-*/() ]+", e or ""):
        try:
            return int(eval(e))
        except Exception:
            return None
    return None


def parse_defines(text, prefix):
    out = {}
    for line in text.splitlines():
        m = re.match(r"\s*#define\s+(" + prefix + r"\w+)\s+(.+)$", line)
        if not m:
            continue
        v = eval_int(m.group(2), out)
        if v is not None:
            out[m.group(1)] = v
    return out


def parse_named_array(text, const_map):
    """[CONST] = _("NAME") -> {id: name}."""
    out = {}
    for m in re.finditer(r'\[(\w+)\]\s*=\s*_\("((?:[^"\\]|\\.)*)"\)', text):
        cid = const_map.get(m.group(1))
        if cid is not None:
            out[cid] = m.group(2)
    return out


def iter_blocks(text):
    """Yield (CONST, body) for `[CONST] = { ... }` initializers."""
    for m in re.finditer(r"\[(\w+)\]\s*=\s*\{", text):
        i = m.end()
        depth = 1
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        yield m.group(1), text[m.end():i - 1]


def field(body, name):
    m = re.search(r"\." + name + r"\s*=\s*([^,\n}]+)", body)
    return m.group(1).strip() if m else None


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def prettify(const, *strip):
    s = const
    for p in strip:
        if s.startswith(p):
            s = s[len(p):]
    return s.replace("_", " ")


# ---- constant maps ----
SPEC = parse_defines(rd("include/constants/species.h"), "SPECIES_")
MOVE = parse_defines(rd("include/constants/moves.h"), "MOVE_")
ITEM = parse_defines(rd("include/constants/items.h"), "ITEM_")
ABIL = parse_defines(rd("include/constants/abilities.h"), "ABILITY_")
PKMN = rd("include/constants/pokemon.h")
TYPE = {k: v for k, v in parse_defines(PKMN, "TYPE_").items() if v < 64}  # drop sentinels (TYPE_NONE=255)
GROWTH = parse_defines(PKMN, "GROWTH_")
GENDER = parse_defines(PKMN, "MON_")          # MON_MALE/FEMALE/GENDERLESS
# Contest categories aren't a simple #define list in the decomp; hardcode the
# standard Gen-3 order (COOL, BEAUTY, CUTE, SMART, TOUGH).
CONTEST = {"CONTEST_CATEGORY_COOL": 0, "CONTEST_CATEGORY_BEAUTY": 1,
           "CONTEST_CATEGORY_CUTE": 2, "CONTEST_CATEGORY_SMART": 3,
           "CONTEST_CATEGORY_TOUGH": 4}
MAPSEC = parse_defines(rd("include/constants/region_map_sections.h"), "MAPSEC_")

inv_type = {v: k for k, v in TYPE.items()}
inv_contest = {v: k for k, v in CONTEST.items()}


def gender_val(tok):
    tok = tok.strip()
    if tok in GENDER:
        return GENDER[tok]
    m = re.match(r"PERCENT_FEMALE\(([\d.]+)\)", tok)
    if m:
        return int(float(m.group(1)) * 255 / 100)
    try:
        return int(tok, 0)
    except ValueError:
        return 0


# ---- species data (indexed by internal id) ----
sp_name = parse_named_array(rd("src/data/text/species_names.h"), SPEC)
sp_base = [[0] * 6 for _ in range(MAX_SPECIES + 1)]
sp_t1 = [0] * (MAX_SPECIES + 1)
sp_t2 = [0] * (MAX_SPECIES + 1)
sp_a0 = [0] * (MAX_SPECIES + 1)
sp_a1 = [0] * (MAX_SPECIES + 1)
sp_gr = [0xFF] * (MAX_SPECIES + 1)
sp_growth = [0] * (MAX_SPECIES + 1)
for const, body in iter_blocks(rd("src/data/pokemon/base_stats.h")):
    sid = SPEC.get(const)
    if sid is None or sid > MAX_SPECIES:
        continue
    for i, f in enumerate(["baseHP", "baseAttack", "baseDefense", "baseSpeed", "baseSpAttack", "baseSpDefense"]):
        v = field(body, f)
        sp_base[sid][i] = int(v, 0) if v else 0
    sp_t1[sid] = TYPE.get((field(body, "type1") or "TYPE_NORMAL").strip(), 0)
    sp_t2[sid] = TYPE.get((field(body, "type2") or "TYPE_NORMAL").strip(), 0)
    g = field(body, "genderRatio")
    if g:
        sp_gr[sid] = gender_val(g)
    gr = field(body, "growthRate")
    if gr:
        sp_growth[sid] = GROWTH.get(gr.strip(), 0)
    ab = re.search(r"\.abilities\s*=\s*\{([^}]*)\}", body)
    if ab:
        parts = [p.strip() for p in ab.group(1).split(",")]
        sp_a0[sid] = ABIL.get(parts[0], 0) if len(parts) > 0 else 0
        sp_a1[sid] = ABIL.get(parts[1], 0) if len(parts) > 1 else 0

# ---- internal -> national dex number (exact; the +25 shortcut mismaps the
#      displaced legendaries, e.g. Kyogre 404->382 not 379) ----
PBS_PATH = os.path.join(ROOT, "assets", "sprites", "Gen 3 Sprite Pack V1", "PBS", "pokemon_metrics.txt")
pbs_nat = {}
try:
    cur = None
    with open(PBS_PATH, encoding="utf-8-sig") as pf:
        for line in pf:
            s = line.strip()
            mm = re.match(r"^\[(.+)\]$", s)
            if mm:
                cur = re.sub(r"[^A-Z0-9]", "", mm.group(1).upper())
                continue
            mm = re.match(r"^#(\d+)", s)
            if mm and cur:
                pbs_nat[cur] = int(mm.group(1))
                cur = None
except FileNotFoundError:
    pass
pbs_nat.setdefault("NIDORANF", pbs_nat.get("NIDORANFE", 29))
pbs_nat.setdefault("NIDORANM", pbs_nat.get("NIDORANMA", 32))
sp_national = [0] * (MAX_SPECIES + 1)
for cname, iid in SPEC.items():
    if iid <= MAX_SPECIES:
        key = re.sub(r"[^A-Z0-9]", "", cname[len("SPECIES_"):].upper())
        sp_national[iid] = pbs_nat.get(key, (iid if iid <= 251 else 0))

# ---- moves ----
mv_name = parse_named_array(rd("src/data/text/move_names.h"), MOVE)
NMOVE = (max(MOVE.values()) + 1) if MOVE else 355
mv_type = [0] * NMOVE
mv_pp = [0] * NMOVE
mv_power = [0] * NMOVE
mv_acc = [0] * NMOVE
for const, body in iter_blocks(rd("src/data/battle_moves.h")):
    mid = MOVE.get(const)
    if mid is None or mid >= NMOVE:
        continue
    mv_type[mid] = TYPE.get((field(body, "type") or "TYPE_NORMAL").strip(), 0)
    pp = field(body, "pp")
    mv_pp[mid] = int(pp, 0) if pp else 0
    pw = field(body, "power")
    mv_power[mid] = int(pw, 0) if pw else 0
    ac = field(body, "accuracy")
    mv_acc[mid] = int(ac, 0) if ac else 0
mv_contest = [0] * NMOVE
for const, body in iter_blocks(rd("src/data/contest_moves.h")):
    mid = MOVE.get(const)
    if mid is None or mid >= NMOVE:
        continue
    cc = field(body, "contestCategory")
    if cc:
        mv_contest[mid] = CONTEST.get(cc.strip(), 0)

# ---- move descriptions (text -> var -> move id via the pointer table) ----
mv_desc = {}
_dsrc = rd("src/data/text/move_descriptions.h")
_dtext = {}
for _m in re.finditer(r"static const u8 (\w+)\[\]\s*=\s*_\((.*?)\);", _dsrc, re.S):
    _parts = re.findall(r'"([^"]*)"', _m.group(2))
    _dtext[_m.group(1)] = "".join(_parts).replace("\\n", " ").replace("\\p", " ").replace("\\l", " ").strip()
for _m in re.finditer(r"\[MOVE_(\w+)\s*-\s*1\]\s*=\s*(\w+)", _dsrc):
    _mid = MOVE.get("MOVE_" + _m.group(1))
    if _mid is not None and _mid < NMOVE:
        mv_desc[_mid] = _dtext.get(_m.group(2), "")

# ---- items (names + descriptions) ----
it_name = {}
it_desc = {}
_idsrc = rd("src/data/text/item_descriptions.h")
_idtext = {}
for _m in re.finditer(r"static const u8 (\w+)\[\]\s*=\s*_\((.*?)\);", _idsrc, re.S):
    _parts = re.findall(r'"([^"]*)"', _m.group(2))
    _idtext[_m.group(1)] = "".join(_parts).replace("\\n", " ").replace("\\p", " ").replace("\\l", " ").strip()
for const, body in iter_blocks(rd("src/data/items.h")):
    iid = ITEM.get(const)
    if iid is None:
        continue
    m = re.search(r'\.name\s*=\s*_\("((?:[^"\\]|\\.)*)"\)', body)
    if m:
        it_name[iid] = m.group(1)
    dm = re.search(r"\.description\s*=\s*(\w+)", body)
    if dm and dm.group(1) in _idtext:
        it_desc[iid] = _idtext[dm.group(1)]
NITEM = (max(ITEM.values()) + 1) if ITEM else 400

# ---- abilities (names + descriptions) ----
abtext = rd("src/data/text/abilities.h")
ab_name = parse_named_array(abtext, ABIL)
desc_vars = {m.group(1): m.group(2) for m in
             re.finditer(r'static const u8 (\w+)\[\]\s*=\s*_\("((?:[^"\\]|\\.)*)"\)', abtext)}
ab_desc = {}
for m in re.finditer(r"\[(ABILITY_\w+)\]\s*=\s*(\w+)", abtext):
    aid = ABIL.get(m.group(1))
    if aid is not None and m.group(2) in desc_vars:
        ab_desc[aid] = desc_vars[m.group(2)]
NABIL = (max(ABIL.values()) + 1) if ABIL else 78

# ---- natures ----
nat_names = re.findall(r'_\("((?:[^"\\]|\\.)*)"\)', rd("src/data/text/nature_names.h"))
nat_boost = [-1] * 25
nat_hinder = [-1] * 25
nb = re.search(r"gNatureStatTable.*?\{(.*?)\n\};", rd("src/pokemon.c"), re.S)
if nb:
    rows = re.findall(r"\[NATURE_\w+\]\s*=\s*\{([^}]*)\}", nb.group(1))
    for i, row in enumerate(rows[:25]):
        vals = [int(x) for x in re.findall(r"[+-]?\d+", row)]   # Atk,Def,Spd,SpAtk,SpDef
        for col, v in enumerate(vals[:5]):
            if v > 0:
                nat_boost[i] = col + 1     # -> PK_ATK..PK_SPD (1..5)
            elif v < 0:
                nat_hinder[i] = col + 1

# ---- locations ----
loc_name = {}
for const, v in MAPSEC.items():
    loc_name[v] = prettify(const, "MAPSEC_", "KANTO_") if 0 <= v < 0xF0 else prettify(const, "MAPSEC_")
loc_name[0xFD] = "EGG"
loc_name[0xFE] = "TRADE"
loc_name[0xFF] = "FATEFUL"
NLOC = 256

# ---- experience tables (computed from the Gen-3 formulas) ----
def exp_at(growth, n):
    if n == 0:
        return 0
    if growth == GROWTH["GROWTH_MEDIUM_FAST"]:
        return n**3
    if growth == GROWTH["GROWTH_ERRATIC"]:
        if n <= 50:  return (100 - n) * n**3 // 50
        if n <= 68:  return (150 - n) * n**3 // 100
        if n <= 98:  return ((1911 - 10 * n) // 3) * n**3 // 500
        return (160 - n) * n**3 // 100
    if growth == GROWTH["GROWTH_FLUCTUATING"]:
        if n <= 15:  return ((n + 1) // 3 + 24) * n**3 // 50
        if n <= 36:  return (n + 14) * n**3 // 50
        return ((n // 2) + 32) * n**3 // 50
    if growth == GROWTH["GROWTH_MEDIUM_SLOW"]:
        return 6 * n**3 // 5 - 15 * n**2 + 100 * n - 140
    if growth == GROWTH["GROWTH_FAST"]:
        return 4 * n**3 // 5
    if growth == GROWTH["GROWTH_SLOW"]:
        return 5 * n**3 // 4
    return n**3

NGROWTH = max(GROWTH.values()) + 1
exp_tbl = [[exp_at(g, L) for L in range(101)] for g in range(NGROWTH)]


# ---- emit ----
def emit_strtab(c, name, d, n, default="?"):
    c.write("static const char* const %s[%d] = {\n" % (name, n))
    for i in range(0, n, 4):
        c.write("  " + ",".join(cstr(d.get(j, default)) for j in range(i, min(i + 4, n))) + ",\n")
    c.write("};\n\n")


def emit_u8(c, name, arr):
    c.write("static const uint8_t %s[%d] = {\n" % (name, len(arr)))
    for i in range(0, len(arr), 16):
        c.write("  " + ",".join(str(x) for x in arr[i:i + 16]) + ",\n")
    c.write("};\n\n")


with open(OUT, "w") as c:
    c.write("/* GENERATED by tools/gen_data.py - do not edit. */\n")
    c.write('#include "data_tables.h"\n#include <stddef.h>\n\n')

    SN = MAX_SPECIES + 1
    emit_strtab(c, "s_species", sp_name, SN)
    emit_strtab(c, "s_move", mv_name, NMOVE, "-")
    emit_strtab(c, "s_item", it_name, NITEM, "????????")
    emit_strtab(c, "s_ability", ab_name, NABIL, "-")
    emit_strtab(c, "s_location", loc_name, NLOC, "FARAWAY PLACE")
    # NOTE: item/ability/move in-game DESCRIPTIONS are intentionally NOT emitted
    # (they are verbatim copyrighted game text). The pk_*_desc getters return "".

    c.write("static const char* const s_nature[25] = {\n  ")
    c.write(",".join(cstr(nat_names[i]) if i < len(nat_names) else '"?"' for i in range(25)))
    c.write("\n};\n\n")
    TYPES = max(TYPE.values()) + 1
    c.write("static const char* const s_type[%d] = {\n  " % TYPES)
    c.write(",".join(cstr(prettify(inv_type.get(i, "TYPE_?"), "TYPE_")) for i in range(TYPES)))
    c.write("\n};\n\n")
    CC = max(CONTEST.values()) + 1
    c.write("static const char* const s_contest[%d] = {\n  " % CC)
    c.write(",".join(cstr(prettify(inv_contest.get(i, "CONTEST_CATEGORY_?"), "CONTEST_CATEGORY_")) for i in range(CC)))
    c.write("\n};\n\n")

    # species numeric data
    c.write("static const uint8_t s_base[%d][6] = {\n" % SN)
    for sid in range(SN):
        c.write("  {" + ",".join(str(x) for x in sp_base[sid]) + "},\n")
    c.write("};\n\n")
    emit_u8(c, "s_t1", sp_t1)
    emit_u8(c, "s_t2", sp_t2)
    emit_u8(c, "s_gr", sp_gr)
    emit_u8(c, "s_growth", sp_growth)
    c.write("static const uint16_t s_a0[%d] = {\n" % SN)
    for i in range(0, SN, 16):
        c.write("  " + ",".join(str(x) for x in sp_a0[i:i + 16]) + ",\n")
    c.write("};\n\n")
    c.write("static const uint16_t s_a1[%d] = {\n" % SN)
    for i in range(0, SN, 16):
        c.write("  " + ",".join(str(x) for x in sp_a1[i:i + 16]) + ",\n")
    c.write("};\n\n")
    c.write("static const uint16_t s_national[%d] = {\n" % SN)
    for i in range(0, SN, 16):
        c.write("  " + ",".join(str(x) for x in sp_national[i:i + 16]) + ",\n")
    c.write("};\n\n")

    # move numeric data
    emit_u8(c, "s_mvtype", mv_type)
    emit_u8(c, "s_mvpp", mv_pp)
    emit_u8(c, "s_mvcontest", mv_contest)
    emit_u8(c, "s_mvpower", mv_power)
    emit_u8(c, "s_mvacc", mv_acc)

    # nature mods
    c.write("static const signed char s_natboost[25] = {%s};\n" % ",".join(str(x) for x in nat_boost))
    c.write("static const signed char s_nathinder[25] = {%s};\n\n" % ",".join(str(x) for x in nat_hinder))

    # exp tables
    c.write("static const uint32_t s_exp[%d][101] = {\n" % NGROWTH)
    for g in range(NGROWTH):
        c.write("  {" + ",".join(str(x) for x in exp_tbl[g]) + "},\n")
    c.write("};\n\n")

    # getters
    c.write(f"""
const char* pk_species_name(uint16_t i){{ return i<{SN}?s_species[i]:"?"; }}
uint16_t pk_national_no(uint16_t i){{ return i<{SN}?s_national[i]:0; }}
void pk_base_stats(uint16_t i,uint8_t o[6]){{ for(int k=0;k<6;k++)o[k]=(i<{SN})?s_base[i][k]:0; }}
uint8_t pk_species_type1(uint16_t i){{ return i<{SN}?s_t1[i]:0; }}
uint8_t pk_species_type2(uint16_t i){{ return i<{SN}?s_t2[i]:0; }}
uint16_t pk_species_ability(uint16_t i,uint8_t n){{ if(i>={SN})return 0; return n?s_a1[i]:s_a0[i]; }}
uint8_t pk_species_gender_ratio(uint16_t i){{ return i<{SN}?s_gr[i]:0xFF; }}
uint8_t pk_species_growth(uint16_t i){{ return i<{SN}?s_growth[i]:0; }}
const char* pk_move_name(uint16_t i){{ return i<{NMOVE}?s_move[i]:"-"; }}
uint8_t pk_move_type(uint16_t i){{ return i<{NMOVE}?s_mvtype[i]:0; }}
uint8_t pk_move_pp(uint16_t i){{ return i<{NMOVE}?s_mvpp[i]:0; }}
uint8_t pk_move_contest(uint16_t i){{ return i<{NMOVE}?s_mvcontest[i]:0; }}
uint8_t pk_move_power(uint16_t i){{ return i<{NMOVE}?s_mvpower[i]:0; }}
uint8_t pk_move_accuracy(uint16_t i){{ return i<{NMOVE}?s_mvacc[i]:0; }}
const char* pk_move_desc(uint16_t i){{ (void)i; return ""; }}
const char* pk_item_name(uint16_t i){{ return i<{NITEM}?s_item[i]:"????????"; }}
const char* pk_item_desc(uint16_t i){{ (void)i; return ""; }}
const char* pk_ability_name(uint16_t i){{ return i<{NABIL}?s_ability[i]:"-"; }}
const char* pk_ability_desc(uint16_t i){{ (void)i; return ""; }}
const char* pk_nature_name(uint8_t i){{ return i<25?s_nature[i]:"?"; }}
int pk_nature_boost(uint8_t i){{ return i<25?s_natboost[i]:-1; }}
int pk_nature_hinder(uint8_t i){{ return i<25?s_nathinder[i]:-1; }}
const char* pk_type_name(uint8_t i){{ return i<{TYPES}?s_type[i]:"?"; }}
const char* pk_contest_name(uint8_t i){{ return i<{CC}?s_contest[i]:"?"; }}
const char* pk_location_name(uint16_t i){{ return i<{NLOC}?s_location[i]:"FARAWAY PLACE"; }}
uint32_t pk_exp_for_level(uint8_t g,uint8_t l){{ if(g>={NGROWTH}||l>100)return 0; return s_exp[g][l]; }}
uint8_t pk_level_from_exp(uint8_t g,uint32_t e){{
  int l=1;
  if(g>={NGROWTH})return 1;
  while(l<100&&e>=s_exp[g][l+1])l++;
  return (uint8_t)l;
}}
""")

print("data_tables.c: species=%d names, moves=%d, items=%d, abilities=%d, locations~%d, growth=%d"
      % (len(sp_name), len(mv_name), len(it_name), len(ab_name), len(loc_name), NGROWTH))
print("written:", OUT)
