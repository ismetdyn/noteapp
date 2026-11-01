#!/usr/bin/env bash
# Gerçek terminal e2e (T52): `notes` tmux içinde, podman konteynerinde çalışır; notlar bayt bayt karşılaştırılır (G4'ten beri
# graph.db'de; e2e_note_tool aynı dosyaya ayrı süreçten, dış değişiklik gibi dokunur).
# Kullanım: tests/tui/e2e_editor.sh [imaj]; ana makinede tmux ve podman gerekir.
# Derleme birimi NOTEAPP_E2E_VOLUME'dan gelir; yoksa oluşturulur, `notes` her koşuda bu ağaçtan güncellenir.
set -euo pipefail

image="${1:-noteapp-fedora}"
volume="${NOTEAPP_E2E_VOLUME:-noteapp-e2e-debug}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
name="noteapp-e2e-$$"
session="noteapp-e2e-$$"
work="$(mktemp -d)"
notes="$work/notes"
passed=0
failed=0

die() { echo "ERROR: $*" >&2; exit 1; }
command -v tmux >/dev/null || die "tmux is required"
command -v podman >/dev/null || die "podman is required"

cleanup() {
    tmux kill-session -t "$session" 2>/dev/null || true
    podman rm -f "$name" >/dev/null 2>&1 || true
    podman unshare rm -rf "$work" 2>/dev/null || rm -rf "$work"
}
trap cleanup EXIT

mkdir -p "$notes" "$work/home/config"
printf 'notes_dir = "/data/notes"\nlanguage = "en"\n' >"$work/home/config/config.toml"

# create idempotent değil: var olan birim hata verir.
if ! podman volume exists "$volume"; then
    podman volume create "$volume" >/dev/null
fi
podman run --rm -v "$repo_root:/src:ro,z" -v "$volume:/build:z" "$image" \
    sh -c 'cmake --preset linux-gcc-debug -B /build >/dev/null && cmake --build /build --target notes note e2e_note_tool >/dev/null' ||
    die "building notes failed"

start_app() {
    # --detach-keys boş: podman'ın varsayılanı Ctrl-P'yi yutar, uygulamaya ulaşmaz.
    podman rm -f "$name" >/dev/null 2>&1 || true
    tmux kill-session -t "$session" 2>/dev/null || true
    tmux new-session -d -s "$session" -x 100 -y 30 \
        "podman run --name $name -it --detach-keys= -e TERM=xterm-256color -e NOTEAPP_HOME=/data/home \
         -v $work:/data:z -v $volume:/build:ro,z $image /build/src/tui/notes"
    wait_screen "${1:-${ready_text:-Enter edit}}"
}

screen() { tmux capture-pane -p -t "$session" 2>/dev/null || true; }

wait_screen() {
    local tries=100
    while ! screen | grep -qF -- "$1"; do
        tries=$((tries - 1))
        if [[ $tries -le 0 ]]; then
            screen >&2
            die "screen never showed: $1"
        fi
        sleep 0.1
    done
}

keys() { tmux send-keys -t "$session" "$@"; }
literal() { tmux send-keys -t "$session" -l -- "$1"; sleep 0.3; }

wait_exit() {
    local tries=100
    while [[ "$(podman inspect -f '{{.State.Running}}' "$name" 2>/dev/null)" == "true" ]]; do
        tries=$((tries - 1))
        if [[ $tries -le 0 ]]; then
            die "container did not exit"
        fi
        sleep 0.1
    done
    podman inspect -f '{{.State.ExitCode}}' "$name"
}

note_tool() {
    podman run --rm -i -e NOTEAPP_HOME=/data/home -v "$work:/data:z" -v "$volume:/build:ro,z" "$image" \
        /build/tests/tui/e2e_note_tool "$@"
}
put_note() { note_tool put "$@"; }
clear_notes() { note_tool clear; }

check_bytes() {
    local label="$1" file="${2#"$notes"/}" expected="$3"
    if [[ "$(note_tool cat "$file" | od -An -tx1)" == "$(printf '%b' "$expected" | od -An -tx1)" ]]; then
        passed=$((passed + 1))
        echo "ok   $label"
    else
        failed=$((failed + 1))
        echo "FAIL $label: got $(note_tool cat "$file" | od -c | head -5)"
    fi
}

check() {
    local label="$1"
    shift
    if "$@"; then
        passed=$((passed + 1))
        echo "ok   $label"
    else
        failed=$((failed + 1))
        echo "FAIL $label"
        screen
    fi
}

