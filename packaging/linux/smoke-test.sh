#!/usr/bin/env bash
# Temiz bir dağıtım konteynerinde AppImage smoke testi. Kullanım (depo kökünden, motor docker ya da podman):
#   podman run --rm -e APPIMAGE_EXTRACT_AND_RUN=1 -v "$PWD/out:/app:ro,z" -v "$PWD/packaging/linux:/t:ro,z" \
#       fedora:44 bash /t/smoke-test.sh /app/noteapp-<sürüm>-x86_64.AppImage
# FUSE olmayan konteynerde APPIMAGE_EXTRACT_AND_RUN=1, --appimage-extract-and-run ile aynıdır; symlink'te de geçerlidir.
# Kütüphane testleri hiçbir paket kurulmadan koşar; yalnız sonrasında `script` ve CA paketi kurulur (TUI pty'si, TLS).
set -uo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }

appimage="${1:?usage: smoke-test.sh /path/to/noteapp.AppImage}"
[[ -x "$appimage" ]] || die "not executable: $appimage"
tls_url="${SMOKE_TLS_URL:-https://github.com/octocat/Spoon-Knife.git}"
timeout_seconds="${SMOKE_TIMEOUT_SECONDS:-5}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
failures=0

# Her satır: ad, gözlenen ve beklenen çıkış kodu. Tek kural: uyuşmayan her satır sonucu kırar.
check() {
    local name="$1" actual="$2" expected="$3"
    if [[ "$actual" == "$expected" ]]; then
        echo "PASS $name: exit $actual"
    else
        echo "FAIL $name: exit $actual (expected $expected)"
        failures=$((failures + 1))
    fi
}

probe() {
    local name="$1" condition="$2"
    if [[ "$condition" == "yes" ]]; then
        echo "PASS $name"
    else
        echo "FAIL $name"
        failures=$((failures + 1))
    fi
}

. /etc/os-release
echo "== $PRETTY_NAME"

ln -s "$appimage" "$work/note"
ln -s "$appimage" "$work/notes"
ln -s "$appimage" "$work/notes-gui"
note="$work/note"

export NOTEAPP_HOME="$work/home"
mkdir -p "$NOTEAPP_HOME/config"
notes_dir="$work/notes-data"
"$note" config set notes_dir "$notes_dir" >/dev/null
check "note config set notes_dir" "$?" 0

# Paket içi bağımlılık çözümü: host glibc dışında çözülemeyen kütüphane olmamalı.
squash="$work/extract"
mkdir -p "$squash"
(cd "$squash" && "$appimage" --appimage-extract >/dev/null)
check "--appimage-extract" "$?" 0
for binary in note notes notes-gui; do
    if command -v ldd >/dev/null; then
        missing="$(ldd "$squash/squashfs-root/usr/bin/$binary" 2>&1 | grep 'not found')"
        [[ -z "$missing" ]] && verdict=yes || verdict=no
        [[ -z "$missing" ]] || echo "$missing"
        probe "ldd $binary: no missing library" "$verdict"
    fi
done

# Dağıtım: symlink adı, ilk argüman, varsayılan.
version="$("$note" --version)"
check "symlink note --version ($version)" "$?" 0
"$appimage" note --version >/dev/null
check "first-arg note --version" "$?" 0

env -u APPIMAGE_EXTRACT_AND_RUN "$appimage" --appimage-extract-and-run note --version >/dev/null
check "--appimage-extract-and-run note --version" "$?" 0
for gui_args in "" "gui"; do
    # shellcheck disable=SC2086 -- boş değer "argümansız" demektir, kelime bölünmesi istenen davranış
    SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software timeout 2 "$appimage" $gui_args >/dev/null 2>&1
    check "AppImage ${gui_args:-without arguments} opens notes-gui" "$?" 124
done

"$note" "merhaba dünya" >/dev/null
check 'note "merhaba dünya"' "$?" 0
listing="$("$note" ls)"
check "note ls" "$?" 0
[[ "$listing" == *merhaba* ]] && verdict=yes || verdict=no
probe "note ls shows the new note" "$verdict"

