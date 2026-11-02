#!/usr/bin/env bash
# Konteynerde ASan + UBSan derleme ve testler. Kullanım: scripts/check-sanitize.sh [ctest argümanları]
# NOTEAPP_SANITIZE_BUILD_VOLUME: derleme birimi; yeni ad = sıfırdan derleme.
#
# Çalışma zamanı bastırması yalnız düzeltilebilir (recover) denetimde çalışır, bu yüzden yalnız C kodunda (üçüncü
# parti) yalnız alignment düzeltilebilir; bizim C++ kodumuzda her bulgu derlemeden ölümcüldür. halt_on_error=1 de C'de
# tests/ubsan.supp dışındaki her hizasız okumayı ölümcül yapar.
set -euo pipefail
cd "$(dirname "$0")/.."

source scripts/container-engine.sh

image=noteapp-fedora
volume="${NOTEAPP_SANITIZE_BUILD_VOLUME:-noteapp-sanitize-build}"
"$engine" build -t "$image" -f docker/fedora.Containerfile docker

export MSYS_NO_PATHCONV=1
source_dir="$(pwd -W 2>/dev/null || pwd)"

"$engine" run --rm \
    -v "$source_dir:/src:ro,z" \
    -v "$volume:/build" \
    -e UBSAN_OPTIONS=suppressions=/src/tests/ubsan.supp:halt_on_error=1:print_stacktrace=1 \
    "$image" bash -c 'set -euo pipefail
sanitize="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
cmake --preset linux-gcc-debug -B /build \
    -DCMAKE_CXX_FLAGS="$sanitize" \
    -DCMAKE_C_FLAGS="$sanitize -fsanitize-recover=alignment" \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined
cmake --build /build
ctest --test-dir /build --output-on-failure "$@"' bash "$@"