screen_has() { screen | grep -qF -- "$1"; }
# Renk kaçışlarıyla ekran: mod rengi ve sözdizimi vurgusu gerçek terminalde görünsün.
screen_ansi() { tmux capture-pane -p -e -t "$session" 2>/dev/null || true; }
ansi_has() { screen_ansi | grep -qF -- "$1"; }
clear_prompt() {
    local count
    for ((count = 0; count < $1; count++)); do
        keys BSpace
    done
    sleep 0.2
}
screen_lacks() { ! screen | grep -qF -- "$1"; }
# Tek ekran görüntüsünde: editör açık ve kirli işareti yok. Editör kapalıyken " *" zaten görünmezdi.
editor_clean() {
    local shown
    shown="$(screen)"
    grep -qF -- "Ctrl+S save" <<<"$shown" && ! grep -qF -- " *" <<<"$shown"
}
no_notes() { [[ "$(note_tool count)" == 0 ]]; }
# Liste odağında sol panel kalın çerçevelidir: üst kenarda sağ köşeye dek karakter sayısı = sol panel genişliği
# (kip etiketi kenarın üstünde olduğundan ━ saymak yetmez).
list_width() {
    local LC_ALL=C.UTF-8 top
    top="$(screen | sed -n 2p)"
    top="${top%%┓*}"
    echo $((${#top} + 1))
}
width_is() { [[ "$(list_width)" == "$1" ]]; }
mouse_press() { literal $'\e'"[<$1;$2;$3M"; }
mouse_release() { literal $'\e'"[<$1;$2;$3m"; }

# 1. Editör: Enter, Türkçe yaz, Ctrl+S, SIGKILL, yeniden aç: baytlar diskte.
printf 'ilk' | put_note a.md
start_app
keys Enter
wait_screen "Ctrl+S save"
literal 'ığüşöçİĞ '
keys C-s
wait_screen "Saved"
check_bytes "turkish text saved" "$notes/a.md" 'ığüşöçİĞ ilk'
podman kill --signal KILL "$name" >/dev/null
wait_exit >/dev/null
check_bytes "bytes survive SIGKILL" "$notes/a.md" 'ığüşöçİĞ ilk'
start_app
check "reopened note shows the text" screen_has "ığüşöçİĞ ilk"

# 2. → Önizleme açar, editör açmaz; e editöre girer. Harfler metne gider; Ctrl-C kopyalar, çıkmaz.
check "List focus shows its label" screen_has " LIST "
keys Right
wait_screen "← list"
check "Preview focus shows the REVIEW label" screen_has " REVIEW "
check "the label is green" ansi_has $'\e[32m'
literal 'zzd'
keys z
sleep 0.2
keys e
wait_screen "Ctrl+S save"
check "the editor shows the EDIT label" screen_has " EDIT "
check "EDIT is magenta" ansi_has $'\e[35m'
literal 'dq'
keys C-c
sleep 0.3
check "Ctrl-C does not quit the editor" screen_has "Ctrl+S save"
check_bytes "d and q did not touch the disk" "$notes/a.md" 'ığüşöçİĞ ilk'
check "the status bar shows the position" screen_has "1:3"
check "the dirty mark is shown" screen_has "a.md *"

# 3. Ctrl+Z geri alır, süreci askıya almaz; Esc kaydedilmemişi sorar, Enter (varsayılan Kaydet) yazar, Önizlemeye döner.
keys C-z
sleep 0.3
check "Ctrl+Z undoes instead of suspending" screen_has "1:1"
literal 'yeni '
keys C-q
wait_screen "Unsaved changes"
keys Enter
wait_screen "← list"
check_bytes "default Save at the prompt" "$notes/a.md" 'yeni ığüşöçİĞ ilk'
keys Left
wait_screen "Enter edit"

# 4. Çakışma: dış değişiklikte varsayılan kopya, diskteki sürüm ezilmez.
keys Enter
wait_screen "Ctrl+S save"
literal 'x'
printf 'dis' | put_note a.md
keys C-s
wait_screen "Changed on disk"
keys Enter
wait_screen "Saved as copy"
check_bytes "conflict keeps the disk version" "$notes/a.md" 'dis'
check_bytes "conflict writes the copy" "$notes/a (copy).md" 'xyeni ığüşöçİĞ ilk'
keys C-q
wait_screen "← list"

# 5. CRLF + BOM + kontrol baytı düzenleme sonrası korunur; Shift+ok ve Delete gerçek diziyle çalışır.
clear_notes
printf '\xef\xbb\xbfbir\r\n\x1b[31mX\r\nson' | put_note c.md
start_app
keys Enter
wait_screen "Ctrl+S save"
literal $'\e[1;2C'
keys DC
keys C-s
wait_screen "Saved"
check_bytes "crlf, bom and control bytes preserved" "$notes/c.md" '\xef\xbb\xbfir\r\n\x1b[31mX\r\nson'
check "no raw escape effect on screen" screen_has "[31mX"
keys C-q
wait_screen "← list"
keys Escape
wait_screen "Enter edit"
keys q
check "q quits from the list with status 0" test "$(wait_exit)" = 0

# 6. n: boş editör tüm terminali alır, ad ilk Ctrl+S'te sorulur; kayıttan önce dosya yok.
clear_notes
start_app
keys n
wait_screen "New note"
check "n hides the top line" screen_lacks "[name]"
check "n shows the EDIT label" screen_has " EDIT "
keys C-q
wait_screen "Enter edit"
check "Esc on an empty draft closes silently" no_notes
keys n
wait_screen "New note"
literal 'dq şöyle'
check "typing creates no file" no_notes
check "the dirty mark is on the frame" screen_has "EDIT *"
keys C-s
wait_screen "Save as:"
check "the name prompt is prefilled from the first line" screen_has "dq-soyle.md"
keys Escape
wait_screen "Ctrl+S save"
check "Esc at the name prompt writes nothing" no_notes
keys C-s
wait_screen "Save as:"
clear_prompt 12
literal 'İş/Toplantı Notu'
keys Enter
wait_screen "Saved"
check_bytes "n: folder, slug and default type from detect" "$notes/İş/toplanti-notu.md" 'dq şöyle'
check "Enter saves and stays in the editor" screen_has "New note"
keys C-q
wait_screen "Enter edit"
check "closing returns to two panes with the note selected" screen_has "▸ İş/toplanti-notu.md"
check "the list label is back" screen_has " LIST "

# 6b. env yapıştırma: tuş olayı olarak gelir, ad isteminde .env görünür, değer ada girmez.
clear_notes
start_app
keys n
wait_screen "New note"
literal $'API_URL=https://gizli\rDB_PORT=5432\r'
keys C-s
wait_screen "Save as:"
check "pasted env content suggests .env in the name prompt" screen_has "api-url.env"
keys Enter
wait_screen "Saved"
check_bytes "the env note is saved under the suggested name" "$notes/api-url.env" 'API_URL=https://gizli\nDB_PORT=5432\n'
keys C-q
wait_screen "Enter edit"
check "one note only" test "$(note_tool count)" = 1

# 7. /: girdi altta, üst satırda yalnız mod ve sayaç; Enter seçimi tutar; eşleşme yoksa sorgu taslağın ilk satırı olur.
keys /
wait_screen "Enter keep"
literal 'qqzz'
check "the query is on the bottom line" test "$(screen | tail -1 | cut -c1-6)" = "> qqzz"
check "the top line has no query" screen_lacks "> qqzz  [name]"
keys Enter
wait_screen "Ctrl+S save"
check "nvALT: query is the first line of the draft" screen_has "qqzz"
check "nvALT draft is dirty" screen_has "EDIT *"
keys C-q
wait_screen "Unsaved changes"
keys Right
keys Enter
wait_screen "Enter edit"
check "discarding the draft leaves no note" test "$(note_tool count)" = 1
keys Escape
sleep 0.2

# 8. Önizlemede / : Türkçe katlamalı vurgu, sayaç, n/N.
clear_notes
printf 'Işık ılık\nikinci IŞIK\nson ışık\n' | put_note f.md
start_app
keys Right
wait_screen "← list"
keys /
wait_screen "Find:"
literal 'ışık'
wait_screen "1/3"
keys Enter
wait_screen "1/3"
keys n
wait_screen "2/3"
keys N
wait_screen "1/3"
keys Escape
sleep 0.2
check "Esc clears the find counter" screen_lacks "1/3"

# 9. Editörde Ctrl+F: aynı girdi, Enter ve F3 sonraki; harfler metne gitmez.
keys e
wait_screen "Ctrl+S save"
keys C-f
wait_screen "Find:"
literal 'ışık'
wait_screen "1/3"
keys Enter
wait_screen "2/3"
keys F3
wait_screen "3/3"
keys C-p
wait_screen "2/3"
keys Escape
sleep 0.2
check "find letters did not dirty the note" editor_clean
keys C-q
wait_screen "← list"

# 10. Pano ve yerleşim: < > z, fare ile bölücü sürükleme, tekerlek, satır tıklama.
keys Left
wait_screen "Enter edit"
check "the default list width" width_is 40
keys '>'
sleep 0.3
check "> widens by 5" width_is 45
keys '<'
keys '<'
sleep 0.3
check "< narrows by 5" width_is 35
keys z
sleep 0.3
check "z hides the right pane" screen_lacks "┌"
keys z
sleep 0.3
check "z again brings it back" width_is 35
mouse_press 0 36 5
mouse_press 32 51 5
mouse_release 0 51 5
check "mouse drag moves the divider" width_is 51
# Release kayboldu: tuşsuz hareket (35) sürüklemeyi bitirir, bölücüyü taşımaz.
mouse_press 0 51 5
mouse_press 35 30 5
check "a move with no button ends a drag whose release was lost" width_is 51
mouse_press 0 80 5
mouse_release 0 80 5
check "a click on the right pane focuses Preview" screen_has "← list"
keys Left
wait_screen "Enter edit"
printf 'b\n' | put_note g.md
sleep 6
wait_screen "g.md"
mouse_press 0 10 4
mouse_release 0 10 4
sleep 0.2
check "a click selects the second row" screen_has "▸ f.md"
mouse_press 64 5 5
sleep 0.2
check "wheel up over the list moves the selection" screen_has "▸ g.md"

# 10b. Dış değişiklik tuşa basmadan ekrana gelir: temiz tampon yenilenir, kirli tampona dokunulmaz.
keys Enter
wait_screen "Ctrl+S save"
printf 'dis yeni\n' | put_note g.md
sleep 6
wait_screen "dis yeni"
literal 'k'
printf 'dis ikinci\n' | put_note g.md
sleep 6
wait_screen "Changed on disk"
check "a dirty buffer is kept on rescan" screen_has "kdis yeni"
keys C-q
wait_screen "Unsaved changes"
keys Right
keys Enter
wait_screen "← list"
check_bytes "discard keeps the external version" "$notes/g.md" 'dis ikinci\n'
keys Left
wait_screen "Enter edit"

# 11. SIGTERM ve SIGHUP: kirli metin normal yoldan yazılır, süreç 128+N ile biter; taslak da düşmez.
for signal in TERM:143 HUP:129; do
    clear_notes
    printf 'taban' | put_note s.md
    start_app
    keys Enter
    wait_screen "Ctrl+S save"
    literal 'kirli '
    podman kill --signal "${signal%%:*}" "$name" >/dev/null
    code="$(wait_exit)"
    check_bytes "SIG${signal%%:*} saves the dirty buffer" "$notes/s.md" 'kirli taban'
    check "SIG${signal%%:*} exit status" test "$code" = "${signal##*:}"
done
clear_notes
start_app
keys n
wait_screen "New note"
literal 'kaybolmasın'
podman kill --signal TERM "$name" >/dev/null
wait_exit >/dev/null
check_bytes "SIGTERM saves an unsaved draft" "$notes/kaybolmasin.md" 'kaybolmasın'

# 12. Temiz notta sinyal yazmaz.
clear_notes
printf 'temiz' | put_note t.md
revision_before="$(note_tool revision t.md)"
start_app
keys Enter
wait_screen "Ctrl+S save"
podman kill --signal TERM "$name" >/dev/null
wait_exit >/dev/null
check "clean buffer is not rewritten on SIGTERM" test "$(note_tool revision t.md)" = "$revision_before"

# 13. Çıkış tuşları. Ctrl+Q (XON) ve Ctrl+S (XOFF) gerçek pty'de uygulamaya ulaşmalı: IXON açıkken başlatılır,
# FTXUI oturumda kapatır, normal çıkışta ve sinyalde geri açılmalıdır.
tty_of_notes() {
    podman exec "$name" sh -c 'for p in /proc/[0-9]*; do
        if [ "$(cat "$p/comm" 2>/dev/null)" = notes ]; then readlink "$p/fd/0"; exit 0; fi
    done; exit 1'
}
signal_notes() {
    podman exec "$name" sh -c 'for p in /proc/[0-9]*; do
        if [ "$(cat "$p/comm" 2>/dev/null)" = notes ]; then kill -'"$1"' "${p#/proc/}"; exit 0; fi
    done; exit 1'
}
ixon_off_in_session() { podman exec "$name" stty -F "$(tty_of_notes)" -a | grep -q -- '-ixon'; }
start_app_with_stty() {
    rm -f "$work/stty.before" "$work/stty.after"
    podman rm -f "$name" >/dev/null 2>&1 || true
    tmux kill-session -t "$session" 2>/dev/null || true
    tmux new-session -d -s "$session" -x 100 -y 30 \
        "podman run --name $name -it --detach-keys= -e TERM=xterm-256color -e NOTEAPP_HOME=/data/home \
         -v $work:/data:z -v $volume:/build:ro,z $image \
         sh -c 'stty ixon; stty -a >/data/stty.before; /build/src/tui/notes; stty -a >/data/stty.after'"
    wait_screen "Enter edit"
}
ixon_on() { grep -qE '(^|[ ;])ixon' "$1"; }

clear_notes
printf 'taban' | put_note q.md
start_app_with_stty
check "the tty starts with IXON on" ixon_on "$work/stty.before"
check "IXON is off during the session" ixon_off_in_session
keys Escape
sleep 0.3
check "Esc on an empty filter does not quit the list" screen_has "Enter edit"
keys Enter
wait_screen "Ctrl+S save"
literal 'x'
keys C-s
wait_screen "Saved"
check_bytes "Ctrl+S reaches the app with IXON on at start" "$notes/q.md" 'xtaban'
keys Escape
sleep 0.3
check "Esc alone stays in the editor" screen_has "Ctrl+S save"
literal 'y'
literal 'q'
check "q types in the editor" screen_has "xyqtaban"
keys C-q
wait_screen "Unsaved changes"
check "Ctrl+Q reaches the app and asks about unsaved text" screen_has "Unsaved changes"
keys Escape
sleep 0.3
check "Esc at the prompt cancels" screen_has "Ctrl+S save"
keys C-z
sleep 0.3
keys C-s
sleep 0.2
keys C-q
wait_screen "← list"
check "Ctrl+Q on a clean note goes to REVIEW" screen_has " REVIEW "
keys q
wait_exit >/dev/null
check "q quits from REVIEW" test -s "$work/stty.after"
check "IXON is restored after a normal exit" ixon_on "$work/stty.after"

start_app_with_stty
keys Enter
wait_screen "Ctrl+S save"
signal_notes TERM
wait_exit >/dev/null
check "IXON is restored after SIGTERM" ixon_on "$work/stty.after"

# 14. Düzenleme görünümü: numara sütunu, gerçek terminal imleci, durum çubuğu, `~`; REVIEW `cat` gibi kalır.
cursor_at() { [[ "$(tmux display-message -p -t "$session" '#{cursor_x},#{cursor_y},#{cursor_flag}')" == "$1,$2,1" ]]; }
clear_notes
printf 'birinci\nikinci\n' | put_note v.md
start_app
keys Enter
wait_screen "Ctrl+S save"
# Sol panel 40, kenar 1, numara sütunu 3: metnin ilk hücresi 44, ilk satır 2.
check "the terminal cursor is at the first text cell after the gutter" cursor_at 44 2
keys Down
keys End
sleep 0.3
check "the terminal cursor follows after motion" cursor_at 50 3
check "the gutter shows the line number" screen_has " 2 ikinci"
check "rows past the end show a tilde" screen_has "~"
check "the status bar names the file and format" screen_has "UTF-8  LF"
mouse_press 0 47 3
mouse_release 0 47 3
sleep 0.3
check "a click lands on the text cell, the gutter is not text" cursor_at 46 2
keys C-q
wait_screen "← list"
check "REVIEW has no gutter" screen_has "┃birinci"
check "REVIEW has no tilde" screen_lacks "~"
check "REVIEW hides the terminal cursor" test "$(tmux display-message -p -t "$session" '#{cursor_flag}')" = 0
keys q
wait_exit >/dev/null

# 15. Sözdizimi vurgusu ve anlık görüntüler: java, env, py için REVIEW ve EDIT.
snapshot_dir="${NOTEAPP_E2E_SNAPSHOT_DIR:-}"
snapshot() {
    if [[ -n "$snapshot_dir" ]]; then
        mkdir -p "$snapshot_dir"
        screen_ansi >"$snapshot_dir/$1.ansi"
        screen >"$snapshot_dir/$1.txt"
    fi
}
clear_notes
printf 'package a;\n\n/* blok\n   yorum */\npublic class Main {\n    // not\n    private final String ad = "dize";\n    int sayi = 42;\n}\n' | put_note ornek.java 2020-01-03
printf '# veritabani\nexport DB_HOST=localhost\nDB_PORT=5432\nAPI_URL="https://example.org"\n' | put_note ornek.env 2020-01-02
printf 'import os\n\n"""belge\nmetni"""\ndef topla(a: int, b: int) -> int:\n    # yorum\n    return a + b + 42\n\nprint(topla(1, 2), "bitti", None)\n' | put_note ornek.py 2020-01-01
start_app
for shot in java:ornek.java env:ornek.env py:ornek.py; do
    wait_screen "▸ ${shot##*:}"
    keys Right
    wait_screen "← list"
    sleep 0.2
    snapshot "review-${shot%%:*}"
    check "${shot%%:*}: REVIEW uses syntax colors" ansi_has $'\e[35m'
    keys e
    wait_screen "Ctrl+S save"
    sleep 0.2
    snapshot "edit-${shot%%:*}"
    check "${shot%%:*}: EDIT shows the type in the status bar" screen_has "${shot%%:*}  UTF-8"
    keys C-q
    wait_screen "← list"
    keys Left
    wait_screen "Enter edit"
    keys Down
    sleep 0.2
done

# 16. T55b: f ile genel arama, argümanlar, bozuk argüman, d/u, D, T, Alt+ok, v, o ve kalıcılık.
clear_notes
printf 'birinci satir\naranan kelime\n' | put_note a.md 2020-01-02
printf 'x\n' | put_note b.txt 2020-01-01
start_app
keys f
wait_screen "Enter open"
literal 'aranan'
wait_screen "kelime"
check "f searches content and shows the best line" screen_has "▸ a.md"
check "f shows its mode on the top line" screen_has "[all]"
keys Enter
wait_screen "← list"
check "Enter on an f hit opens REVIEW" screen_has " REVIEW "
keys Left
wait_screen "Enter edit"
keys Escape
sleep 0.2
keys /
wait_screen "Enter keep"
literal 'type:txt'
check "a query argument filters in /" screen_lacks "a.md"
keys Escape
sleep 0.2
keys Escape
sleep 0.2
keys /
wait_screen "Enter keep"
literal 'size:abc'
wait_screen "6: "
check "an invalid argument shows its located error" screen_has "abc"
clear_prompt 8
keys Escape
sleep 0.2
keys d
wait_screen "Trashed"
check "d moves the note to the trash with no question" note_tool in-trash a.md
keys u
wait_screen "Restored"
check "u restores the last trashed" note_tool has a.md
literal $'\e[1;3C'
wait_screen "size: "
check "Alt+right opens the details of the row" screen_has "size: "
literal $'\e[1;3D'
sleep 0.2
check "Alt+left closes them" screen_lacks "size: "
keys d
wait_screen "Trashed"
keys T
wait_screen "[trash]"
check "T shows the trashed note" screen_has "a.md"
keys D
wait_screen "Delete note"
keys Enter
sleep 0.2
check "D defaults to Cancel" note_tool in-trash a.md
keys u
wait_screen "Restored"
keys q
wait_screen "Enter edit"
check "q leaves the trash view" screen_lacks "[trash]"
keys v
sleep 0.2
check "v shows the size column" screen_has " B"
keys o
wait_screen "Sort:"
keys s
sleep 0.2
check "the active sort is on the top line" screen_has "size↓"
keys q
wait_exit >/dev/null
check "sort and column are saved" grep -q 'sort = "size"' "$work/home/state/tui-state.toml"
start_app
check "the saved sort is restored" screen_has "size↓"

# 17. T61: REVIEW'da sürükleme kopyalar (konteynerde araç yok: OSC 52, iç pano); Ctrl+V iç panodan yapıştırır.
keys g
keys Right
wait_screen "← list"
# Sol panel 40 sütun, kenar 1; SGR 1 tabanlı: metnin ilk hücresi 42, ilk satır 3.
screen_x=42
mouse_press 0 "$screen_x" 3
mouse_press 32 "$((screen_x + 6))" 3
mouse_release 0 "$((screen_x + 6))" 3
wait_screen "OSC 52"
keys e
wait_screen "Ctrl+S save"
keys C-v
wait_screen "Ctrl+Shift+V"
check "the dragged text is pasted from the internal clipboard" screen_has "birincibirinci"
keys C-z
sleep 0.2

# 18. T60: gölge öneri ve Ctrl+Space. tmux C-Space NUL baytı gönderir.
keys C-End
keys Enter
literal 'ara'
wait_screen "aranan"
keys Tab
sleep 0.3
check "Tab accepts the ghost" screen_has "aranan"
literal ' ka'
keys C-Space
wait_screen "Complete"
check "Ctrl+Space opens the popup" screen_has "kelime"
literal 'l'
sleep 0.2
keys Enter
sleep 0.3
check "Enter inserts the choice" screen_has "aranan kelime"
keys Tab
sleep 0.3
keys C-s
wait_screen "Saved"
check "completion and tab reached the note" bash -c "$(declare -f note_tool); work='$work' volume='$volume' image='$image' note_tool cat a.md | grep -q 'aranan kelime'"
keys C-q
wait_screen "← list"
keys q
wait_exit >/dev/null

# 17. G6: eski düzen notları varken açılış nedenini söyler, liste boş kalsa da kullanıcı notların kaybolduğunu sanmaz.
clear_notes
rm -f "$work/home/data/migrated-notes-dirs"
printf 'eski' >"$notes/eski.md"
start_app "note graph migrate"
check "old-layout notes: the status tells to migrate" screen_has "note graph migrate"
keys q
wait_exit >/dev/null
rm -f "$notes/eski.md"

# 18. T64: Türkçe; her genişlikte T, Space, o, v, f; satır işareti, çöp, F2, ayarlar, sayfalama. Gerçek ev dizini yok.
clear_notes
printf 'birinci metin\n' | put_note is/a.md
printf 'ikinci metin\n' | put_note b.md
printf 'ucuncu metin\n' | put_note c.md
printf 'notes_dir = "/data/notes"\nlanguage = "tr"\n' >"$work/home/config/config.toml"
rm -f "$work/home/state/tui-state.toml"
ready_text="Enter düzenle"
start_app
not_in_trash() { ! note_tool in-trash "$1"; }
bottom_line() { screen | sed -n "$1p"; }
bar_has() {
    local bar
    bar="$(bottom_line "$1")"
    shift
    for hint in "$@"; do
        grep -qF -- "$hint" <<<"$bar" || return 1
    done
}
for width in 60 80 110 160; do
    tmux resize-window -t "$session" -x "$width" -y 30
    sleep 0.5
    keys Down
    sleep 0.2
    echo "bar@$width: $(bottom_line 30)"
    check "the list bar at $width columns shows the main keys" bar_has 30 "T çöp" "Space detay" "o sırala" "v sütun" "f ara" "? yardım"
done
keys g
tmux resize-window -t "$session" -x 100 -y 30
sleep 0.5
check "totals are above the hints" bar_has 29 "Toplam 3 not" "sayfa 1/1"
check "rows are numbered with a closed marker" screen_has "1 ▸ "

# Satır ayrıntısı: Space, tıklama, I.
keys Space
wait_screen "boyut:"
check "Space opens the details and the marker turns" screen_has "1 ▾ "
keys Space
sleep 0.2
check "Space closes the details" screen_lacks "boyut:"
mouse_press 0 5 3
mouse_release 0 5 3
wait_screen "boyut:"
check "clicking the marker opens the details" screen_has "1 ▾ "
mouse_press 0 5 3
mouse_release 0 5 3
sleep 0.2
check "clicking the marker again closes them" screen_lacks "boyut:"
keys I
sleep 0.2
check "I opens every row" screen_lacks "2 ▸ "
keys I
sleep 0.2

# Sıralama ve sütun.
keys o
wait_screen "Sırala:"
keys n
sleep 0.2
check "o n sorts by name" screen_has "ad↑"
keys Down
sleep 0.2
keys v
sleep 0.2
check "v changes the right column" bar_has 3 "B"

# Çöp: d, gösterge, T, salt okunur içerik, u, D, E, fare. Sıra ada göre: kök ve klasördeki notlar karışır, seçili not ekrandan okunur.
selected_name() { screen | grep -E '┃>' | grep -oE '(is/)?[abc]\.md' | head -1; }
content_of() { case "$1" in is/a.md) echo birinci ;; b.md) echo ikinci ;; c.md) echo ucuncu ;; esac; }
keys g
sleep 0.2
first="$(selected_name)"
keys d
wait_screen "Çöpe atıldı"
wait_screen "çöp: 1"
check "the top line counts the trash" bar_has 1 "çöp: 1"
check "d moved the selected note to the trash" note_tool in-trash "$first"
keys T
wait_screen "ÇÖP KUTUSU"
check "the trash hints are shown" bar_has 30 "u geri getir" "D kalıcı sil" "E boşalt" "Esc/q geri"
wait_screen "$(content_of "$first") metin"
check "REVIEW shows the trashed note" screen_has "$(content_of "$first") metin"
keys u
wait_screen "Geri alındı"
check "u restores the note" note_tool has "$first"
keys q
sleep 0.2
check "the indicator is gone when the trash is empty" screen_lacks "çöp: 1"
sleep 0.2
first="$(selected_name)"
keys d
wait_screen "çöp: 1"
mouse_press 0 2 1
mouse_release 0 2 1
wait_screen "ÇÖP KUTUSU"
check "clicking the indicator opens the trash" screen_has "ÇÖP KUTUSU"
keys D
wait_screen "Vazgeç"
keys Enter
sleep 0.3
check "D defaults to Cancel" note_tool in-trash "$first"
keys D
wait_screen "Vazgeç"
keys Right
keys Enter
wait_screen "Silindi"
check "D Right Enter deletes for good" not_in_trash "$first"
keys q
sleep 0.2
first="$(selected_name)"
keys d
wait_screen "çöp: 1"
keys g
keys d
wait_screen "çöp: 2"
keys T
wait_screen "ÇÖP KUTUSU"
keys E
wait_screen "2 not"
keys Enter
sleep 0.3
check "E defaults to Cancel" note_tool in-trash "$first"
keys E
wait_screen "2 not"
keys Right
keys Enter
wait_screen "Çöp boşaltıldı"
check "E empties the trash" not_in_trash "$first"
keys q
sleep 0.2

