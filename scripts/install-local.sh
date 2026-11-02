#!/usr/bin/env bash
# Günlük kullanım için yerel kurulum: FUSE bağlaması yok, `notes` AppImage'dan altı kat hızlı açılır (README).
# Kaynak AppImage dosyası ya da `cmake --install` ağacıdır. Yalnız şunlara yazar:
#   ~/.local/share/noteapp/<sürüm>/    paketin kendisi; aynı sürüm yeniden kurulunca değişir
#   ~/.local/bin/{note,notes,notes-gui} bu dizine symlink
# Kullanım: scripts/install-local.sh <noteapp-*.AppImage | cmake-install-dizini> | --uninstall
# ~/.local/share/noteapp aynı zamanda veri dizinidir (graph.db, geçiş yedekleri): dizin bütünüyle silinmez.
set -euo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }

[[ $# -eq 1 ]] || die "usage: $0 <AppImage | cmake install prefix> | --uninstall"
: "${HOME:?HOME is required}"

share="$HOME/.local/share/noteapp"
bin="$HOME/.local/bin"
readonly stale_staging_minutes=60

# Bizim bağımız: hedef mutlak ve sürüm dizinimizin içinde. `..` hedefi dışarı kaçırabilir, sayılmaz.
is_our_link() {
    [[ -L "$1" ]] || return 1
    local link_target
    link_target="$(readlink -- "$1")"
    [[ "$link_target" == "$share"/* && "$link_target" != */../* ]]
}

# Sürüm dizini paketin çalıştırılabilirini kökte taşır; geçiş yedeği notları notes/ altında tutar, karışmaz.
is_version_dir() {
    [[ -d "$1" && ! -L "$1" && (-x "$1/AppRun" || -x "$1/bin/note") ]]
}

uninstall() {
    local name link dir
    for name in note notes notes-gui; do
        link="$bin/$name"
        if is_our_link "$link"; then
            rm -- "$link"
        elif [[ -e "$link" || -L "$link" ]]; then
            echo "not ours, left in place: $link" >&2
        fi
    done
    [[ -d "$share" ]] || return 0
    for dir in "$share"/* "$share"/.install.*; do
        if is_version_dir "$dir" || [[ -d "$dir" && "${dir##*/}" == .install.* ]]; then
            rm -rf -- "$dir"
        fi
    done
    # Yalnız boşsa kalkar: not verisi varsa dizin durur.
    if rmdir -- "$share" 2>/dev/null; then
        echo "uninstalled"
    else
        echo "uninstalled; your notes stay in: $share"
    fi
}

if [[ "$1" == --uninstall ]]; then
    uninstall
    exit 0
fi

source_path="$(realpath -- "$1")" || die "not found: $1"
mkdir -p "$share" "$bin"

# Kesilmiş kurulumun artığı. Taze olanı çalışan başka bir kuruluma ait olabilir.
find "$share" -mindepth 1 -maxdepth 1 -type d -name '.install.*' -mmin "+$stale_staging_minutes" \
    -exec rm -rf -- {} +

# Açılan paket özel dizinde kalır: paylaşılan /tmp'de başka kullanıcı önceden dizin yaratabilir (README).
# Aynı dosya sisteminde olması rename'i atomik yapar.
staging="$(mktemp -d "$share/.install.XXXXXX")"
cleanup() { rm -rf "$staging"; }
trap cleanup EXIT

# Paket kökünü ve üç komutun paket içindeki hedefini belirler.
if [[ -f "$source_path" ]]; then
    [[ -x "$source_path" ]] || die "not executable: $source_path"
    (cd "$staging" && "$source_path" --appimage-extract >/dev/null) || die "extracting failed: $source_path"
    package="$staging/squashfs-root"
    [[ -x "$package/AppRun" ]] || die "no AppRun in: $source_path"
    version_output="$("$package/AppRun" note --version)" || die "cannot read the version"
    # AppRun komutu symlink adından seçer, CA ayarını da o yapar.
    commands=(note:AppRun notes:AppRun notes-gui:AppRun)
elif [[ -d "$source_path" ]]; then
    [[ -x "$source_path/bin/notes" && -x "$source_path/bin/note" ]] || die "no bin/notes and bin/note in: $source_path"
    package="$staging/tree"
    cp -a "$source_path" "$package"
    version_output="$("$package/bin/note" --version)" || die "cannot read the version"
    commands=(note:bin/note notes:bin/notes)
    if [[ -x "$source_path/bin/notes-gui" ]]; then
        commands+=(notes-gui:bin/notes-gui)
    fi
else
    die "neither a file nor a directory: $source_path"
fi

# Çıktı `note <sürüm>`: son sözcük. Yol olarak kullanılacağı için doğrulanır.
version="${version_output##* }"
[[ "$version" =~ ^[0-9A-Za-z.+_-]+$ ]] || die "unexpected version: $version_output"

# Yalnız bizim bağımız değişir; yabancı dosya ve bağ ezilmez. Hiçbir şey değişmeden önce denetlenir.
for entry in "${commands[@]}"; do
    link="$bin/${entry%%:*}"
    if [[ (-e "$link" || -L "$link") ]] && ! is_our_link "$link"; then
        die "refusing to replace, not a noteapp link: $link (remove it yourself to install here)"
    fi
done

target="$share/$version"
if [[ ! -e "$target" ]]; then
    mv -T -- "$package" "$target" || die "cannot move the package to: $target"
elif ! mv --exchange -T -- "$package" "$target" 2>/dev/null; then
    # coreutils < 9.5: --exchange yok. Dolu dizin üzerine rename olmaz: iki adım, ikincisi düşerse geri konur.
    # ponytail: arada kesilirse sürüm dizini bir an yok olur; atomik yol yalnız --exchange.
    previous="$staging/previous"
    mv -T -- "$target" "$previous" || die "cannot replace: $target"
    if ! mv -T -- "$package" "$target"; then
        mv -T -- "$previous" "$target"
        die "cannot move the package to: $target"
    fi
fi
for entry in "${commands[@]}"; do
    ln -sfn "$target/${entry#*:}" "$bin/${entry%%:*}"
done
echo "installed $version: $target"
