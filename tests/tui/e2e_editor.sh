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
printf 'notes_dir = "/data/notes"\nlanguage = "en"\nicons = "off"\n' >"$work/home/config/config.toml"

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
top_line_has() { screen | sed -n 1p | grep -qF -- "$1"; }
# Renk kaçışlarıyla ekran: mod rengi ve sözdizimi vurgusu gerçek terminalde görünsün.
screen_ansi() { tmux capture-pane -p -e -t "$session" 2>/dev/null || true; }
ansi_has() { screen_ansi | grep -qF -- "$1"; }
# Seçim yalnız satır vurgusudur: ters renkli satır.
selected_mark=$'\e\[7m'
selected_row_has() { screen_ansi | grep -a "$selected_mark" | grep -qF -- "$1"; }
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
# Üst kenarda sol panelin sağ köşesine (ilk ╮) dek karakter sayısı = sol panel genişliği
# (kip etiketi kenarın üstünde olduğundan ─ saymak yetmez).
list_width() {
    local LC_ALL=C.UTF-8 top
    top="$(screen | sed -n 2p)"
    top="${top%%╮*}"
    echo $((${#top} + 1))
}
one_pane() { [[ "$(screen | sed -n 2p | grep -o '╭' | wc -l)" == 1 ]]; }
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

# 5b. T91: bidi ve sıfır genişlikli karakter gerçek terminalde işaret olur; imleç adını söyler, silme yalnız
# onun baytlarını siler.
clear_notes
printf 'a\xe2\x80\xaeb\xe2\x80\x8bc' | put_note t.md
start_app
check "hidden characters show as markers in the preview" screen_has "a�b�c"
check "no raw bidi character reaches the terminal" screen_lacks "$(printf '\xe2\x80\xae')"
keys Enter
wait_screen "Ctrl+S save"
keys Right
wait_screen "U+202E RIGHT-TO-LEFT OVERRIDE"
literal $'\e[1;2C'
keys DC
keys C-s
wait_screen "Saved"
check_bytes "deleting the marker removes exactly its bytes" "$notes/t.md" 'ab\xe2\x80\x8bc'
keys C-q
wait_screen "← list"
keys Escape
wait_screen "Enter edit"
keys q
check "q quits after the hidden character check" test "$(wait_exit)" = 0

# 5c. ZWJ/ZWNJ gizli karakter değildir: emoji ZWJ dizisi işaretsiz çizilir, editörde ad söylenmez.
clear_notes
printf 'aile \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7 son' | put_note e.md
start_app
check "an emoji ZWJ sequence shows no marker in the preview" screen_lacks "�"
keys Enter
wait_screen "Ctrl+S save"
keys End
check "an emoji ZWJ sequence shows no marker in the editor" screen_lacks "�"
check "no hidden character notice for ZWJ" screen_lacks "Hidden character"
keys C-q
wait_screen "← list"
keys Escape
wait_screen "Enter edit"
keys q
check "q quits after the emoji check" test "$(wait_exit)" = 0

# 6. a: boş editör tüm terminali alır, ad ilk Ctrl+S'te sorulur; kayıttan önce dosya yok.
clear_notes
start_app
keys a
wait_screen "New note"
check "n hides the top line" screen_lacks "[name]"
check "n shows the EDIT label" screen_has " EDIT "
keys C-q
wait_screen "Enter edit"
check "Esc on an empty draft closes silently" no_notes
keys a
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
check "closing returns to two panes with the note selected" screen_has " 1   toplanti-notu.md"
check "the list went to the folder of the saved note" top_line_has "notes/İş"
check "the list label is back" screen_has " LIST "

# 6b. env yapıştırma: tuş olayı olarak gelir, ad isteminde .env görünür, değer ada girmez.
clear_notes
start_app
keys a
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
check "the default list width" width_is 33
keys '>'
sleep 0.3
check "> widens by 5" width_is 38
keys '<'
keys '<'
sleep 0.3
check "< narrows by 5" width_is 28
keys z
sleep 0.3
check "z hides the right pane" one_pane
keys z
sleep 0.3
check "z again brings it back" width_is 28
mouse_press 0 29 5
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
check "a click selects the second row" selected_row_has "f.md"
mouse_press 64 5 5
sleep 0.2
check "wheel up over the list moves the selection" selected_row_has "g.md"

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

# 10c. NY-T3: sürüklenen genişlik bırakınca tui-state.toml'a yazılır (çıkışı beklemez: start_app SIGKILL'le
# kapatır), yeniden açılışta kalır. Sonra varsayılana döner: sonraki bölümler 33 sütun bekler.
start_app
check "the dragged list width survives a restart" width_is 51
mouse_press 0 51 5
mouse_press 32 33 5
mouse_release 0 33 5
check "the divider drags back to the default width" width_is 33

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
keys a
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
# Sol panel 33 (100 sütunun üçte biri), kenar 1, iç boşluk 1, numara sütunu 3: metnin ilk hücresi 38, ilk satır 2.
check "the terminal cursor is at the first text cell after the gutter" cursor_at 38 2
keys Down
keys End
sleep 0.3
check "the terminal cursor follows after motion" cursor_at 44 3
check "the gutter shows the line number" screen_has " 2 ikinci"
check "rows past the end show a tilde" screen_has "~"
check "the status bar names the file and format" screen_has "UTF-8  LF"
mouse_press 0 41 3
mouse_release 0 41 3
sleep 0.3
check "a click lands on the text cell, the gutter is not text" cursor_at 40 2
keys C-q
wait_screen "← list"
check "REVIEW has no gutter" screen_has "│ birinci"
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
    wait_screen " ${shot##*:}"
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

# 16. T55b: s ile genel arama, argümanlar, bozuk argüman, d/u, D, t, Alt+ok, V, o ve kalıcılık.
clear_notes
printf 'birinci satir\naranan kelime\n' | put_note a.md 2020-01-02
printf 'x\n' | put_note b.txt 2020-01-01
start_app
keys s
wait_screen "Enter open"
literal 'aranan'
wait_screen "kelime"
check "s searches content and shows the best line" screen_has " a.md"
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
keys t
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
keys V
sleep 0.2
check "V shows the size column" screen_has " B"
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
keys g
keys Right
wait_screen "← list"
# Sol panel 33 sütun, kenar 1, iç boşluk 1; SGR 1 tabanlı: metnin ilk hücresi 36, ilk satır 3.
screen_x=36
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

# 19. T68: girdi satırı. Gölge, Tab listesi, renk, Ctrl+T, geçmiş; aynı model `n`de de çalışır.
clear_notes
printf 'plan\n' | put_note work/plan.md
printf 'todo\n' | put_note personal/todo.cpp
start_app "→ open"
keys /
literal 'pla'
wait_screen "pla▏n.md"
check "the search ghost is dim" ansi_has $'\e[2mn.md'
keys Right
wait_screen "plan.md▏"
for _ in 1 2 3 4 5 6 7; do keys BSpace; done
literal 'sort:'
keys Tab
wait_screen "Complete"
check "Tab lists argument values" screen_has "sort:updated"
keys Down
keys Enter
wait_screen "sort:size▏"
keys C-t
sleep 0.2
check "Ctrl+T keeps the search input" screen_has "sort:size▏"
literal ' sync:nope'
sleep 0.3
check "the invalid value is red at once" ansi_has $'\e[31mnope'
for _ in 1 2 3 4 5 6 7 8 9 10; do keys BSpace; done
keys C-t
keys Enter
sleep 0.3
keys /
for _ in 1 2 3 4 5 6 7 8 9; do keys BSpace; done
keys C-p
wait_screen "sort:size▏"
keys Escape
sleep 0.2
keys n
literal 'wo'
wait_screen "wo▏rk/"
keys Right
wait_screen "work/▏"
keys Tab
wait_screen "Complete"
check "the name row lists notes" screen_has "work/plan.md"
keys Escape
sleep 0.2
keys Escape
sleep 0.2
keys q
wait_exit >/dev/null
check "history is stored locally in tui-state.toml" grep -qF 'search_history = ["sort:size"' "$work/home/state/tui-state.toml"

# 17. G6: eski düzen notları varken açılış nedenini söyler, liste boş kalsa da kullanıcı notların kaybolduğunu sanmaz.
clear_notes
rm -f "$work/home/data/migrated-notes-dirs"
printf 'eski' >"$notes/eski.md"
start_app "note graph migrate"
check "old-layout notes: the status tells to migrate" screen_has "note graph migrate"
keys q
wait_exit >/dev/null
rm -f "$notes/eski.md"

# 18. T64: Türkçe; her genişlikte t, Space, o, V, s; satır işareti, çöp, F2, ayarlar, sayfalama. Gerçek ev dizini yok.
clear_notes
printf 'birinci metin\n' | put_note a.md
printf 'ikinci metin\n' | put_note b.md
printf 'ucuncu metin\n' | put_note c.md
printf 'notes_dir = "/data/notes"\nlanguage = "tr"\nicons = "off"\n' >"$work/home/config/config.toml"
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
    # 60 sütunda `s ara` bir hücreyle sığmaz: sağa sabitlenen `F1 yardım` yer tutar (SPEC-tui "Yerleşim").
    if [[ "$width" -eq 60 ]]; then
        check "the list bar at $width columns shows the main keys" bar_has 30 ": komut" "t çöp" "Space aç" "o sırala" "V sütun" "F1 yardım"
    else
        check "the list bar at $width columns shows the main keys" bar_has 30 ": komut" "t çöp" "Space aç" "o sırala" "V sütun" "s ara" "F1 yardım"
    fi
done
keys g
keys g
tmux resize-window -t "$session" -x 100 -y 30
sleep 0.5
check "totals are above the hints" bar_has 29 "Toplam 3 not" "sayfa 1/1"
check "notes are numbered and carry no arrow" screen_has "1   "

# Satır ayrıntısı: Space, tıklama, I.
keys Space
wait_screen "boyut:"
check "note details have no marker" screen_lacks "▾"
keys Space
sleep 0.2
check "Space closes the details" screen_lacks "boyut:"
mouse_press 0 5 3
mouse_release 0 5 3
sleep 0.2
check "clicking a note row only selects, no details" screen_lacks "boyut:"
keys I
sleep 0.2
check "I opens every row" test "$(screen | grep -c "boyut:")" = 3
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
keys V
sleep 0.2
check "v changes the right column" bar_has 3 "B"

# Çöp: d, gösterge, T, salt okunur içerik, u, D, E, fare. Sıra ada göre: kök ve klasördeki notlar karışır, seçili not ekrandan okunur.
selected_name() { screen_ansi | grep -a "$selected_mark" | grep -oE '[abc]\.md' | head -1; }
content_of() { case "$1" in a.md) echo birinci ;; b.md) echo ikinci ;; c.md) echo ucuncu ;; esac; }
keys g
keys g
sleep 0.2
first="$(selected_name)"
keys d
wait_screen "Çöpe atıldı"
wait_screen "çöp: 1"
check "the top line counts the trash" bar_has 1 "çöp: 1"
check "d moved the selected note to the trash" note_tool in-trash "$first"
keys t
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
keys g
keys d
wait_screen "çöp: 2"
keys t
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
selected_note="$(screen_ansi | grep -a "$selected_mark" | grep -oE '[xy]\.md' | head -1)"
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
for i in range(500): print(f'L{i:03d}')" | put_note zz.txt
printf 'sort = "name"\nsort_descending = false\ncolumn = "none"\nversion = 2\n' >"$work/home/state/tui-state.toml"
start_app
keys g
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

