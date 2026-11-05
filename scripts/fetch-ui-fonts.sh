#!/usr/bin/env bash
# resources/fonts altındaki Inter ve JetBrains Mono dosyalarını sürümü ve SHA-256'sı sabit resmî sürüm arşivlerinden
# yeniden çıkarır. Normal derlemede gerekmez (dosyalar depodadır); sürüm yükseltilince elle koşulur. Host'ta curl,
# sha256sum ve unzip gerekir. Lisanslar (OFL-1.1): resources/fonts/LICENSE-Inter.txt, LICENSE-JetBrainsMono.txt.
set -euo pipefail
cd "$(dirname "$0")/.."

die() { echo "ERROR: $*" >&2; exit 1; }
for tool in curl sha256sum unzip; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool is required"
done

work="$(mktemp -d)"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

# Arşiv adı, adres, SHA-256, ardından "arşivdeki üye=resources/fonts altındaki ad" çiftleri.
fetch() {
    local name="$1" url="$2" sha256="$3"
    shift 3
    curl --fail --location --silent --show-error --proto '=https' --output "$work/$name" "$url"
    echo "$sha256  $work/$name" | sha256sum --check --quiet - || die "$name SHA-256 mismatch"
    local pair member target
    for pair in "$@"; do
        member="${pair%%=*}"
        target="resources/fonts/${pair#*=}"
        unzip -p "$work/$name" "$member" >"$target"
        [[ -s "$target" ]] || die "$member is missing in $name"
        echo "$target: $(stat -c %s "$target") bytes"
    done
}

fetch Inter-4.1.zip https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip \
    9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e \
    extras/ttf/Inter-Regular.ttf=Inter-Regular.ttf extras/ttf/Inter-Bold.ttf=Inter-Bold.ttf \
    extras/ttf/Inter-Italic.ttf=Inter-Italic.ttf extras/ttf/Inter-BoldItalic.ttf=Inter-BoldItalic.ttf \
    LICENSE.txt=LICENSE-Inter.txt
fetch JetBrainsMono-2.304.zip https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip \
    6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf \
    fonts/ttf/JetBrainsMono-Regular.ttf=JetBrainsMono-Regular.ttf OFL.txt=LICENSE-JetBrainsMono.txt
