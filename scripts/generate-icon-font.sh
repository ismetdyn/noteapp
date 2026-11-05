#!/usr/bin/env bash
# resources/fonts/NoteappIcons.ttf dosyasını yeniden üretir; normal derlemede gerekmez, ikon tablosu değişince elle koşulur.
# Sürümleri sabit Nerd Fonts arşivinden src/core/file_icon_table.hpp içindeki kod noktalarını ayıklar. Host'ta yalnız
# docker ya da podman gerekir. Lisans ve kaynak: resources/fonts/LICENSE-NoteappIcons.txt.
set -euo pipefail
cd "$(dirname "$0")/.."

source scripts/container-engine.sh

image="docker.io/library/python:3.14-slim"
"$engine" run --rm -i -v "$PWD:/src:z" "$image" bash -euo pipefail -c '
cat >/tmp/requirements.txt <<REQUIREMENTS
fonttools==4.68.0 --hash=sha256:d76f8fdcb6936f5e1045a0c9dd721a4bf11112e60d1e243bfa1d39f06a8b3104
REQUIREMENTS
pip install --quiet --root-user-action=ignore --disable-pip-version-check --only-binary=:all: \
    --require-hashes -r /tmp/requirements.txt
cat >/tmp/generate.py <<GENERATE
import hashlib, io, pathlib, re, subprocess, sys, tarfile, urllib.request
from fontTools.ttLib import TTFont

ARCHIVE_URL = "https://github.com/ryanoasis/nerd-fonts/releases/download/v3.5.1/NerdFontsSymbolsOnly.tar.xz"
ARCHIVE_SHA256 = "01172f37db8543edb102e5cb5c64101c9f4686630804d49b419aa07b23a69996"
MEMBER = "SymbolsNerdFont-Regular.ttf"
OUTPUT = pathlib.Path("/src/resources/fonts/NoteappIcons.ttf")
EXPECTED_GLYPHS = 50

table = pathlib.Path("/src/src/core/file_icon_table.hpp").read_text(encoding="utf-8")
code_points = sorted({int(found, 16) for found in re.findall(r"\b0x([0-9A-F]{4,5})\b", table)})
if len(code_points) != EXPECTED_GLYPHS:
    sys.exit(f"expected {EXPECTED_GLYPHS} code points in the table, found {len(code_points)}")

archive = urllib.request.urlopen(ARCHIVE_URL).read()
if hashlib.sha256(archive).hexdigest() != ARCHIVE_SHA256:
    sys.exit("archive SHA256 mismatch")
with tarfile.open(fileobj=io.BytesIO(archive), mode="r:xz") as tar:
    source = pathlib.Path("/tmp") / MEMBER
    source.write_bytes(tar.extractfile(MEMBER).read())

subprocess.run(["pyftsubset", str(source), "--unicodes=" + ",".join(f"U+{c:X}" for c in code_points),
                "--output-file=" + str(OUTPUT), "--layout-features=", "--no-hinting", "--name-IDs=*",
                "--notdef-outline", "--no-recalc-timestamp"], check=True)
kept = sorted(TTFont(OUTPUT).getBestCmap())
if kept != code_points:
    sys.exit(f"subset cmap differs from the table: {sorted(set(kept) ^ set(code_points))}")
print(f"{OUTPUT.name}: {len(kept)} glyphs, {OUTPUT.stat().st_size} bytes")
GENERATE
python3 -I /tmp/generate.py
'
