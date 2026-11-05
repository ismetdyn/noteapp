#!/usr/bin/env python3
# src/app/label_fold.cpp'deki katlama tablolarını md4c kaynağındaki md_get_unicode_fold_info tablolarıyla
# karşılaştırır. md4c sürümü (cmake/Dependencies.cmake) değişince yeniden çalıştırılır; fark varsa tablo yenilenir.
# Kullanım: python3 -I scripts/check-label-fold.py <md4c-kaynak-dizini> [label_fold.cpp]
# md4c kaynağı derleme dizinindedir: <build>/_deps/md4c-src (FetchContent).
import pathlib
import re
import sys

ENTRY = re.compile(r"R\((0x[0-9a-f]+),\s*(0x[0-9a-f]+)\)|S\((0x[0-9a-f]+)\)|(0x[0-9a-f]+)", re.I)
OURS = re.compile(r"(0x[0-9a-f]+)(?:\s*\|\s*(kFrom|kTo))?", re.I)


def md4c_tables(md4c_c):
    body = md4c_c[md4c_c.index("md_get_unicode_fold_info(unsigned codepoint"):]
    tables = {}
    for width in (1, 2, 3):
        for md4c_name, our_name in ((f"FOLD_MAP_{width}", f"kMap{width}"), (f"FOLD_MAP_{width}_DATA", f"kData{width}")):
            match = re.search(r"static const unsigned " + md4c_name + r"\[\] = \{(.*?)\};", body, re.S)
            if match is None:
                sys.exit(f"md4c.c: {md4c_name} not found")
            entries = []
            for token in ENTRY.finditer(match.group(1)):
                if token.group(1):
                    entries += [(int(token.group(1), 16), "kFrom"), (int(token.group(2), 16), "kTo")]
                else:
                    entries.append((int(token.group(3) or token.group(4), 16), None))
            tables[our_name] = entries
    return tables


def our_tables(source):
    tables = {}
    for match in re.finditer(r"constexpr std::uint32_t (k(?:Map|Data)[123])\[\] = \{(.*?)\};", source, re.S):
        tables[match.group(1)] = [(int(t.group(1), 16), t.group(2)) for t in OURS.finditer(match.group(2))]
    return tables


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__ or "usage: check-label-fold.py <md4c-src-dir> [label_fold.cpp]")
    md4c_c = pathlib.Path(sys.argv[1], "src", "md4c.c").read_text(encoding="utf-8")
    default = pathlib.Path(__file__).resolve().parent.parent / "src" / "app" / "label_fold.cpp"
    source = pathlib.Path(sys.argv[2] if len(sys.argv) == 3 else default).read_text(encoding="utf-8")
    expected = md4c_tables(md4c_c)
    actual = our_tables(source)
    failed = False
    for name, entries in expected.items():
        got = actual.get(name)
        if got != entries:
            failed = True
            first = next((i for i, pair in enumerate(zip(entries, got or [])) if pair[0] != pair[1]), None)
            print(f"MISMATCH {name}: md4c {len(entries)} entries, label_fold.cpp {len(got or [])}; "
                  f"first difference at index {first}")
        else:
            print(f"ok {name}: {len(entries)} entries")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
