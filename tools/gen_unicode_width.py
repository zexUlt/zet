#!/usr/bin/env python3
"""Generate Unicode 16.0.0 width tables for zet_grid.

Reads EastAsianWidth.txt, DerivedCoreProperties.txt, DerivedGeneralCategory.txt,
DerivedAge.txt and emoji-variation-sequences.txt from unicode.org
and writes src/grid/unicode_tables.cpp.

Usage:
    python3 tools/gen_unicode_width.py
    # Downloads from unicode.org; requires network access.
    # Output is already checked into the repo. Re-run only to update to
    # a new Unicode version (update UNICODE_VERSION below).

The tables implement the wcwidth(3) contract used by zet_grid:
    -1   control characters (C0, DEL, C1 except CSI)
     0   non-spacing (Mn, Me) and format (Cf) marks, BOM
     1   most characters
     2   East Asian Wide (W) and Fullwidth (F)

Refs:
    https://www.unicode.org/reports/tr11/   East Asian Width
    https://unicode.org/reports/tr44/       Unicode Character Database
"""

import hashlib
import sys
import urllib.request
from typing import Iterator

UNICODE_VERSION = "16.0.0"
BASE_URL = f"https://www.unicode.org/Public/{UNICODE_VERSION}/ucd/"
EAW_URL = BASE_URL + "EastAsianWidth.txt"
DCP_URL = BASE_URL + "DerivedCoreProperties.txt"
DC_URL  = BASE_URL + "extracted/DerivedGeneralCategory.txt"
EVS_URL = BASE_URL + "emoji/emoji-variation-sequences.txt"
AGE_URL = BASE_URL + "DerivedAge.txt"

# Wide code points added after this Unicode version are "volatile": a terminal
# on an older table draws them in one column. So are the unassigned code points
# of the emoji blocks below: a terminal on a newer table may draw them wide.
VOLATILE_AFTER_AGE = (9, 0)
EMOJI_BLOCKS = [(0x1F000, 0x1FAFF)]

# SHA-256 hashes pinned for Unicode 16.0.0 (computed from the canonical
# unicode.org files; re-verify if UNICODE_VERSION is bumped).
HASHES: dict[str, str] = {
    EAW_URL: "43adc76c0686a42cb370764eb8cfe2b2"
             "a45b10b855e5572a2db4a0eecce15d5b",
    DCP_URL: "39d35161f2954497f69e08bdb9e70149"
             "3f476a3d30222de20028feda36c1dabd",
    DC_URL:  "7676ab755a41ef82108460238569e60a"
             "d65c191ddafe61b36c6765ec1353f293",
    EVS_URL: "71d93ec015011371a027ba2bc0a63155"
             "d381c6e0b94a586c1a88a49400cd6864",
    AGE_URL: "60caa81b99a4dfb0bc5d38440d323073"
             "e3f356f46f9ca7869da848cd0715408b",
}

OUTPUT = "src/grid/unicode_tables.cpp"


def fetch(url: str) -> list[str]:
    print(f"  fetching {url}", file=sys.stderr)
    with urllib.request.urlopen(url, timeout=30) as resp:
        data = resp.read()
    digest = hashlib.sha256(data).hexdigest()
    expected = HASHES.get(url)
    if expected and digest != expected:
        raise ValueError(
            f"SHA-256 mismatch for {url}\n"
            f"  got:      {digest}\n"
            f"  expected: {expected}\n"
            "Update HASHES in gen_unicode_width.py if you intentionally "
            "updated the Unicode version."
        )
    return data.decode("utf-8").splitlines()


def parse_ranges(lines: list[str], filter_fn) -> list[tuple[int, int]]:
    """Return sorted list of (start, end) ranges where filter_fn(fields) is True."""
    ranges: list[tuple[int, int]] = []
    for line in lines:
        line = line.split("#")[0].strip()
        if not line:
            continue
        semi = line.find(";")
        if semi < 0:
            continue
        cp_field = line[:semi].strip()
        rest = line[semi + 1:].strip()
        if ".." in cp_field:
            lo_s, hi_s = cp_field.split("..")
            lo, hi = int(lo_s, 16), int(hi_s, 16)
        else:
            lo = hi = int(cp_field, 16)
        if filter_fn(rest):
            ranges.append((lo, hi))
    return merge(sorted(ranges))