# F2 yol girdisi, çakışma, Esc; ayarlar `,` ve F9.
keys q
wait_exit >/dev/null
clear_notes
printf 'bir\n' | put_note x.md
printf 'iki\n' | put_note y.md
start_app
selected_note="$(screen | grep -E '┃>' | grep -oE '[xy]\.md' | head -1)"
other_note=x.md
if [[ "$selected_note" == x.md ]]; then other_note=y.md; fi
keys F2
wait_screen "Yeni yol:"
keys End
for _ in 1 2 3 4; do keys BSpace; done
literal "$other_note"
keys Enter
sleep 0.3
check "a rename onto another note keeps the input open" screen_has "Yeni yol:"
check_bytes "the collision overwrote nothing (x)" "$notes/x.md" 'bir\n'
check_bytes "the collision overwrote nothing (y)" "$notes/y.md" 'iki\n'
keys Escape
sleep 0.2
check "Esc cancels the rename" screen_lacks "Yeni yol:"
keys F2
wait_screen "Yeni yol:"
literal '-yeni'
keys Enter
wait_screen "Taşındı"
check "F2 renames at the stem" note_tool has "${selected_note%.md}-yeni.md"
keys ,
wait_screen "Ayarlar"
keys Escape
sleep 0.2
keys F9
wait_screen "Ayarlar"
keys Escape
sleep 0.2
keys q
wait_exit >/dev/null

