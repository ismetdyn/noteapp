#!/usr/bin/env bash
# Linux'ta konteynerde Windows ikilileri üretir: out/windows-x86_64/{note,notes,notes-gui}.exe ve licenses/. Host'ta yalnız
# docker ya da podman gerekir. Kaynak salt-okunur bağlanır; derleme dizini ve FetchContent önbelleği adlı birimde kalır.
#   scripts/build-windows.sh                 ikilileri üretir (testler derlenmez)
#   scripts/build-windows.sh --test [ctest argümanları]   önce testleri Wine'da koşar, sonuçtan bağımsız ikilileri üretip DLL denetimini
#                                                         de koşar; test düştüyse sonda sıfırdan farklı çıkar
# NOTEAPP_WINDOWS_BUILD_VOLUME: derleme birimi; yeni ad = sıfırdan derleme.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/container-engine.sh

die() { echo "ERROR: $*" >&2; exit 1; }

image=noteapp-windows-cross
volume="${NOTEAPP_WINDOWS_BUILD_VOLUME:-noteapp-windows-build}"
output_dir=out/windows-x86_64
run_tests=0
if [[ "${1:-}" == "--test" ]]; then
    run_tests=1
    shift
fi

# Çıktı konteynerden tar akışıyla çıkar: dosyalar her motorda (root'lu docker dahil) host kullanıcısına ait olur.
# Derleme çıktısı stderr'e gider, stdout yalnız tar akışıdır.
"$engine" build -t "$image" -f docker/windows-cross.Containerfile docker >&2
mkdir -p "$output_dir"
rm -rf "$output_dir/licenses"
"$engine" run --rm -i \
    -v "$(pwd):/src:ro,z" \
    -v "$volume:/build" \
    -e "RUN_TESTS=$run_tests" \
    "$image" bash -c '
set -euo pipefail
exec 3>&1 1>&2

die() { echo "ERROR: $*" >&2; exit 1; }

start_wine() {
    # Mono/Gecko indirme istemi başsız kabukta askıda kalmasın; test komutları cwd Z: üzerindeyken /build yolunu çözer.
    export WINEPREFIX=/build/wine-prefix WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
    # Kalıcı wineserver: yoksa ilk test explorer.exe başlatır, o da ctest çıktı borusunu tutar ve ctest askıda kalır.
    # Boş birimde önek henüz yok; wineserver önek dizinine geçemezse çıkar.
    mkdir -p "$WINEPREFIX"
    wineserver -p
    wineboot -i
    # Wine host ortamındaki SDL_* değişkenlerini Windows sürecine geçirmez; ctest ortamı etkisiz kalır, kayıt defterinden gelir.
    wine reg add "HKCU\\Environment" /f /v SDL_VIDEO_DRIVER /d dummy
    wine reg add "HKCU\\Environment" /f /v SDL_RENDER_DRIVER /d software
}

tests_failed=0
if [[ "$RUN_TESTS" == 1 ]]; then
    start_wine
    cmake --preset windows-cross-release -B /build/test
    cmake --build /build/test
    # Düşen test DLL denetimini gizlemesin: sonuç en sonda, tar akışından sonra çıkış kodu olur.
    ctest --test-dir /build/test --output-on-failure "$@" || tests_failed=1
    wineserver -k
fi

cmake --preset windows-cross-release -B /build/release -DNOTEAPP_BUILD_TESTS=OFF
cmake --build /build/release
rm -rf /tmp/install
cmake --install /build/release --prefix /tmp/install --strip