# Biçimli görünümde kaydırma birimi görüntü satırıdır: Pnnn satırı 2+2n'de (öncesinde akan paragraf ve boşluk).
python3 -c "
print('L000\nL001\n')
for i in range(100): print(f'P{i:03d}\n')" | put_note zzz.md
start_app
keys G
sleep 0.2
keys Right
wait_screen "L000 L001"
check "soft line breaks flow into one rendered paragraph" bar_has 3 "L000 L001"
keys C-d
sleep 0.2
check "Ctrl+D moves rendered REVIEW by half a page" bar_has 3 "P005"
keys J
sleep 0.2
check "J moves rendered REVIEW by five display lines" bar_has 4 "P008"
check "J left the earlier block behind" screen_lacks "P007"
mouse_press 65 70 5
sleep 0.3
check "the wheel moves rendered REVIEW by three display lines" bar_has 3 "P009"
keys R
sleep 0.2
check "R keeps the same block on top of the raw view" bar_has 3 "P009"
check "the pane label names the raw view" screen_has "· ham"
keys J
sleep 0.2
check "raw REVIEW scrolls by source lines" bar_has 4 "P012"
keys R
sleep 0.2
check "the pane label names the rendered view" screen_has "· görünüm"
keys Left
sleep 0.2
keys q
wait_exit >/dev/null