# Çıkış kodu aynen geçer: bulunamayan not 1, geçersiz kullanım 2.
"$note" rm >/dev/null 2>&1
check "exit code passthrough (usage error)" "$?" 2
"$note" show bu-not-yok-zzz >/dev/null 2>&1
check "exit code passthrough (not found)" "$?" 1

# Symlink `note` iken `gui`/`note` ilk argümanı dağıtım değil, not metnidir.
"$note" gui >/dev/null
check 'note gui (argument stays note text)' "$?" 0
"$note" ls gui | grep -q gui && verdict=yes || verdict=no
probe "note gui created a note instead of opening the GUI" "$verdict"

# Sorgu: yazma, geri okuma, salt-okunur ret, LOAD CSV reddi. Ayrı ev: önceki adımların notları git denetimine karışmasın.
query_home="$work/query-home"
query_notes="$query_home/notes"
mkdir -p "$query_home/config" "$query_notes"
NOTEAPP_HOME="$query_home" "$note" config set notes_dir "$query_notes" >/dev/null
check "query: config set notes_dir" "$?" 0
NOTEAPP_HOME="$query_home" "$note" query --write 'CREATE (:Note {name:"a"})' >/dev/null
check "query --write CREATE" "$?" 0
query_read='MATCH (n:Note) RETURN n.name AS name'
query_output="$(NOTEAPP_HOME="$query_home" "$note" query "$query_read")"
check "query read" "$?" 0
# Tam eşleşme: başlık da `a` içerir, parça eşleşme satır dönmese de geçerdi.
[[ "$query_output" == $'name\na' ]] && verdict=yes || verdict=no
probe "query read returns the created node" "$verdict"
NOTEAPP_HOME="$query_home" "$note" query 'CREATE (:Note {name:"b"})' >/dev/null 2>&1
check "query read-only CREATE rejected" "$?" 2
query_output="$(NOTEAPP_HOME="$query_home" "$note" query "$query_read")"
[[ "$query_output" == $'name\na' ]] && verdict=yes || verdict=no
probe "query read-only CREATE wrote nothing" "$verdict"
query_error="$(NOTEAPP_HOME="$query_home" "$note" query "LOAD CSV FROM 'file:///etc/passwd' AS x RETURN x" 2>&1)"
check "query LOAD CSV rejected" "$?" 2
[[ "$query_error" == *LOAD* ]] && verdict=yes || verdict=no
probe "query LOAD CSV reports the clause" "$verdict"
NOTEAPP_HOME="$query_home" "$note" query --write "LOAD CSV FROM 'file:///etc/passwd' AS x RETURN x" >/dev/null 2>&1
check "query --write LOAD CSV rejected" "$?" 2

# PDF
note_path="$(find "$notes_dir" -type f -name '*merhaba*' -print -quit)"
[[ -n "$note_path" ]] && relative="${note_path#"$notes_dir"/}" || relative=""
"$note" export --pdf "$relative" -o "$work/out.pdf" >/dev/null
check "note export --pdf" "$?" 0
[[ "$(head -c 5 "$work/out.pdf" 2>/dev/null)" == "%PDF-" ]] && verdict=yes || verdict=no
probe "PDF header present" "$verdict"

# Çocuk süreç ortamı: editör paketli kütüphaneleri görmemeli.
cat >"$work/fake-editor" <<EOF
#!/bin/sh
env >"$work/child.env"
cat /proc/\$\$/maps >"$work/child.maps"
echo edited >>"\$1"
EOF
chmod +x "$work/fake-editor"
# SECRET_BACKEND ve SNAP_NAME: libsecret token'ı Secret Service yerine dosyaya yazabilir; AppRun ikisini de temizlemeli.
SECRET_BACKEND=file SNAP_NAME=noteapp-smoke EDITOR="$work/fake-editor" VISUAL="" "$note" -e "child env" >/dev/null
check "note -e with fake editor" "$?" 0
# Dosya yoksa editör hiç çalışmamıştır: `grep` başarısızlığı temiz sonuç sayılmasın.
[[ -s "$work/child.env" && -s "$work/child.maps" ]] && verdict=yes || verdict=no
probe "fake editor ran" "$verdict"
if grep -qE '^LD_(LIBRARY_PATH|PRELOAD)=' "$work/child.env"; then verdict=no; else verdict=yes; fi
probe "child has no LD_LIBRARY_PATH or LD_PRELOAD" "$verdict"
if grep -qE 'appimage_extracted|\.mount_' "$work/child.maps"; then verdict=no; else verdict=yes; fi
probe "child maps no bundled library" "$verdict"