# Sayfalama: 60 not, PgDn satır numarasını panel yüksekliği kadar taşır; REVIEW'da ^D yarım sayfa.
clear_notes
for i in $(seq 10 69); do printf 'n%s\n' "$i" | put_note "n$i.md"; done
python3 -c "
for i in range(500): print(f'L{i:03d}')" | put_note zz.md
printf 'sort = "name"\nsort_descending = false\ncolumn = "none"\nversion = 2\n' >"$work/home/state/tui-state.toml"
start_app
keys g
keys PageDown
sleep 0.2
check "PgDn pages the list" bar_has 29 "Toplam 61 not" "sayfa 2/"
keys G
sleep 0.2
keys Right
wait_screen "L000"
keys C-d
sleep 0.2
check "Ctrl+D moves REVIEW by half a page" screen_has "L012"
keys J
sleep 0.2
check "J moves REVIEW by five lines" screen_has "L017"
mouse_press 65 70 5
sleep 0.3
check "the wheel moves REVIEW by three lines" screen_lacks "L019 "
keys Left
sleep 0.2
keys q
wait_exit >/dev/null

# 19. T66: `a` listenin başında ad satırı; alt satır girdi değildir. Enter boş notu oluşturup EDIT açar.
clear_notes
printf 'bir\n' | put_note ilk.md
rm -f "$work/home/state/tui-state.toml"
start_app
keys a
wait_screen "Enter oluştur"
literal 'deneme.cpp'
check "a: the name row is the first row of the list pane" bar_has 3 "┃>" "+ deneme.cpp▏"
check "a: the row number column stays aligned" bar_has 4 "┃ 1 ▸ ilk.md"
check "a: the bottom line holds no input" test "$(bottom_line 30 | grep -c deneme)" = 0
check "a: nothing is created while typing" test "$(note_tool count)" = 1
keys Escape
sleep 0.2
check "a: Esc removes the row" screen_lacks "deneme.cpp"
check "a: Esc creates nothing" test "$(note_tool count)" = 1
keys a
wait_screen "Enter oluştur"
literal 'deneme.cpp'
keys Enter
wait_screen " DÜZENLE "
check "a: Enter creates the note at once, empty" test "$(note_tool cat deneme.cpp | wc -c)" = 0
check "a: the type follows the typed extension" screen_has "cpp  UTF-8"
literal 'return 0'
keys C-s
wait_screen "Kaydedildi"
check_bytes "a: the new note takes the typed text" "$notes/deneme.cpp" 'return 0'
keys C-q
wait_screen "← liste"
check "a: highlight follows the typed extension" ansi_has $'\e[91m'
keys Left
wait_screen "Enter düzenle"
keys a
wait_screen "Enter oluştur"
literal 'boş not.txt'
keys Enter
wait_screen " DÜZENLE "
keys C-q
wait_screen "← liste"
keys Left
wait_screen "Enter düzenle"
check "a: left empty, it stays; the stem is slugged, the extension is not" note_tool has "bos-not.txt"
for typed in deneme.xyz a.tar.gz notlar.v2 Rapor.PDF; do
    keys a
    wait_screen "Enter oluştur"
    literal "$typed"
    keys Enter
    wait_screen " DÜZENLE "
    keys C-q
    wait_screen "← liste"
    keys Left
    wait_screen "Enter düzenle"