# 19. T66: `a` listenin başında ad satırı; alt satır girdi değildir. Enter boş notu oluşturup EDIT açar.
clear_notes
printf 'bir\n' | put_note ilk.md
rm -f "$work/home/state/tui-state.toml"
start_app
keys n
wait_screen "Enter oluştur"
literal 'deneme.cpp'
check "a: the name row is the first row of the list pane" bar_has 3 "│ " "+ deneme.cpp▏"
check "a: the row number column stays aligned" bar_has 4 "│  1   ilk.md"
check "a: the bottom line holds no input" test "$(bottom_line 30 | grep -c deneme)" = 0
check "a: nothing is created while typing" test "$(note_tool count)" = 1
keys Escape
sleep 0.2
check "a: Esc removes the row" screen_lacks "deneme.cpp"
check "a: Esc creates nothing" test "$(note_tool count)" = 1
keys n
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
keys n
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
    keys n
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
keys n
wait_screen "Enter oluştur"
literal 'yalin'
keys Enter
wait_screen " DÜZENLE "
keys C-q
wait_screen "← liste"
keys Left
wait_screen "Enter düzenle"
check "a: no extension takes default_type" note_tool has yalin.md
keys n
wait_screen "Enter oluştur"
literal 'yalin.md'
keys Enter
sleep 0.3
check "a: a taken name keeps the row open with the text" screen_has "+ yalin.md▏"
check "a: a taken name is an inline error, not -2" screen_lacks "yalin-2.md"
check "a: the error is in the list pane" bar_has 4 "│ Zaten"
keys Escape
sleep 0.2
keys n
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
keys n
wait_screen "Enter oluştur"
check "a: the row is visible under an active filter" bar_has 3 "│ " "+ "
literal 'filtre-disi.md'
keys Enter
wait_screen " DÜZENLE "
keys C-q
wait_screen "← liste"
check "a: the note is visible, the filter gave way" screen_has "filtre-disi.md"
keys Left
wait_screen "Enter düzenle"

