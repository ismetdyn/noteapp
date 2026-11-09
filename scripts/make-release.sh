#!/usr/bin/env bash
# GitHub release varlıklarını üretir (sözleşme: SPEC.md §4 "Release sözleşmesi"; kendi kendini güncelleme buna dayanır).
# Girdi: scripts/build-appimage.sh ve scripts/build-windows.sh çıktıları (out/). Çıktı out/release/<sürüm>/:
#   noteapp-<sürüm>-x86_64.AppImage, noteapp-<sürüm>-windows-x86_64.zip (kökte üç exe, noteapp-package.txt ve
#   licenses/), mupdf-<MuPDF sürümü>-source.tar.gz (cmake/Dependencies.cmake), SHA256SUMS
# Yüklemez: tag v<sürüm> ve yükleme elle. Yeniden çalıştırmak dizini baştan üretir.
# Kullanım: scripts/make-release.sh <X.Y.Z>
set -euo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }

[[ $# -eq 1 ]] || die "usage: $0 <X.Y.Z>"
version="$1"
# Updater yalnız vX.Y.Z etiketini ve bu adları kabul eder (src/app/updater.cpp).
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "version must be X.Y.Z: $version"
for tool in zip sha256sum; do
    command -v "$tool" >/dev/null || die "$tool is required"
done

cd "$(dirname -- "$0")/.."
appimage_name="noteapp-$version-x86_64.AppImage"
zip_name="noteapp-$version-windows-x86_64.zip"
appimage="out/$appimage_name"
windows_dir="out/windows-x86_64"
release_dir="out/release/$version"
windows_entries=(note.exe notes.exe notes-gui.exe licenses)
# Sahiplik işareti: güncelleme yalnız bunu taşıyan dizini kurulum sayar (src/app/update_install.hpp
# kWindowsPackageMarker). Sabit tarih: aynı girdiyle yeniden üretilen zip bayt bayt aynıdır.
marker_name=noteapp-package.txt
readonly marker_date="2000-01-01T00:00:00Z"

[[ -f "$appimage" && -x "$appimage" ]] || die "missing $appimage (run scripts/build-appimage.sh)"
for entry in "${windows_entries[@]}"; do
    [[ -e "$windows_dir/$entry" ]] || die "missing $windows_dir/$entry (run scripts/build-windows.sh)"
done
[[ -d "$windows_dir/licenses" ]] || die "$windows_dir/licenses is not a directory"
# Kurulum açılan pakette bağı reddeder (src/app/update_install.cpp): pakete hiç girmesin.
[[ -z "$(find "${windows_entries[@]/#/$windows_dir/}" -type l)" ]] || die "$windows_dir has symbolic links"

# MuPDF AGPL-3.0: ikililerle birlikte karşılık gelen kaynak da sunulur (GPLv3 §6(d)). Adres ve SHA256 tek kaynaktan,
# cmake/Dependencies.cmake'teki FetchContent_Declare(mupdf ...) bloğundan okunur; release tam o arşivi taşır.
mupdf_pin="$(sed -n '/^FetchContent_Declare(mupdf$/,/)/p' cmake/Dependencies.cmake)"
mupdf_url="$(sed -n 's/^ *URL \(https:\/\/[^ ]*\.tar\.gz\)$/\1/p' <<<"$mupdf_pin")"
mupdf_sha256="$(sed -n 's/^ *URL_HASH SHA256=\([0-9a-f]\{64\}\))\{0,1\}$/\1/p' <<<"$mupdf_pin")"
[[ -n "$mupdf_url" && "$mupdf_url" != *$'\n'* ]] || die "cannot read the MuPDF URL from cmake/Dependencies.cmake"
[[ "$mupdf_sha256" =~ ^[0-9a-f]{64}$ ]] || die "cannot read the MuPDF SHA256 from cmake/Dependencies.cmake"
mupdf_name="${mupdf_url##*/}"

mkdir -p out/release
staging="$(mktemp -d "out/release/.$version.XXXXXX")"
cleanup() { rm -rf -- "$staging"; }
trap cleanup EXIT

staging="$(realpath -- "$staging")"

# Yayımlanan AppImage tam bu sürümü bildirmeli: güncelleme --expect-version ile başkasını reddeder.
appimage_check="$staging/appimage-check"
mkdir -- "$appimage_check"
appimage_path="$(realpath -- "$appimage")"
(cd "$appimage_check" && "$appimage_path" --appimage-extract >/dev/null) ||
    die "cannot extract $appimage"
reported="$("$appimage_check/squashfs-root/AppRun" note --version)" || die "$appimage cannot report its version"
[[ "$reported" == "note $version" ]] || die "$appimage reports '$reported', expected 'note $version'"
rm -rf -- "$appimage_check"

cp -- "$appimage" "$staging/$appimage_name"
# -X: işletim sistemine özgü ek öznitelik yok; yalnız adı verilen girdiler, kökte.
(cd "$windows_dir" && zip -q -r -X "$staging/$zip_name" "${windows_entries[@]}")
marker_dir="$staging/marker"
mkdir -- "$marker_dir"
printf '%s\n' "noteapp $version for Windows. noteapp updates replace note.exe, notes.exe, notes-gui.exe," \
    "this file and licenses/ in this folder; nothing else here is touched." >"$marker_dir/$marker_name"
touch -d "$marker_date" -- "$marker_dir/$marker_name"
(cd "$marker_dir" && zip -q -X "$staging/$zip_name" "$marker_name")
rm -rf -- "$marker_dir"

# Önce yerel derlemenin FetchContent önbelleği (build/<preset>/_deps), yoksa sabit adresten indirilir. Her iki yolda
# da yayımlanan kopya sabit SHA256 ile doğrulanır.
shopt -s nullglob
mupdf_cached=(build/*/_deps/mupdf-subbuild/mupdf-populate-prefix/src/"$mupdf_name")
shopt -u nullglob
if [[ ${#mupdf_cached[@]} -gt 0 ]]; then
    echo "MuPDF source: ${mupdf_cached[0]}"
    cp -- "${mupdf_cached[0]}" "$staging/$mupdf_name"
else
    command -v curl >/dev/null || die "curl is required to download $mupdf_url"
    echo "MuPDF source: downloading $mupdf_url"
    curl --fail --location --silent --show-error --proto =https --proto-redir =https \
        --output "$staging/$mupdf_name" "$mupdf_url" || die "cannot download $mupdf_url"
fi
mupdf_actual="$(sha256sum -- "$staging/$mupdf_name")"
mupdf_actual="${mupdf_actual%% *}"
[[ "$mupdf_actual" == "$mupdf_sha256" ]] ||
    die "$mupdf_name SHA256 is $mupdf_actual, expected $mupdf_sha256 (cmake/Dependencies.cmake)"

# Updater SHA256SUMS'ta yalnız kendi varlığının satırını arar (src/app/updater.cpp sums_entry); ek satır zararsız.
(cd "$staging" && sha256sum -- "$appimage_name" "$zip_name" "$mupdf_name" >SHA256SUMS)

rm -rf -- "$release_dir"
mv -T -- "$staging" "$release_dir"
echo "release $version: $release_dir"
cat -- "$release_dir/SHA256SUMS"
