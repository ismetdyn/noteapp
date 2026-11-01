#!/usr/bin/env bash
# Gerçek hunspell: Türkçe ve İngilizce sözlükle `hunspell -a` kodlaması ve SpellChecker protokolü (T60). Ana makinede
# paket kurmaz, konteynerde kurar; ağ ister. Kullanım: tests/tui/hunspell_real.sh [imaj]; birim NOTEAPP_E2E_VOLUME.
set -euo pipefail

image="${1:-noteapp-fedora}"
volume="${NOTEAPP_E2E_VOLUME:-noteapp-e2e-debug}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

die() { echo "ERROR: $*" >&2; exit 1; }
command -v podman >/dev/null || die "podman is required"

if ! podman volume exists "$volume"; then
    podman volume create "$volume" >/dev/null
fi
podman run --rm -v "$repo_root:/src:ro,z" -v "$volume:/build:z" -w /src "$image" bash -euc '
    dnf install -y --setopt=install_weak_deps=False hunspell hunspell-tr hunspell-en-US >/dev/null
    hunspell -D 2>&1 | grep -E "/(tr_TR|en_US)$"
    printf "güneş güneşx\n" | hunspell -a -i utf-8 -d tr_TR
    cmake --preset linux-gcc-debug -B /build >/dev/null
    cmake --build /build --target tui_tests >/dev/null
    NOTEAPP_HOME=/tmp/home /build/tests/tui/tui_tests -tc="real hunspell*" --no-skip
'