# 20. Uzantı kuralı öbür yollarda: a, F2, r, note -n.
keys a
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

# 21. created ve origin gerçekten görünür: satır ayrıntısı, V sütunu, sort:created, origin:, çöp.
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
for _ in 1 2 3; do keys V; done
sleep 0.3
check "V: the created column" screen_has "2020-01-02"
for _ in 1 2; do keys V; done
sleep 0.3
check "V: the origin column" bar_has 3 "linux"
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
keys t
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

# 21b. T70: klasör gezinmesi. `a` ile klasör, içine not, numara yeniden başlar, ad değiştir, geri dön, sil.
clear_notes
printf 'kok\n' | put_note kok.md
printf 'notes_dir = "/data/notes"\nlanguage = "en"\nicons = "off"\n' >"$work/home/config/config.toml"
rm -f "$work/home/state/tui-state.toml"
ready_text="Enter edit"
start_app
keys n
literal 'proje/'
wait_screen "proje/▏"
keys Enter
wait_screen "Folder created: proje"
check "a with a slash stays in the list, no editor" screen_has "LIST"
check "the folder row comes first" screen_has "1 ▸ proje/"
check "the note row follows" screen_has "2   kok.md"
check "the top line is the root" top_line_has "notes"
check "the totals count the folder" screen_has "Folder: 2 items · Total 1 note"
keys Up
wait_screen "n add, name/ = folder"
check "the hint bar names rename on a folder" screen_has "F2 rename"
keys l
wait_screen "notes/proje"
check "an empty folder shows the add hint" screen_has "n add, name/ = folder"
keys n
literal 'ic.md'
wait_screen "ic.md▏"
keys Enter
wait_screen "Ctrl+S save"
literal 'icerik'
keys C-s
wait_screen "Saved"
keys C-q
wait_screen "← list"
check_bytes "a note is created inside the current folder" "$notes/proje/ic.md" 'icerik'
keys h
wait_screen "Enter edit"
check "numbers restart inside the folder" screen_has "1   ic.md"
check "the short name is shown" screen_lacks "proje/ic.md"
keys n
literal 'alt/iki.md'
keys Enter
wait_screen "Ctrl+S save"
keys C-q
wait_screen "← list"
keys h
wait_screen "notes/proje/alt"
keys h
wait_screen "1 ▸ alt/"
check "going back selects the left folder" screen_has "1 item"
keys h
wait_screen "▸ proje/"
check "going back to the root keeps the folder" top_line_has "notes"
keys F2
wait_screen "proje▏"
for _ in 1 2 3 4 5; do keys BSpace; done
literal 'yeni'
keys Enter
wait_screen "Moved: proje -> yeni"
check "the folder is renamed in the list" screen_has "▸ yeni/"
check_bytes "the note moved with the folder" "$notes/yeni/ic.md" 'icerik'
keys /
literal 'ic'
wait_screen "yeni/ic.md"
check "search is flat with full paths" screen_has "yeni/ic.md"
keys Enter
wait_screen "Enter edit"
keys Left
wait_screen "notes/yeni"
check "left on a result goes to its folder" screen_has "ic.md"
keys h
wait_screen "▸ yeni/"
keys d
wait_screen "Trash folder"
check "the confirmation shows the counts" screen_has "2 notes"
keys Enter
sleep 0.3
check "default Cancel keeps the folder" screen_has "▸ yeni/"
keys d
wait_screen "Trash folder"
keys Right
keys Enter
wait_screen "Moved to trash: yeni, 2 notes"
check "the folder left the list" screen_lacks "▸ yeni/"
check "the note is in the trash" note_tool in-trash yeni/ic.md
keys u
wait_screen "Restored: yeni, 2 notes"
check_bytes "u brings the note back" "$notes/yeni/ic.md" 'icerik'
keys D
wait_screen "Delete folder"
keys Right
keys Enter
wait_screen "Deleted: yeni"
check "D removed the folder" screen_lacks "▸ yeni/"
check "D is not the trash" not_in_trash yeni/ic.md
keys q
wait_exit >/dev/null