done
check "a: an unknown extension stays" note_tool has deneme.xyz
check "a: a multi-dot name stays" note_tool has a.tar.gz
check "a: a version-like extension stays" note_tool has notlar.v2
check "a: extension case is preserved, the stem is slugged" note_tool has rapor.PDF
keys a
wait_screen "Enter oluştur"
literal 'yalin'
keys Enter
wait_screen " DÜZENLE "
keys C-q
wait_screen "← liste"
keys Left
wait_screen "Enter düzenle"
check "a: no extension takes default_type" note_tool has yalin.md
keys a
wait_screen "Enter oluştur"
literal 'yalin.md'
keys Enter
sleep 0.3
check "a: a taken name keeps the row open with the text" screen_has "+ yalin.md▏"
check "a: a taken name is an inline error, not -2" screen_lacks "yalin-2.md"
check "a: the error is in the list pane" bar_has 4 "┃Zaten"
keys Escape
sleep 0.2
keys a
wait_screen "Enter oluştur"
literal '../kacis.md'
keys Enter
sleep 0.3
check "a: an invalid name keeps the row open" screen_has "+ ../kacis.md▏"
keys Escape
sleep 0.2
# Süzgeç: satır görünür kalır; süzgeci geçmeyen yeni not için süzgeç temizlenir.
keys /
wait_screen "Enter tut"
literal 'ilk'
keys Enter
wait_screen "> ilk"
keys a
wait_screen "Enter oluştur"
check "a: the row is visible under an active filter" bar_has 3 "┃>"
literal 'filtre-disi.md'
keys Enter
wait_screen " DÜZENLE "
keys C-q
wait_screen "← liste"
check "a: the note is visible, the filter gave way" screen_has "filtre-disi.md"
keys Left
wait_screen "Enter düzenle"