# Yalnız Windows sistem DLLleri kabul edilir: başka bir DLL "kopyala-çalıştır"ı bozar. crypt32, iphlpapi, version, winhttp,
# winmm bilerek listede yok: KnownDLLs dışındadırlar, içe aktarma tablosundaysalar exe yanındaki kopya main öncesi yüklenir
# (cmake/toolchains/mingw-delay-load.cmake).
system_dlls="^(advapi32|bcrypt|gdi32|imm32|kernel32|msvcrt|ole32|oleaut32|rpcrt4|secur32|setupapi|shell32|shlwapi|user32|ws2_32)\.dll$"
# PE biçimi: IMAGE_LOAD_CONFIG_DIRECTORY64.DependentLoadFlags 78. baytta; küçük uçlu 0x0800 = LOAD_LIBRARY_SEARCH_SYSTEM32.
dependent_load_flags_offset=78
for exe in /tmp/install/bin/note.exe /tmp/install/bin/notes.exe /tmp/install/bin/notes-gui.exe; do
    headers="$(x86_64-w64-mingw32-objdump -p "$exe")"
    dlls="$(sed -n "s/^.*DLL Name: //p" <<<"$headers" | tr "[:upper:]" "[:lower:]" | sort)"
    echo "$(basename "$exe") imports:" $dlls
    foreign="$(grep -Ev "$system_dlls" <<<"$dlls" || true)"
    if [[ -n "$foreign" ]]; then
        echo "ERROR: $exe imports non-system or hijackable DLLs:" $foreign
        exit 1
    fi

    image_base="$(sed -nE "s/^ImageBase\s+([0-9a-f]+).*/\1/p" <<<"$headers")"
    load_config_rva="$(sed -nE "s/^Entry a +([0-9a-f]+) .*/\1/p" <<<"$headers")"
    address=$((16#$image_base + 16#$load_config_rva + dependent_load_flags_offset))
    flags="$(x86_64-w64-mingw32-objdump -s --start-address="$address" --stop-address="$((address + 2))" "$exe" \
        | sed -nE "s/^ +[0-9a-f]+ ([0-9a-f]{4}) .*/\1/p")"
    echo "$(basename "$exe") DependentLoadFlags bytes: $flags"
    [[ "$flags" == 0008 ]] || die "$exe: DependentLoadFlags is not LOAD_LIBRARY_SEARCH_SYSTEM32"
done

assert_no_planted_dll() {
    local planted
    planted="$(find . -name "*.planted" | sort | tr "\n" " ")"
    [[ -z "$planted" ]] || die "$1: planted DLLs ran: $planted"
}

# $1 kendi sürecinde $2.dlli System32den yüklemiş olmalı: yoksa sahte DLL hiç sınanmamıştır ("iz yok" bir şey kanıtlamaz).
# Süreç kimliği +pid izinden gelir: alt süreçlerin (conhost, note.exe) yüklemesi bu ikiliyi kanıtlamaz.
assert_delay_loaded() {
    local exe="$1" dll="$2" log="$3" pid
    assert_no_planted_dll "$exe"
    pid="$(sed -nE "s/^([0-9a-f]+):[0-9a-f]+:trace:loaddll:.*Loaded L\"[^\"]*\\\\$exe\".*/\1/p" "$log" | head -n 1)"
    [[ -n "$pid" ]] || die "$exe: process not found in the Wine loader trace"
    grep -aqiE "^$pid:[0-9a-f]+:trace:loaddll:.*Loaded L\"C:.*system32.*$dll\.dll\"" "$log" ||
        die "$exe did not load $dll.dll from System32: the planted DLL was not exercised"
    echo "$exe: $dll.dll loaded from System32 in its own process, no planted DLL ran"
}

# Exe yanına bırakılan sahte DLLler: DllMain "<dll>.planted" yazar. Prob aynı beş DLLi çağırır; kısıtsız kipi kontrol
# grubudur (sahteler gerçekten yüklenebiliyor mu), kısıtlı kipi ve gerçek ikililer hiç iz bırakmamalıdır.
check_dll_planting() {
    local work=/tmp/dll-planting planted gui_run_seconds=10 tui_run_seconds=15
    rm -rf "$work"
    mkdir -p "$work"
    cp /tmp/install/bin/*.exe "$work"
    cat >"$work/fake.c" <<EOF
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID unused) {
    (void)unused;
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH + 16];
        DWORD length = GetModuleFileNameA(self, path, MAX_PATH);
        lstrcpyA(path + length, ".planted");
        CloseHandle(CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
    }
    return TRUE;
}
int stub(void) { return 0; }
EOF
    cat >"$work/probe.cpp" <<EOF
#include "core/dll_search.hpp"
#include <iphlpapi.h>
#include <timeapi.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <winver.h>
int main(int argc, char**) {
    if (argc == 1)
        noteapp::core::restrict_dll_search_to_system32();
    GetFileVersionInfoSizeW(L"x", nullptr);
    timeGetTime();
    WinHttpCheckPlatform();
    CertCloseStore(nullptr, 0);
    ULONG interfaces = 0;
    GetNumberOfInterfaces(&interfaces);
    return 0;
}
EOF
    local delay_dir=/build/release/delay-load dll
    x86_64-w64-mingw32-g++ -std=c++23 -I/src/src -static "$work/probe.cpp" -o "$work/probe.exe" \
        -L"$delay_dir" -lversion -lwinmm -lwinhttp -lcrypt32 -liphlpapi -ldelayimp
    for dll in version winmm winhttp crypt32 iphlpapi; do
        sed "2,\$s/\$/=stub/" "$delay_dir/$dll.def" >"$work/$dll-fake.def"
        x86_64-w64-mingw32-gcc -shared -o "$work/$dll.dll" "$work/fake.c" "$work/$dll-fake.def"
    done

    start_wine
    cd "$work"
    wine probe.exe --unrestricted
    planted="$(find . -name "*.planted" | sort | tr "\n" " ")"
    echo "control, unrestricted search: $planted"
    [[ "$(wc -w <<<"$planted")" -eq 5 ]] || die "control failed: Wine did not load the planted DLLs, the check proves nothing"
    rm -f ./*.planted

    wine probe.exe
    wine note.exe --version
    assert_no_planted_dll "probe.exe and note.exe --version"

    export NOTEAPP_HOME="Z:$work/home" NOTEAPP_GIT_TOKEN=placeholder NOTEAPP_GIT_TOKEN_HOST=127.0.0.1
    local remote=https://127.0.0.1:9/none.git status=0
    wine note.exe config set notes_dir "Z:$work/notes"
    wine note.exe config set sync.remote "$remote"
    # notes.exe ve notes-gui.exe açılışta güncelleme denetler: kapı ağa çıkmasın, yüklenen DLL yalnız akıştan gelsin.
    wine note.exe config set update.check_interval 0

    # note.exe: kapalı porta bağlanmak WinHTTPyi yükler. Kod 4: bağlantı hatası, yani akış ağa kadar gitti.
    WINEDEBUG=+loaddll,+pid wine note.exe sync init "$remote" >/dev/null 2>"$work/note.log" || status=$?
    [[ "$status" -eq 4 ]] || die "note.exe sync init: expected exit 4, got $status"
    assert_delay_loaded note.exe winhttp "$work/note.log"

    # notes-gui.exe: SDL zamanlayıcısı açılışta winmm yükler; kayıt defterindeki dummy sürücü ekran istemez.
    # Timeout kodu (124): süreç çökmeden açık kaldı.
    status=0
    WINEDEBUG=+loaddll,+pid timeout "$gui_run_seconds" wine notes-gui.exe >/dev/null 2>"$work/gui.log" || status=$?
    [[ "$status" -eq 124 ]] || die "notes-gui.exe: expected to keep running until timeout, got exit $status"
    assert_delay_loaded notes-gui.exe winmm "$work/gui.log"

    # notes.exe açılışta gecikmeli DLLe ulaşmaz; arka plan senkronu da ayrı note.exe sürecidir ve bu ikilinin kısıtını
    # kanıtlamaz. Kendi sürecinde WinHTTPye giden tek yol Ayarlar kaydıdır (SettingsState::submit -> SyncService::enable).
    # Tuşlar WriteConsoleInput ile gider: Wine konsolu bir terminalden gelen kaçış dizisini uChar=0 olan tuşa çevirir, FTXUI
    # ise yalnız uChar okur; bu yüzden ok ve F tuşu yok, `,` ve Tab. Wine pencere sürücüsü olmadan konsolu yalnız pty üzerinde
    # açar. Satır sayıları: src/core/config.cpp settings() (14) + token + bağlantı testi; sıra değişirse kapı kırmızı olur.
    cat >"$work/tui-driver.c" <<"EOF"
#include <windows.h>
#include <stdio.h>
int main(void) {
    static const char keys[] = ",\t\t\t\t\t\t\t \t\t\t\t\t\t\t\t\r"; /* Ayarlar, sync.enabled, boşluk, bağlantı testi, Enter */
    INPUT_RECORD records[sizeof keys - 1] = {0};
    for (size_t i = 0; i < sizeof keys - 1; ++i) {
        records[i].EventType = KEY_EVENT;
        records[i].Event.KeyEvent.bKeyDown = TRUE;
        records[i].Event.KeyEvent.wRepeatCount = 1;
        records[i].Event.KeyEvent.uChar.UnicodeChar = (unsigned char)keys[i];
    }
    HANDLE input = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
    STARTUPINFOA startup = {sizeof startup};
    PROCESS_INFORMATION app;
    char command[] = "notes.exe";
    DWORD written;
    /* Tek çağrı: kuyruk açılışı bekler. */
    if (input == INVALID_HANDLE_VALUE || !CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &app) ||
        !WriteConsoleInputW(input, records, sizeof keys - 1, &written) || written != sizeof keys - 1) {
        fprintf(stderr, "tui-driver: error %lu\n", GetLastError());
        return 2;
    }
    /* Bağlantı reddi TUIde hata satırıdır: süreç açık kalmalı. */
    if (WaitForSingleObject(app.hProcess, TUI_RUN_MILLISECONDS) != WAIT_TIMEOUT) {
        fprintf(stderr, "tui-driver: notes.exe exited during the settings flow\n");
        return 3;
    }
    TerminateProcess(app.hProcess, 0);
    return 0;
}
EOF
    x86_64-w64-mingw32-gcc -static -DTUI_RUN_MILLISECONDS="$((tui_run_seconds * 1000))" "$work/tui-driver.c" -o tui-driver.exe
    command -v python3 >/dev/null || die "python3 is required to give Wine a pty"
    WINEDEBUG=+loaddll,+pid python3 -c "import os, pty, sys; sys.exit(os.waitstatus_to_exitcode(pty.spawn(sys.argv[1:])))" \
        bash -c "exec wine tui-driver.exe 2>tui.log" </dev/null >/dev/null ||
        die "tui-driver failed: $(grep -a "tui-driver:" tui.log)"
    assert_delay_loaded notes.exe winhttp "$work/tui.log"

    unset NOTEAPP_HOME NOTEAPP_GIT_TOKEN NOTEAPP_GIT_TOKEN_HOST
    echo "planted DLLs: none loaded (probe, note, notes, notes-gui); each exe loaded a delay-loaded DLL from System32"
    wineserver -k
    cd /
}
# Wine süreçleri tar akışını (fd 3) miras alıp açık tutmasın.
check_dll_planting 3>&-

