#!/usr/bin/env bash
# Fedora'da derleme + tüm testler (Docker, yoksa rootless Podman). Kullanım: scripts/check-fedora.sh
# Kaynak salt-okunur bağlanır; derleme dizini ve FetchContent önbelleği adlı bir konteyner biriminde kalır.
set -euo pipefail
cd "$(dirname "$0")/.."

source scripts/container-engine.sh

image=noteapp-fedora
"$engine" build -t "$image" -f docker/fedora.Containerfile docker

# Git Bash yolları Windows biçimine çevirip konteyner argümanlarını bozmasın.
export MSYS_NO_PATHCONV=1
source_dir="$(pwd -W 2>/dev/null || pwd)"

# :z SELinux etiketi: Fedora'da etiketsiz bağlanan kaynak konteynerde okunamaz.
"$engine" run --rm \
    -v "$source_dir:/src:ro,z" \
    -v noteapp-fedora-build:/build \
    "$image" bash -c 'cmake --preset linux-gcc-debug -B /build \
        && cmake --build /build \
        && ctest --test-dir /build --output-on-failure'