# 20. Uzantı kuralı öbür yollarda: n, F2, r, note -n.
keys n
wait_screen "Yeni not"
literal 'x'
keys C-s
wait_screen "Farklı"
clear_prompt 8
literal 'n-yolu.tar.gz'
keys Enter
wait_screen "Kaydedildi"
keys C-q
wait_screen "Enter düzenle"
check "n: a multi-dot name stays" note_tool has n-yolu.tar.gz
keys F2
wait_screen "Yeni yol:"
keys End
for _ in $(seq 1 13); do keys BSpace; done
literal 'f2-yolu.v2'
keys Enter
wait_screen "Taşındı"
check "F2: a typed extension stays" note_tool has f2-yolu.v2
keys r
wait_screen "Klasör"
for _ in $(seq 1 12); do keys BSpace; done
literal 'r-yolu.cpp'
keys Enter
wait_screen "Taşındı"
check "r: a typed extension is not appended to" note_tool has r-yolu.cpp
keys q
wait_exit >/dev/null
podman run --rm -e NOTEAPP_HOME=/data/home -v "$work:/data:z" -v "$volume:/build:ro,z" "$image" \
    /build/src/cli/note -n cli-yolu.xyz '# baslik' >/dev/null
check "note -n: a typed extension stays" note_tool has cli-yolu.xyz