# 21c. T71: çoklu seçim, toplu `d` ve `u`, `y` `p`, çakışma `-2`, `g` öneki.
clear_notes
printf 'A\n' | put_note a.md
printf 'B\n' | put_note b.md
printf 'C\n' | put_note c.md
printf 'K\n' | put_note hedef/a.md
printf 'sort = "name"\nsort_descending = false\ncolumn = "none"\nversion = 2\n' >"$work/home/state/tui-state.toml"
start_app "→ open"
keys Down
keys v
keys v
wait_screen "selected: 2"
check "marked rows carry the text marker" screen_has "*2"
check "marked rows are yellow" ansi_has $'\e[1m\e[33m*'
keys d
wait_screen "Moved to trash: 2 items"
check "d trashes the selection" note_tool in-trash a.md
check "d trashes the whole selection" note_tool in-trash b.md
keys u
wait_screen "Restored: 2 items"
check "one u restores the batch" note_tool has a.md
check "one u restores all of it" note_tool has b.md
keys g
wait_screen "gg top"
keys g
sleep 0.2
keys Down
keys y
wait_screen "copy: 1"
keys g
keys g
keys Enter
wait_screen "notes/hedef"
keys p
wait_screen "1 done, 0 failed"
check "the collision is reported" screen_has "a-2.md"
check_bytes "copy got -2" "$notes/hedef/a-2.md" 'A\n'
check_bytes "copy overwrote nothing" "$notes/hedef/a.md" 'K\n'
keys Escape
sleep 0.2
keys h
wait_screen "→ open"
keys Down
keys y
wait_screen "copy: 1"
keys Escape
sleep 0.2
check "Esc cancels the pending yank" screen_lacks "copy: 1"
keys q
wait_exit >/dev/null

# 21d. T74: yerinde açılan ağaç, `m`, satır görünümü (60 ve 110 sütun, 30 satır), Nerd Font glif baytları.
clear_notes
printf 'n\n' | put_note proje/alt/derin/not.md
printf 'p\n' | put_note proje/alt/b.py
printf 'c\n' | put_note proje/c.cpp
printf 'o\n' | put_note oku.md
printf '{}\n' | put_note veri.json
printf 'x\n' | put_note dokuman/x.md
printf 'notes_dir = "/data/notes"\nlanguage = "en"\nicons = "auto"\n' >"$work/home/config/config.toml"
printf 'sort = "name"\nsort_descending = false\ncolumn = "size"\nversion = 2\n' >"$work/home/state/tui-state.toml"
start_app "→ open"
tmux resize-window -t "$session" -x 110 -y 30
sleep 0.5
keys Down
keys Space
wait_screen "▾ "
check "Space opens the folder in place" top_line_has "notes"
check "children are numbered within their folder, indented" screen_has "   1 ▸ "
check "no selection marker at the far left" screen_lacks ">1"
check "the selected row is highlighted" selected_row_has "proje/"
keys Down
keys Space
sleep 0.2
keys Down
keys Space
sleep 0.3
snapshot tree-110
icon_gap_holds() { screen | grep -qF -- "$(printf '\xf3\xb0\x8d\x94')  oku.md"; }
check "the glyph keeps an empty cell before the name" icon_gap_holds
tree_file="$(mktemp)"
screen >"$tree_file"
glyph_in_capture() { grep -qF -- "$(printf '%b' "$1")" "$tree_file"; }
check "folder closed glyph bytes (U+F024B)" glyph_in_capture '\xf3\xb0\x89\x8b'
check "folder open glyph bytes (U+F0770)" glyph_in_capture '\xf3\xb0\x9d\xb0'
check "markdown glyph bytes (U+F0354)" glyph_in_capture '\xf3\xb0\x8d\x94'
check "python glyph bytes (U+E606)" glyph_in_capture '\xee\x98\x86'
check "cpp glyph bytes (U+E646)" glyph_in_capture '\xee\x99\x86'
check "json glyph bytes (U+E60B)" glyph_in_capture '\xee\x98\x8b'
rm -f "$tree_file"
check "the folder glyph is blue" ansi_has $'\e[34m'
check "the python glyph is green" ansi_has $'\e[32m'
keys h
sleep 0.2
tmux resize-window -t "$session" -x 60 -y 30
sleep 0.5
check "the tree fits 60 columns" screen_has "▾ "
# 60 sütunda sağa sabit `F1 help` `F2 rename`in yerini alır; F2 110 sütunda da denetlenir.
check "folder hints stay at 60 columns" bar_has 30 "→ open" "F1 help"
snapshot tree-60
tmux resize-window -t "$session" -x 110 -y 30
sleep 0.5
check "folder hints at 110 columns name rename" bar_has 30 "→ open" "F2 rename" "F1 help"
keys q
wait_exit >/dev/null
check "the open set is saved" grep -qF 'open_folders = ' "$work/home/state/tui-state.toml"
start_app "→ open"
check "the open set is restored" screen_has "▾ "
keys g
keys g
keys Down
keys Space
sleep 0.2
keys g
keys g
keys m
wait_screen "Move to:"
literal 'hedef/yeni/'
keys Enter
wait_screen "Moved to: hedef/yeni"
check_bytes "m moves a folder into a new nested folder" "$notes/hedef/yeni/dokuman/x.md" 'x\n'
keys h
sleep 0.2
keys h
wait_screen "→ open"
keys q
wait_exit >/dev/null
printf 'x2\n' | put_note hedef/oku.md
printf 'sort = "name"\nsort_descending = false\ncolumn = "size"\nversion = 2\n' >"$work/home/state/tui-state.toml"
start_app "→ open"
keys G
keys k
sleep 0.2
keys m
wait_screen "Move to:"
literal 'hed'
keys Tab
sleep 0.3
check "Tab completes the folder" screen_has "hedef/"
keys Enter
sleep 0.3
check "a collision stays inline" screen_has "Move to:"
check_bytes "the collision overwrote nothing" "$notes/hedef/oku.md" 'x2\n'
check_bytes "the source stays after a collision" "$notes/oku.md" 'o\n'
keys Escape
sleep 0.2
keys q
wait_exit >/dev/null
printf 'notes_dir = "/data/notes"\nlanguage = "en"\nicons = "off"\n' >"$work/home/config/config.toml"
start_app "→ open"
check "icons off shows no glyph" test "$(screen | grep -c "$(printf '\xf3\xb0\x89\x8b')")" = 0
keys q
wait_exit >/dev/null

