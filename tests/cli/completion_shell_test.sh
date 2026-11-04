#!/usr/bin/env bash
# `note completion zsh|bash|fish`: betikler kabuğun kendi ayrıştırıcısından geçer (-n), bash betiği ve kabuk
# varsa zsh betiği geçici bir depoda gerçekten tamamlar. Kurulu olmayan kabuk açıkça SKIP yazılır.
# Kullanım: completion_shell_test.sh <note ikilisi>
set -euo pipefail

die() { echo "FAIL: $*" >&2; exit 1; }

note_exe="$(realpath -- "$1")"
work="$(mktemp -d)"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

# Betikler `note`u PATH'ten çağırır.
mkdir -p "$work/bin" "$work/home/config"
ln -s "$note_exe" "$work/bin/note"
export PATH="$work/bin:$PATH"
export NOTEAPP_HOME="$work/home"
printf 'language = "en"\nnotes_dir = "%s"\n' "$work/home/notes" >"$work/home/config/config.toml"

for shell in zsh bash fish; do
    note completion "$shell" >"$work/note.$shell" || die "note completion $shell failed"
    if command -v "$shell" >/dev/null; then
        "$shell" -n "$work/note.$shell" || die "$shell -n rejects the generated script"
        echo "ok: $shell -n"
    else
        echo "SKIP: $shell not installed, $shell -n not run"
    fi
done

printf '#proje gündem\n' | note -f iş -n 'plan toplantı' -t md --no-sync >/dev/null
printf '#çalışma\n' | note -n ev -t md --no-sync >/dev/null

# bash: tamamlama işlevi COMP_* değişkenleriyle doğrudan çağrılır. COMP_WORDBREAKS ':' içerir: readline
# `tag:pr`yi `tag` `:` `pr` diye böler.
bash_complete() {
    bash -c '
        source "$1"; shift
        COMP_WORDBREAKS=" :="  # readline varsayılanında da bulunanlar; testte yalnız ":" önemli
        COMP_WORDS=("$@"); COMP_CWORD=$(( ${#COMP_WORDS[@]} - 1 ))
        _note 2>/dev/null
        printf "%s\n" "${COMPREPLY[@]}"
    ' bash "$work/note.bash" "$@"
}
contains() { grep -qxF -- "$2" <<<"$1"; }

out="$(bash_complete note "")"
contains "$out" ls || die "bash: no command list: $out"
contains "$out" completion || die "bash: completion command missing: $out"
if contains "$out" __complete; then die "bash: hidden __complete offered"; fi
out="$(bash_complete note show "")"
contains "$out" "iş/plan toplantı.md" || die "bash: note paths: $out"
out="$(bash_complete note show "iş/p")"
[[ "$out" == "iş/plan toplantı.md" ]] || die "bash: prefix filter: $out"
out="$(bash_complete note ls -f "")"
contains "$out" "iş" || die "bash: folders for -f: $out"
out="$(bash_complete note ls tag : pr)"
[[ "$out" == proje ]] || die "bash: tag value after the colon break: $out"
out="$(bash_complete note view "")"
contains "$out" run || die "bash: view verbs: $out"
out="$(bash_complete note sync init --)"
contains "$out" --replace || die "bash: verb option: $out"
out="$(bash_complete note help "")"
contains "$out" import || die "bash: help operand: $out"
out="$(bash_complete note config get lang)"
[[ "$out" == language ]] || die "bash: config keys: $out"
echo "ok: bash completes"

# zsh: compadd ve _describe yerine adayları yazan taslaklar; tamamlama sistemi yüklenmeden işlev çağrılır.
if command -v zsh >/dev/null; then
    zsh_complete() {
        zsh -f -c '
            compdef() { :; }
            compadd() { while [[ $# -gt 0 && $1 != -- ]]; do shift; done; shift; print -rl -- "$@"; }
            _describe() { print -rl -- "${(@P)${@[-1]}}"; }
            _files() { :; }
            source "$1"; shift
            words=("$@"); CURRENT=$#words; PREFIX=${words[-1]}
            _note
        ' zsh "$work/note.zsh" "$@"
    }
    out="$(zsh_complete note show "")"
    contains "$out" "iş/plan toplantı.md" || die "zsh: note paths: $out"
    out="$(zsh_complete note ls "tag:")"
    contains "$out" "tag:proje" || die "zsh: tag values: $out"
    out="$(zsh_complete note "")"
    contains "$out" "ls:List notes" || die "zsh: described commands: $out"
    out="$(zsh_complete note import -)"
    contains "$out" "--folder:Put the notes in this folder" || die "zsh: described options: $out"
    echo "ok: zsh completes"
else
    echo "SKIP: zsh not installed, zsh behaviour not run"
fi
echo ok