def merge(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    for lo, hi in ranges:
        if out and lo <= out[-1][1] + 1:
            out[-1] = (out[-1][0], max(out[-1][1], hi))
        else:
            out.append((lo, hi))
    return out


def emit_table(name: str, ranges: list[tuple[int, int]]) -> Iterator[str]:
    yield f"static constexpr Range {name}[] = {{"
    for lo, hi in ranges:
        yield f"    {{0x{lo:04X}, 0x{hi:04X}}},"
    yield "};"


def subtract_ranges(a: list[tuple[int, int]],
                    b: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """Remove all code points in b from a."""
    result: list[tuple[int, int]] = []
    bi = 0
    for lo, hi in a:
        while bi < len(b) and b[bi][1] < lo:
            bi += 1
        cur = lo
        i = bi
        while i < len(b) and b[i][0] <= hi:
            if cur < b[i][0]:
                result.append((cur, b[i][0] - 1))
            cur = b[i][1] + 1
            i += 1
        if cur <= hi:
            result.append((cur, hi))
    return result


def main() -> None:
    print("Generating Unicode width tables...", file=sys.stderr)

    eaw_lines = fetch(EAW_URL)
    dcp_lines = fetch(DCP_URL)
    dc_lines = fetch(DC_URL)
    evs_lines = fetch(EVS_URL)
    age_lines = fetch(AGE_URL)

    # Wide: W or F (Fullwidth)
    wide = parse_ranges(eaw_lines, lambda f: f.split()[0] in ("W", "F"))

    # Ambiguous: EAW A. Terminals disagree on their width (1 in most locales,
    # 2 in CJK ones), so the predictive-echo overlay keeps off rows holding
    # one (design.md section 9).
    ambiguous = parse_ranges(eaw_lines, lambda f: f.split()[0] == "A")

    # Zero-width: Default_Ignorable_Code_Point + combining marks (Mn, Me).
    # Subtract EAW Wide characters so that Hangul Fillers (U+115F, U+1160)
    # and other W-class Mn/Me characters are classified as Wide (width 2),
    # not Zero-width (width 0).
    default_ignorable = parse_ranges(
        dcp_lines, lambda f: "Default_Ignorable_Code_Point" in f)
    mn_me = parse_ranges(dc_lines, lambda f: f.split(";")[0].strip() in ("Mn", "Me"))
    zero_width_raw = merge(sorted(default_ignorable + mn_me))
    zero_width = subtract_ranges(zero_width_raw, wide)

    # Bases of emoji variation sequences: a base followed by VS16 (U+FE0F)
    # is drawn as an emoji, which terminals may draw two columns wide.
    vs16_bases = []
    for line in evs_lines:
        fields = line.split("#")[0].split(";")[0].split()
        if len(fields) == 2 and fields[1] == "FE0F":
            vs16_bases.append((int(fields[0], 16),) * 2)
    vs16_bases = merge(sorted(vs16_bases))

    # Version-volatile: wide code points newer than Unicode 9.0, plus the
    # unassigned (Cn) code points of the emoji blocks.
    def newer_than_9(f: str) -> bool:
        age = tuple(int(x) for x in f.split()[0].split("."))
        return age > VOLATILE_AFTER_AGE
    newer = parse_ranges(age_lines, newer_than_9)
    wide_newer = subtract_ranges(wide, subtract_ranges(wide, newer))
    unassigned = parse_ranges(
        dc_lines, lambda f: f.split(";")[0].strip() == "Cn")
    unassigned_emoji = subtract_ranges(
        unassigned, subtract_ranges(unassigned, EMOJI_BLOCKS))
    version_volatile = merge(sorted(wide_newer + unassigned_emoji))

    # Write output
    lines = [
        "// AUTO-GENERATED by tools/gen_unicode_width.py — DO NOT EDIT.",
        f"// Unicode {UNICODE_VERSION}.",
        "// Re-generate: python3 tools/gen_unicode_width.py",
        "",
        "#include \"zet/grid/unicode.hpp\"",
        "",
        "#include <cstddef>",
        "",
        "namespace zet::grid {",
        "",
        "struct Range {",
        "    char32_t Lo;",
        "    char32_t Hi;",
        "};",
        "",
        "// East Asian Width W (Wide) and F (Fullwidth) — width 2.",
        "// Source: EastAsianWidth.txt, Unicode 16.0.0.",
        *emit_table("WIDE", wide),
        "",
        "// East Asian Width A (Ambiguous): width 1 here, 2 on terminals set to",
        "// wide ambiguous. Source: EastAsianWidth.txt, Unicode 16.0.0.",
        *emit_table("AMBIGUOUS", ambiguous),
        "",
        "// Zero-width: combining marks (Mn, Me), Default_Ignorable, etc.",
        "// EAW Wide characters are excluded so Hangul Fillers stay width 2.",
        "// Source: DerivedCoreProperties.txt + DerivedGeneralCategory.txt.",
        *emit_table("ZERO_WIDTH", zero_width),
        "",
        "// Bases of emoji variation sequences with VS16 (U+FE0F).",
        "// Source: emoji-variation-sequences.txt, Unicode 16.0.0.",
        *emit_table("EMOJI_VS16_BASE", vs16_bases),
        "",
        "// Code points whose width depends on the terminal's Unicode version:",
        "// wide ones newer than Unicode 9.0 (older tables draw them narrow) and",
        "// unassigned ones in the emoji blocks (newer tables may draw them wide).",
        "// Source: DerivedAge.txt, EastAsianWidth.txt, DerivedGeneralCategory.txt,",
        f"// Unicode {UNICODE_VERSION}.",
        *emit_table("VERSION_VOLATILE", version_volatile),
        "",
        "// Binary search over a sorted range table.",
        "static bool InRanges(const Range* table, std::size_t n,",
        "                     char32_t cp) noexcept {",
        "    std::size_t lo = 0, hi = n;",
        "    while (lo < hi) {",
        "        const std::size_t mid = lo + (hi - lo) / 2;",
        "        if (cp < table[mid].Lo)      hi = mid;",
        "        else if (cp > table[mid].Hi) lo = mid + 1;",
        "        else return true;",
        "    }",
        "    return false;",
        "}",
        "",
        "bool IsAmbiguousWidth(char32_t cp) noexcept {",
        "    if (cp < 0xA1) return false;",
        "    return InRanges(AMBIGUOUS, sizeof(AMBIGUOUS) / sizeof(*AMBIGUOUS),",
        "                    cp);",
        "}",
        "",
        "bool HasEmojiVariation(char32_t cp) noexcept {",
        "    return InRanges(EMOJI_VS16_BASE,",
        "                    sizeof(EMOJI_VS16_BASE) / sizeof(*EMOJI_VS16_BASE), cp);",
        "}",
        "",
        "bool IsVersionVolatileWidth(char32_t cp) noexcept {",
        "    return InRanges(VERSION_VOLATILE,",
        "                    sizeof(VERSION_VOLATILE) / sizeof(*VERSION_VOLATILE),",
        "                    cp);",
        "}",
        "",
        "bool IsClusterJoiner(char32_t cp) noexcept {",
        "    return cp == 0x200D || cp == 0xFE0E || cp == 0xFE0F ||",
        "           (cp >= 0x1F3FB && cp <= 0x1F3FF);",
        "}",
        "",
        "int Wcwidth(char32_t cp) noexcept {",
        "    // C0 controls and DEL",
        "    if (cp < 0x20 || cp == 0x7F) return -1;",
        "    // C1 controls (8-bit, rare in UTF-8 terminals)",
        "    if (cp >= 0x80 && cp < 0xA0) return -1;",
        "    // Zero-width (combining marks, default-ignorable, etc.)",
        "    if (InRanges(ZERO_WIDTH, sizeof(ZERO_WIDTH)/sizeof(*ZERO_WIDTH), cp))",
        "        return 0;",
        "    // Wide / Fullwidth",
        "    if (InRanges(WIDE, sizeof(WIDE)/sizeof(*WIDE), cp))",
        "        return 2;",
        "    return 1;",
        "}",
        "",
        "}  // namespace zet::grid",
    ]
    with open(OUTPUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Wrote {OUTPUT}", file=sys.stderr)


if __name__ == "__main__":
    main()
