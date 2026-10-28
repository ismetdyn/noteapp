#!/usr/bin/env bash
# Fedora'da derleme + tüm testler (Docker). Kullanım: scripts/check-fedora.sh
# Kaynak salt-okunur bağlanır; derleme dizini ve FetchContent önbelleği adlı bir Docker biriminde kalır.
set -euo pipefail
cd "$(dirname "$0")/.."

image=noteapp-fedora
docker build -t "$image" -f docker/fedora.Containerfile docker

# Git Bash yolları Windows biçimine çevirip docker argümanlarını bozmasın.
export MSYS_NO_PATHCONV=1
source_dir="$(pwd -W 2>/dev/null || pwd)"

docker run --rm \
    -v "$source_dir:/src:ro" \
    -v noteapp-fedora-build:/build \
    -e QT_QPA_PLATFORM=offscreen \
    "$image" bash -c 'cmake --preset linux-gcc-debug -B /build \
        && cmake --build /build \
        && ctest --test-dir /build --output-on-failure'