# 22. NOTEAPP_HOME: varsayılan notes_dir `home/notes`, depo `home/data`. Eski not yalnız notes_dir'de aranır.
check "the store is under data_dir" test -s "$work/home/data/graph.db"
check "no store in notes_dir" test ! -e "$notes/graph.db"
printf 'language = "tr"\nicons = "off"\n' >"$work/home/config/config.toml"
mkdir -p "$work/home/notes"
rm -f "$work/home/data/migrated-notes-dirs"
printf 'eski' >"$work/home/notes/eski-duz.md"
start_app "note graph migrate"
check "the default notes_dir sits under NOTEAPP_HOME" screen_has "note graph migrate"
keys q
wait_exit >/dev/null
unset ready_text

# 23. Fark çıktısı (T75): terminale yalnız değişen hücre yazılır. Uzun oturumdan sonra ekran, Ctrl+L ile baştan
# çizilenle ve yeni süreçle hücre hücre aynı olmalı: Türkçe, CJK, birleşik işaret ve Nerd Font glifiyle.
capture_settled() { sleep 0.5; screen_ansi; }
matches_repaint() {
    local before after
    before="$(capture_settled)"
    keys C-l
    after="$(capture_settled)"
    if [[ "$before" != "$after" ]]; then
        diff <(printf '%s\n' "$before") <(printf '%s\n' "$after") | head -20
        return 1
    fi
}
clear_notes
printf 'notes_dir = "/data/notes"\nlanguage = "en"\nicons = "auto"\neditor = "true"\n' >"$work/home/config/config.toml"
for index in 1 2 3 4 5 6 7 8; do
    printf '# başlık %s\nışğü İŞĞÜ 漢字語 e\xcc\x81 satır %s\n\n- madde\n' "$index" "$index" | put_note "şablon-$index.md"
done
printf '漢字語 geniş\n' | put_note "漢字語-not.md"
printf '{"anahtar": "değer"}\n' | put_note proje/veri.json
start_app "→ open"
for round in 1 2 3 4 5 6 7 8 9 10; do
    keys j j j j j k k j k j j k k k k k k k
done
check "selection walk leaves no artifacts" matches_repaint
keys F1
sleep 0.2
keys Escape
check "a closed modal leaves no artifacts" matches_repaint
keys /
literal 'şab'
keys Escape
sleep 0.2
check "a cancelled search leaves no artifacts" matches_repaint
keys j
keys Enter
sleep 0.3
literal 'yazı ğü 漢字 '
keys Left Left Up Down BSpace
check "the editor leaves no artifacts" matches_repaint
keys C-q
wait_screen "Unsaved changes"
keys Enter
wait_screen "← list"
keys Escape
wait_screen "Enter edit"
keys Escape  # süzgeç temizlenir: yeni süreçle karşılaştırma aynı listeyi görsün
sleep 0.3
keys E
sleep 0.5
check "the external editor return repaints cleanly" matches_repaint
tmux resize-window -t "$session" -x 130 -y 35
check "growing the terminal repaints cleanly" matches_repaint
tmux resize-window -t "$session" -x 70 -y 20
check "shrinking the terminal repaints cleanly" matches_repaint
tmux resize-window -t "$session" -x 100 -y 30
for round in 1 2 3 4 5 6 7 8 9 10 11 12; do
    keys k
done
long_session="$(capture_settled)"
keys q
wait_exit >/dev/null
start_app "→ open"
fresh_start="$(capture_settled)"
same_as_fresh() {
    if [[ "$long_session" != "$fresh_start" ]]; then
        diff <(printf '%s\n' "$long_session") <(printf '%s\n' "$fresh_start") | head -20
        return 1
    fi
}
check "the long session equals a fresh process" same_as_fresh
keys q
wait_exit >/dev/null

