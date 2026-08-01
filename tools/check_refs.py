#!/usr/bin/env python3
"""Verify that every `path/file.ext:line` reference in docs/ resolves.

Reference checkouts live outside the repo (EternalTerminal, mosh, tmux, ...).
Point at their parent directory:

    ZET_REF_ROOT=/path/to/checkouts tools/check_refs.py

Without it the script reports how many references it would have checked and
exits 0 — a green run must never imply that unchecked references are fine.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

# Requires a directory component and a file extension, so prose like
# "port 2022:" or "RFC 9000:9" cannot match.
REF = re.compile(r"\b([\w.\-]+(?:/[\w.\-]+)+\.[A-Za-z]\w*):(\d+)(?:-(\d+))?\b")

# Leading path component -> reference checkout that owns it.
OWNER = {
    "src": ("ET", "mosh"),  # both use src/; try each
    "proto": ("ET",),
    "test": ("ET",),
    "device": ("wireguard-go",),
    "replay": ("wireguard-go",),
    "tai64n": ("wireguard-go",),
}
# Bare filenames at a checkout root, e.g. tmux.h or master.c.
ROOT_FILES = ("tmux", "dtach", "abduco", "mosh", "ET")


def candidates(rel: str) -> tuple[str, ...]:
    head = rel.split("/", 1)[0]
    return OWNER.get(head, ROOT_FILES)


def main() -> int:
    root = os.environ.get("ZET_REF_ROOT")
    docs = Path("docs")
    if not docs.is_dir():
        print("нет каталога docs/", file=sys.stderr)
        return 2

    refs = [
        (doc, m)
        for doc in sorted(docs.rglob("*.md"))
        for m in REF.finditer(doc.read_text(encoding="utf-8"))
    ]

    if not root:
        print(f"{len(refs)} ссылок найдено, ни одна не проверена (нет $ZET_REF_ROOT)")
        return 0

    base = Path(root)
    if not base.is_dir():
        print(f"$ZET_REF_ROOT={root} не существует", file=sys.stderr)
        return 2

    checked = 0
    unresolved: list[str] = []
    errors: list[str] = []

    for doc, m in refs:
        rel, start = m.group(1), int(m.group(2))
        last = int(m.group(3)) if m.group(3) else start
        for proj in candidates(rel):
            target = base / proj / rel
            if target.is_file():
                total = sum(1 for _ in target.open("rb"))
                if start < 1 or last > total:
                    errors.append(
                        f"{doc.name}: {proj}/{rel}:{m.group(0).split(':')[-1]} "
                        f"вне диапазона (в файле {total} строк)"
                    )
                checked += 1
                break
        else:
            unresolved.append(f"{doc.name}: не нашёл {rel} (ссылка :{start})")

    print(f"ссылок: {len(refs)}, проверено: {checked}, не найдено: {len(unresolved)}")
    for u in unresolved:
        print(f"  skip  {u}")
    for e in errors:
        print(f"  FAIL  {e}")

    if errors:
        print(f"\n{len(errors)} битых ссылок")
        return 1
    print("\nвсе разрешённые ссылки попадают в файлы")
    return 0


if __name__ == "__main__":
    sys.exit(main())
