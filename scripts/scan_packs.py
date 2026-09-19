"""Scan every Crimson Desert pack group's index for entries matching a pattern.

Walks the game folder's numbered pack directories (0000..0040 plus dmmgen and
dmmsa), parses each 0.pamt, and prints every entry whose path contains one of
the search terms. Use it to find which group owns a file before extracting it.

    py -3 scan_packs.py stamina
    py -3 scan_packs.py stamina glide --game "D:/SteamLibrary/steamapps/common/Crimson Desert"
    py -3 scan_packs.py --ext .json --group 0020

Exit code is 1 when nothing matched, so a shell can branch on it.
"""

import argparse
import os
import sys

DEFAULT_GAME = r"D:/SteamLibrary/steamapps/common/Crimson Desert"
UNPACKER = r"C:/working/cd mods/master looter/tools/crimson-desert-unpacker/python"


def load_parser():
    if UNPACKER not in sys.path:
        sys.path.insert(0, UNPACKER)
    try:
        from paz_parse import parse_pamt
    except ImportError as exc:
        sys.exit(f"cannot import paz_parse from {UNPACKER}: {exc}")
    return parse_pamt


def pack_groups(game):
    """Every directory under the game root that holds a 0.pamt."""
    out = []
    for name in sorted(os.listdir(game)):
        d = os.path.join(game, name)
        if os.path.isdir(d) and os.path.isfile(os.path.join(d, "0.pamt")):
            out.append(name)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("terms", nargs="*", help="case-insensitive substrings to match in the entry path")
    ap.add_argument("--game", default=DEFAULT_GAME, help="game root folder")
    ap.add_argument("--group", action="append", help="limit to this pack group (repeatable)")
    ap.add_argument("--ext", action="append", help="limit to this file extension (repeatable)")
    ap.add_argument("--count-only", action="store_true", help="print one total per group, not the entries")
    args = ap.parse_args()

    parse_pamt = load_parser()

    if not os.path.isdir(args.game):
        sys.exit(f"game folder not found: {args.game}")

    groups = args.group or pack_groups(args.game)
    terms = [t.lower() for t in args.terms]
    exts = [e.lower() if e.startswith(".") else "." + e.lower() for e in (args.ext or [])]

    total = 0
    for g in groups:
        pamt = os.path.join(args.game, g, "0.pamt")
        if not os.path.isfile(pamt):
            print(f"[{g}] no 0.pamt", file=sys.stderr)
            continue
        try:
            entries = parse_pamt(pamt, paz_dir=os.path.join(args.game, g))
        except Exception as exc:
            print(f"[{g}] parse failed: {exc}", file=sys.stderr)
            continue

        hits = []
        for e in entries:
            p = e.path.lower()
            if terms and not any(t in p for t in terms):
                continue
            if exts and not any(p.endswith(x) for x in exts):
                continue
            hits.append(e)

        if not hits:
            continue
        total += len(hits)
        print(f"[{g}] {len(hits)} of {len(entries)}")
        if args.count_only:
            continue
        for e in hits:
            print(f"    {e.path}  ({e.orig_size} bytes, comp {e.comp_size}, "
                  f"ctype {e.compression_type}, paz {e.paz_index})")

    print(f"total {total}")
    return 0 if total else 1


if __name__ == "__main__":
    sys.exit(main())