licenses=/tmp/install/bin/licenses
source /src/packaging/linux/third-party-licenses.sh
add_static_component_licenses /build/release/_deps
# Statik bağlanan MinGW çalışma zamanı: paketlerin kendi lisans dosyaları.
add_license winpthreads /usr/share/licenses/mingw64-winpthreads/COPYING
add_license mingw-w64-crt /usr/share/licenses/mingw64-crt/COPYING
add_license mingw-w64-crt /usr/share/licenses/mingw64-crt/DISCLAIMER
add_license mingw-w64-crt /usr/share/licenses/mingw64-crt/DISCLAIMER.PD
add_license libstdc++ /usr/share/licenses/mingw64-gcc/COPYING3
add_license libstdc++ /usr/share/licenses/mingw64-gcc/COPYING.RUNTIME

tar -C /tmp/install/bin -cf - note.exe notes.exe notes-gui.exe licenses >&3
if [[ "$tests_failed" == 1 ]]; then
    die "tests failed under Wine (the DLL checks above ran regardless)"
fi
' build-windows "$@" | tar -xf - -C "$output_dir" --no-same-owner

for exe in note.exe notes.exe notes-gui.exe; do
    [[ -s "$output_dir/$exe" ]] || die "$output_dir/$exe was not produced"
done
for license in noteapp-icons/LICENSE-NoteappIcons.txt inter/LICENSE-Inter.txt jetbrains-mono/LICENSE-JetBrainsMono.txt \
    sdl3/LICENSE.txt sdl3/SOURCE-NOTICES.txt libgit2/COPYING libgit2/SOURCE-NOTICES.txt \
    libharu/SOURCE-NOTICES.txt curl/COPYING curl/SOURCE-NOTICES.txt mupdf/COPYING mupdf/SOURCE-NOTICES.txt \
    winpthreads/COPYING libstdc++/COPYING.RUNTIME; do
    [[ -s "$output_dir/licenses/$license" ]] || die "$output_dir/licenses/$license was not produced"
done
ls -l "$output_dir"