# T78: bağlantı bölümü, gerçek terminalde görünür ve nota gider.
clear_notes
printf 'hedef notu\n' | put_note hedef.md
printf '[[hedef]] [[yok]]\n' | put_note kaynak.md
start_app
keys /
literal kaynak
keys Enter
sleep 0.2
keys Right
wait_screen "Outgoing (1)"
check "the links section lists a missing target" screen_has "[[yok]]"
check "the preview hints name the section key" screen_has "b links"
keys b
sleep 0.2
check "b focuses the section" screen_has "Enter open"
keys Enter
wait_screen "Incoming (1)"
check "Enter jumps to the linked note" screen_has "kaynak.md"
keys q
wait_exit >/dev/null

# T79: komut paleti. Tablodaki tuşlar gerçek terminalde palet ve F1'de.
clear_notes
printf 'notes_dir = "/data/notes"\nlanguage = "en"\nicons = "off"\n' >"$work/home/config/config.toml"
printf 'bir\n' | put_note palet.md
printf 'iki\n' | put_note klasor/alt.md
start_app "→ open"
check "the hint bar leads with the palette" bar_has 30 ": cmd"
literal ':'
wait_screen "Commands"
check "the palette names the selection" screen_has "klasor"
# Satır: etiket solda; kimlik ve tuş sağ kenarda.
palette_row_has() { screen | grep -qE -- "$1"; }
check "a row shows the label, then id and key at the right" palette_row_has '│New note +new  a │'
keys Escape
sleep 0.2
check "Esc closes the palette" screen_lacks "Commands"
literal ':'
wait_screen "Commands"
literal 'zzqq'
check "a hopeless filter says so" screen_has "No match"
keys BSpace BSpace BSpace BSpace
literal 'links.focus'
check "an unavailable command shows why" screen_has "only in REVIEW"
keys Enter
sleep 0.2
check "Enter does not run an unavailable command" screen_has "Commands"
keys Escape
sleep 0.2
keys j
literal ':'
wait_screen "Commands"
literal 'move'
wait_screen " move  m "
keys Enter
wait_screen "Move to"
check "the palette runs the same move as m" screen_has "Move to"
keys Escape
sleep 0.2
# Tablodaki tuşlar: palet sayfa sayfa yürünür, sağdaki tuşlar F1'de aranır.
palette_keys="$work/palette-keys"
: >"$palette_keys"
literal ':'
wait_screen "Commands"
for page in $(seq 1 12); do
    # Palet kutusu panellerin üstündedir: satır `etiket  kimlik  tuşlar │`; kimlikten sonra iki boşlukla
    # ayrılan tuşlar alınır, tuşsuz satır (`etiket  kimlik │`) atlanır.
    screen | grep -oE '│[^│]*[^ ]  +[a-z][a-z0-9_.]*  [^│]*[^ ] │' |
        sed -nE 's/^│.*[^ ]  +[a-z][a-z0-9_.]*  ([^ ].*[^ ]|[^ ]) │$/\1/p' >>"$palette_keys"
    for step in 1 2 3 4 5 6 7 8; do
        keys Down
    done
    sleep 0.2
done
keys Escape
sleep 0.2
keys F1
sleep 0.2
help_screens="$work/help-screens"
: >"$help_screens"
for page in $(seq 1 45); do
    screen | sed 's/│/ /g; s/[()]/ /g' >>"$help_screens"
    keys PageDown
    sleep 0.05
done
keys Escape
sleep 0.2
every_palette_key_is_in_help() {
    local token missing=0 count=0
    while read -r -a tokens; do
        for token in "${tokens[@]}"; do
            count=$((count + 1))
            if ! grep -qE -- "(^| )$(printf '%s' "$token" | sed 's/[][\.*^$?+(){}|/]/\\&/g')( |$)" "$help_screens"; then
                echo "not in F1: $token"
                missing=1
            fi
        done
    done <"$palette_keys"
    [[ "$count" -ge 30 && "$missing" -eq 0 ]]
}
check "every key the palette shows is in F1" every_palette_key_is_in_help
# Tuşsuz komut ve son kullanılan: ikon ayarı config.toml'a yazar, yeniden açılışta başta gelir.
literal ':'
wait_screen "Commands"
literal 'icons.toggle'
wait_screen " icons.toggle "
keys Enter
sleep 0.3
# toml++ yazarken tek tırnak seçebilir.
icons_set_to() { podman unshare cat "$work/home/config/config.toml" | grep -qE -- "^icons = [\"']$1[\"']$"; }
check "icons.toggle writes config.toml" icons_set_to nerd
keys q
wait_exit >/dev/null
start_app "→ open"
literal ':'
wait_screen "Commands"
# İlk satır: başlık ve girdi satırından sonraki palet satırı.
check "the recent command comes first after a restart" bash -c "tmux capture-pane -p -t '$session' | grep -A2 -F 'Commands' | sed -n 3p | grep -qF ' icons.toggle '"
keys Enter
sleep 0.3
check "icons.toggle flips back" icons_set_to off
literal ':'
wait_screen "Commands"
keys Escape
sleep 0.2
# EDIT: `:` metindir.
keys j Enter
wait_screen "Ctrl+S save"
literal ':x'
check "colon is text in the editor" screen_lacks "Commands"
check "colon reached the buffer" screen_has ":x"
keys C-z C-z
sleep 0.2
keys C-q
sleep 0.3
keys q
wait_exit >/dev/null

