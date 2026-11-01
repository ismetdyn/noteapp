#!/usr/bin/env bash
# Tek dosyalık AppImage üretir: out/noteapp-<sürüm>-x86_64.AppImage. Host'ta yalnız docker ya da podman gerekir.
# Kaynak salt-okunur bağlanır; derleme dizini ve FetchContent önbelleği adlı bir konteyner biriminde kalır.
set -euo pipefail
cd "$(dirname "$0")/.."

die() { echo "ERROR: $*" >&2; exit 1; }

source scripts/container-engine.sh

image="${NOTEAPP_APPIMAGE_IMAGE:-noteapp-appimage}"
build_volume="${NOTEAPP_APPIMAGE_VOLUME:-noteapp-appimage-build}"

"$engine" build -t "$image" -f docker/appimage.Containerfile docker

mkdir -p out
# :z SELinux etiketi: Fedora'da etiketsiz bağlanan dizin konteynerde okunamaz.
"$engine" run --rm \
    -v "$PWD:/src:ro,z" \
    -v "$build_volume:/build" \
    -v "$PWD/out:/out:z" \
    "$image" bash /src/packaging/linux/build-in-container.sh

# Konteynerin sıfır çıkışı dosyanın varlığını kanıtlamaz; host'tan görünen sonucu doğrula.
shopt -s nullglob
artifacts=(out/noteapp-*-x86_64.AppImage)
if [[ "${#artifacts[@]}" -ne 1 || ! -x "${artifacts[0]}" ]]; then
    die "expected exactly one executable AppImage in out/, found ${#artifacts[@]}"
fi
echo "AppImage: ${artifacts[0]}"