if grep -q '^SECRET_BACKEND=' "$work/child.env"; then verdict=no; else verdict=yes; fi
probe "AppRun unsets SECRET_BACKEND" "$verdict"
if grep -q '^SNAP_NAME=' "$work/child.env"; then verdict=no; else verdict=yes; fi
probe "AppRun unsets SNAP_NAME" "$verdict"

# /.flatpak-info ortamdan silinemez, AppRun çalışmayı reddeder. Dosyayı yaratmak root ister (konteyner varsayılanı).
: >/.flatpak-info
flatpak_output="$("$note" --version 2>&1)"
check "AppRun refuses to run under /.flatpak-info" "$?" 1
rm -f /.flatpak-info
[[ "$flatpak_output" == *"refusing to run inside Flatpak"* ]] && verdict=yes || verdict=no
probe "AppRun explains the Flatpak refusal" "$verdict"

# README'nin FUSE'suz güvenli yolu: özel dizine açılan paketin AppRun'ı.
"$squash/squashfs-root/AppRun" note --version >/dev/null
check "extracted AppRun note --version" "$?" 0

# GUI: başsız; SIGTERM'de gui-state.toml yazılır.
SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software timeout "$timeout_seconds" "$work/notes-gui" >"$work/gui.log" 2>&1
check "notes-gui dummy driver" "$?" 124
# extract-and-run'da timeout çalışma zamanını sonlandırır, çocuk SIGTERM'i işlerken döner; yazmasına süre tanı.
for _ in 1 2 3 4 5 6 7 8 9 10; do
    state_file="$(find "$NOTEAPP_HOME" -name gui-state.toml -print -quit)"
    [[ -n "$state_file" ]] && break
    sleep 0.5
done
[[ -s "$state_file" ]] && verdict=yes || { verdict=no; cat "$work/gui.log"; }
probe "gui-state.toml written on SIGTERM" "$verdict"

# Arka plan senkronu: GUI, `note`u kendi yanında bulup başlatmalı. Yerel kapalı porta bağlanır, GitHub'a dokunmaz.
# Spawn hatası da log'a düşerdi; bağlantı hatası görmek çocuğun gerçekten koştuğunu kanıtlar.
sync_home="$work/sync-home"
mkdir -p "$sync_home/config"
export NOTEAPP_GIT_TOKEN=placeholder-not-a-real-token
NOTEAPP_HOME="$sync_home" "$note" config set notes_dir "$work/sync-notes" >/dev/null
NOTEAPP_HOME="$sync_home" "$note" config set sync.remote "https://127.0.0.1:9/none.git" >/dev/null
NOTEAPP_HOME="$sync_home" "$note" config set sync.enabled true >/dev/null
NOTEAPP_HOME="$sync_home" "$note" sync status 2>&1 | grep -q 'Last error' && verdict=no || verdict=yes
probe "background sync: no error recorded before the GUI runs" "$verdict"
NOTEAPP_HOME="$sync_home" SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software \
    timeout "$timeout_seconds" "$work/notes-gui" >/dev/null 2>&1
status=$?
sleep 1
sync_status="$(NOTEAPP_HOME="$sync_home" "$note" sync status 2>&1)"
echo "   [background sync] gui exit $status: $(tr '\n' ' ' <<<"$sync_status" | cut -c1-180)"
sync_log="$(cat "$sync_home/state/log.txt" 2>/dev/null)"
if grep -qiE 'could not be started|not found|spawn' <<<"$sync_log"; then verdict=no; else verdict=yes; fi
probe "background sync: no spawn failure" "$verdict"
grep -q 'Last error' <<<"$sync_status" && verdict=yes || verdict=no
probe "background sync: note inside the package ran and recorded a result" "$verdict"

