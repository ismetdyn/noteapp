#!/usr/bin/env bash
# docker/appimage.Containerfile imajında çalışır; scripts/build-appimage.sh başlatır. Bağlama noktaları: /src (salt
# okunur kaynak), /build (kalıcı derleme dizini), /out (çıktı).
set -euo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
stage="$work/stage"
appdir="$work/AppDir"
packaging=/src/packaging/linux

# -static-libstdc++/-static-libgcc: excludelist libstdc++ ve libgcc_s'i paketlemeyi yasaklar, host'unkine bağlanmak da
# GLIBCXX sürümünü g++-14'e kilitlerdi. Paketli kütüphanelerin hiçbiri C++ değil, ABI sınırı oluşmaz.
cmake --preset linux-gcc-release -B /build \
    -DCMAKE_C_COMPILER=gcc-14 \
    -DCMAKE_CXX_COMPILER=g++-14 \
    -DNOTEAPP_BUILD_TESTS=OFF \
    "-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc"
cmake --build /build
cmake --install /build --prefix "$stage" --strip

# SDL3 X11 ve Wayland'i çalışma zamanında dlopen eder; dev paketi eksikse derleme sessizce arka ucu düşürürdü.
sdl_config="$(find /build/_deps/sdl3-build -name SDL_build_config.h -print -quit)"
[[ -n "$sdl_config" ]] || die "SDL_build_config.h not found"
for define in SDL_VIDEO_DRIVER_X11_DYNAMIC SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC \
    SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR HAVE_DBUS_DBUS_H; do
    grep -q "^#define $define\b" "$sdl_config" || die "SDL3 built without $define"
done

version_output="$("$stage/bin/note" --version)"
version="${version_output#note }"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "unexpected note --version output: $version_output"

# Excludelist libgpg-error'u host'tan bekler ama temiz debian:13'te yok (paketli libgcrypt ister); libgcrypt ile aynı
# kaynaktan paketlenir, RUNPATH ikisini eşleştirir.
gpg_error="$(gcc-14 -print-file-name=libgpg-error.so.0)"
[[ -e "$gpg_error" ]] || die "libgpg-error.so.0 not found"

# İkon tek kaynaktan (resources/icons, scripts/generate-app-icon.sh): linuxdeploy boyutu dosyadan okuyup
# usr/share/icons/hicolor/<boyut>/apps altına koyar; dosya adı .desktop'taki Icon=noteapp ile eşleşmeli.
icon_args=(--icon-file /src/resources/icons/noteapp.svg)
for icon in /src/resources/icons/hicolor/*/apps/noteapp.png; do
    icon_args+=(--icon-file "$icon")
done

# NO_STRIP: linuxdeploy'un gömülü strip'i yeni ELF bölümlerini tanımıyor; ikililer yukarıda --strip ile soyuldu.
NO_STRIP=1 /tools/linuxdeploy --appdir "$appdir" \
    --library "$gpg_error" \
    --executable "$stage/bin/note" \
    --executable "$stage/bin/notes" \
    --executable "$stage/bin/notes-gui" \
    --desktop-file "$packaging/noteapp.desktop" \
    "${icon_args[@]}" \
    --custom-apprun "$packaging/AppRun"
# Paket sözleşmesi: her kaynak boyut hicolor'da aynı yerde; masaüstü .desktop'taki Icon=noteapp'i buradan çözer.
for icon in /src/resources/icons/hicolor/*/apps/noteapp.png /src/resources/icons/noteapp.svg; do
    relative="${icon#/src/resources/icons/}"
    if [[ "$relative" == noteapp.svg ]]; then
        relative=hicolor/scalable/apps/noteapp.svg
    fi
    [[ -f "$appdir/usr/share/icons/$relative" ]] || die "icon missing in AppDir: usr/share/icons/$relative"
done

# Yerel kurulumun güncellemesi (app::install_update) paketteki betiği çalıştırır: kurulum mantığı tek kaynakta kalır.
# Betik menü şablonunu yanında arar. Yol sözleşmesi: src/app/update_install.cpp kInstallScript.
install -D -m 755 /src/scripts/install-local.sh "$appdir/usr/share/noteapp/install-local.sh"
install -m 644 "$packaging/noteapp.desktop" "$appdir/usr/share/noteapp/noteapp.desktop"