# 21. created ve origin gerçekten görünür: satır ayrıntısı, v sütunu, sort:created, origin:, çöp.
clear_notes
printf 'eski\n' | put_note eski.md 2020-01-02
printf 'yeni\n' | put_note yeni.md 2021-01-02
rm -f "$work/home/state/tui-state.toml"
start_app
keys Space
wait_screen "kaynak:"
check "details show the origin" screen_has "kaynak: linux"
check "details show the created date" screen_has "oluşturma: 2021-01-02"
check "details have no unknown value" screen_lacks "bilinmiyor"
keys Space
sleep 0.2
for _ in 1 2 3; do keys v; done
sleep 0.3
check "v: the created column" screen_has "2020-01-02"
for _ in 1 2; do keys v; done
sleep 0.3
check "v: the origin column" bar_has 3 "linux"
keys /
wait_screen "Enter tut"
literal 'sort:created'
sleep 0.3
check "sort:created: oldest first" bar_has 3 "eski.md"
clear_prompt 12
literal 'origin:linux'
sleep 0.3
check "origin: matches" screen_has "Toplam 2 not · 2 eşleşme"
clear_prompt 12
literal 'origin:windows'
sleep 0.3
check "origin: excludes" screen_has "Toplam 2 not · 0 eşleşme"
clear_prompt 14
keys Escape
sleep 0.2
keys d
wait_screen "çöp: 1"
keys T
wait_screen "ÇÖP KUTUSU"
keys Space
wait_screen "kaynak:"
check "trash details show the origin" screen_has "kaynak: linux"
check "trash details have no unknown value" screen_lacks "bilinmiyor"
keys Space
keys q
sleep 0.2
keys q
wait_exit >/dev/null

# 22. NOTEAPP_HOME: varsayılan notes_dir `home/notes`, depo `home/data`. Eski not yalnız notes_dir'de aranır.
check "the store is under data_dir" test -s "$work/home/data/graph.db"
check "no store in notes_dir" test ! -e "$notes/graph.db"
printf 'language = "tr"\n' >"$work/home/config/config.toml"
mkdir -p "$work/home/notes"
rm -f "$work/home/data/migrated-notes-dirs"
printf 'eski' >"$work/home/notes/eski-duz.md"
start_app "note graph migrate"
check "the default notes_dir sits under NOTEAPP_HOME" screen_has "note graph migrate"
keys q
wait_exit >/dev/null
unset ready_text

echo "passed=$passed failed=$failed"
[[ "$failed" -eq 0 ]]