# T3: sekmeler gerçek terminalde. Düzenlenen sekme korunur, Ctrl+K onu yazıp aramaya geçer, sonraki not yeni
# sekmede açılır; Alt+P geçer, Ctrl+W kapatır, açık sekme yeniden açılışta kalır.
clear_notes
printf 'alfa\n' | put_note a.md
printf 'beta\n' | put_note b.md
start_app
keys /
literal a.md
keys Enter
sleep 0.2
keys Enter
wait_screen "Ctrl+S save"
literal 'A'
keys C-k
sleep 0.3
check_bytes "Ctrl+K writes the pending edit to its own note" "$notes/a.md" 'Aalfa\n'
check "the tab line shows the parked tab" screen_has "[a.md]"
clear_prompt 4  # arama girdisi `/` gibi önceki sorguyla açılır
literal b.md
keys Enter
sleep 0.2
keys Enter
wait_screen "Ctrl+S save"
check "the edited tab is kept, the next note opens in a new tab" screen_has "a.md │ [b.md]"
keys M-p
sleep 0.3
check "Alt+P goes to the previous tab" screen_has "[a.md] │ b.md"
check "the tab kept its text" screen_has "Aalfa"
keys C-w
sleep 0.3
check "Ctrl+W closes the clean tab and the neighbour comes forward" screen_has "[b.md]"
check "the closed tab is gone" screen_lacks "a.md │"
check "the neighbour is open in the editor" screen_has "Ctrl+S save"
check_bytes "closing wrote nothing else" "$notes/b.md" 'beta\n'

# T4: harita. `view.map` sol panelde çizgeyi çizer; Tab not seçer, Enter sekme kuralıyla açar.
keys C-k
sleep 0.3
keys Escape
sleep 0.2
literal ':'
wait_screen "Commands"
literal 'view.map'
wait_screen " view.map "
keys Enter
wait_screen "depth 2"
check "the map shows its scope line" screen_has "Tab select"
check "the current note is on the map" screen_has "b.md"
keys Tab Enter
wait_screen "Ctrl+S save"
check "Enter on the map opens the selected note" screen_has "[b.md]"
keys C-k
sleep 0.3
keys Escape
sleep 0.2
keys q
wait_exit >/dev/null
start_app
check "open tabs come back after a restart" screen_has "[b.md]"
keys q
wait_exit >/dev/null

# F1: `L` adlı ilişki. Hedeften sonra ad satırı; Ctrl+D yönlü yapar, Tab var olan adı tamamlar. Sonuç
# `note relation ls` ile ayrı süreçten okunur.
relations_of() {
    podman run --rm -e NOTEAPP_HOME=/data/home -v "$work:/data:z" -v "$volume:/build:ro,z" "$image" \
        /build/src/cli/note relation ls "$1"
}
relation_listed() { relations_of "$1" | grep -qF -- "$2"; }
clear_notes
printf 'ra\n' | put_note ra.md
printf 'rb\n' | put_note rb.md
printf 'rc\n' | put_note rc.md
start_app
keys /
literal ra.md
keys Enter
sleep 0.3
keys L
wait_screen "Relate to:"
literal rb.md
keys Enter
wait_screen "Relation ra.md — rb.md, name:"
check "the name line names both notes and starts undirected" screen_has "^D directed"
literal uses
keys C-d
sleep 0.3
check "Ctrl+D turns the relation directed" screen_has "Relation ra.md → rb.md, name:"
keys Enter
wait_screen "Related as 'uses': ra.md and rb.md"
check "L with a name and Ctrl+D stores a named directed relation" relation_listed ra.md $'rb.md\tuses\toutgoing'
keys L
wait_screen "Relate to:"
literal rc.md
keys Enter
wait_screen "Relation ra.md — rc.md, name:"
literal u
keys Tab
sleep 0.3
check "Tab completes a known name and takes its direction" screen_has "Relation ra.md → rc.md, name: uses"
keys Enter
wait_screen "Related as 'uses': ra.md and rc.md"
check "the completed name is stored with it" relation_listed ra.md $'rc.md\tuses\toutgoing'
keys q
wait_exit >/dev/null

echo "passed=$passed failed=$failed"
[[ "$failed" -eq 0 ]]