# TLS: paketli libgit2 + OpenSSL. Depo ve config her denemede ayrı: başarı önceki denemeden sızmasın.
# `sync init` token'ı erkenden çözer ve kasasız konteynerde durur; yer tutucu ortam token'ı bunu aşar. Herkese açık
# depoda klonlama kimlik istemez, yani token gönderilmez.
tls_try() {
    local name="$1"
    shift
    local home="$work/tls-$name"
    mkdir -p "$home/config"
    NOTEAPP_HOME="$home" "$note" config set notes_dir "$home/notes" >/dev/null
    tls_output="$(env NOTEAPP_HOME="$home" "$@" "$note" sync init "$tls_url" 2>&1)"
    tls_status=$?
    echo "   [$name] exit $tls_status: $(head -n 2 <<<"$tls_output" | tr '\n' ' ' | cut -c1-200)"
}

if [[ -n "${SMOKE_SKIP_TLS:-}" ]]; then
    echo "SKIP TLS"
else
    has_host_ca=no
    for candidate in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt; do
        [[ -s "$candidate" ]] && has_host_ca=yes
    done
    # CA'sız temiz imaj: doğrulama kapanmaz, bağlantı sertifika hatasıyla düşer.
    if [[ "$has_host_ca" == no ]]; then
        tls_try no-host-ca
        check "TLS: no host CA fails" "$tls_status" 4
        grep -qi 'certificate' <<<"$tls_output" && verdict=yes || verdict=no
        probe "TLS: no host CA reports a certificate error" "$verdict"
    fi
fi

# Buradan sonrası paket ister: kütüphane testleri bitti, kurulum onları etkilemez.
install_test_tools() {
    if command -v dnf >/dev/null; then
        dnf install -y -q util-linux-script ca-certificates git-core >/dev/null 2>&1
    elif command -v apt-get >/dev/null; then
        apt-get update -qq >/dev/null 2>&1 &&
            DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends ca-certificates git >/dev/null 2>&1
    fi
}
install_test_tools
check "install test tools" "$?" 0

# Grafik sorgusu not dosyası üretmemeli. Yeni not da görünmeli: temiz sonuç, denetim körlüğünden gelmesin.
if command -v git >/dev/null; then
    git -C "$query_notes" init -q
    check "query: git init notes_dir" "$?" 0
    git_status="$(git -C "$query_notes" status --porcelain)"
    check "query: git status" "$?" 0
    [[ -z "$git_status" ]] && verdict=yes || { verdict=no; echo "$git_status"; }
    probe "query left no new note files" "$verdict"
    NOTEAPP_HOME="$query_home" "$note" "sorgu denetimi" >/dev/null
    check "query: control note" "$?" 0
    git_status="$(git -C "$query_notes" status --porcelain)"
    # G4: not graph.db'dedir; notes_dir temiz kalır, not listeden görünür.
    [[ -z "$git_status" && "$(NOTEAPP_HOME="$query_home" "$note" ls)" == *sorgu-denetimi* ]] && verdict=yes || verdict=no
    probe "a real new note reaches the graph and leaves notes_dir clean" "$verdict"
else
    echo "FAIL query git status: git missing in this image"
    failures=$((failures + 1))
fi

# TUI: gerçek terminal ister.
if command -v script >/dev/null; then
    TERM=xterm script -qec "timeout $timeout_seconds $work/notes" /dev/null >/dev/null 2>&1
    check "notes under pty" "$?" 124
else
    echo "FAIL notes: script(1) missing in this image"
    failures=$((failures + 1))
fi

if [[ -z "${SMOKE_SKIP_TLS:-}" ]]; then
    tls_try default
    check "TLS: sync init against $tls_url" "$tls_status" 0
    [[ -f "$work/tls-default/notes/README.md" ]] && verdict=yes || verdict=no
    probe "TLS: repository content really cloned" "$verdict"
    if grep -qi 'certificate' <<<"$tls_output"; then verdict=no; else verdict=yes; fi
    probe "TLS: no certificate error" "$verdict"

    tls_try closed SSL_CERT_FILE=/dev/null
    check "TLS: SSL_CERT_FILE=/dev/null fails" "$tls_status" 4
    grep -qi 'certificate' <<<"$tls_output" && verdict=yes || verdict=no
    probe "TLS: failure is a certificate error" "$verdict"
fi

echo "== failures: $failures"
[[ "$failures" -eq 0 ]]