# Lisanslar: gömülü bileşenler kendi kaynağından, paylaşımlı kütüphaneler Debian copyright dosyasından.
licenses="$appdir/usr/share/licenses/noteapp"
source "$packaging/third-party-licenses.sh"
add_static_component_licenses /build/_deps
add_license keychain /build/_deps/keychain-src/LICENSE
# AppImage çalışma zamanı ve ona statik bağlananlar; değiştirme yolu NOTICE'ta.
add_license appimage-type2-runtime /tools/LICENSE-type2-runtime
add_license appimage-type2-runtime "$packaging/runtime-NOTICE.txt"
for runtime_license in libfuse-LICENSE libfuse-LGPL2.txt libfuse-mount.c.diff squashfuse-LICENSE zstd-LICENSE zlib-LICENSE \
    mimalloc-LICENSE musl-COPYRIGHT; do
    add_license appimage-type2-runtime "/tools/runtime-licenses/$runtime_license"
done
# Statik bağlanan libstdc++ ve libgcc: GCC Runtime Library Exception.
add_license libstdc++ /usr/share/doc/libstdc++-14-dev/copyright

while IFS= read -r library; do
    owner="$(dpkg -S "*/${library##*/}" | head -n 1)" || die "no package owns ${library##*/}"
    package="${owner%%:*}"
    add_license "$package" "/usr/share/doc/$package/copyright"
done < <(find "$appdir/usr/lib" -maxdepth 1 -name '*.so*' -type f)
# Debian copyright dosyaları LGPL/Apache tam metnine buradan atıf yapar.
for common_license in Apache-2.0 GPL-2 GPL-3 LGPL-2 LGPL-2.1 LGPL-3 MPL-2.0; do
    add_license common-licenses "/usr/share/common-licenses/$common_license"
done

# Paket sözleşmesi: RUNPATH ve glibc tabanı ikililerden okunur, varsayılmaz.
# usr/bin → $ORIGIN/../lib, usr/lib → $ORIGIN; başka değer paket dışından kütüphane çözebilir. ELF olmayan dosya atlanır.
elf_count=0
check_runpaths() {
    local directory="$1" expected="$2" file actual
    while IFS= read -r -d '' file; do
        readelf -h "$file" >/dev/null 2>&1 || continue
        actual="$(readelf -d "$file" | sed -nE 's/.*\((RPATH|RUNPATH)\).*\[(.*)\]/\1=\2/p')"
        [[ "$actual" == "RUNPATH=$expected" ]] || die "${file#"$appdir"/}: expected RUNPATH=$expected, found '$actual'"
        echo "  ${file#"$appdir"/}: $actual"
        elf_count=$((elf_count + 1))
    done < <(find "$directory" -type f -print0)
}
echo "RUNPATH:"
check_runpaths "$appdir/usr/bin" '$ORIGIN/../lib'
check_runpaths "$appdir/usr/lib" '$ORIGIN'
[[ "$elf_count" -ge 3 ]] || die "RUNPATH check saw only $elf_count ELF files"
echo "Bundled libraries:"
find "$appdir/usr/lib" -maxdepth 1 -name '*.so*' -printf '  %f\n' | sort
minimum_glibc="$(find "$appdir/usr/bin" "$appdir/usr/lib" -type f -name '*' -exec sh -c \
    'readelf -V --wide "$1" 2>/dev/null | grep -o "GLIBC_[0-9.]*"' _ {} \; | sort -Vu | tail -n 1)"
echo "Minimum glibc: ${minimum_glibc#GLIBC_}"

artifact="noteapp-$version-x86_64.AppImage"
ARCH=x86_64 VERSION="$version" /tools/appimagetool --no-appstream --runtime-file /tools/runtime-x86_64 \
    "$appdir" "$work/$artifact"

# Eski sürümün dosyası kalmasın: out/'ta tek çıktı. Docker kökten çalışırsa sahibi kaynağın sahibi olsun.
rm -f /out/noteapp-*-x86_64.AppImage
install -m 755 -o "$(stat -c %u /src)" -g "$(stat -c %g /src)" "$work/$artifact" "/out/$artifact"
echo "Artifact: out/$artifact ($(stat -c %s "/out/$artifact") bytes)"
