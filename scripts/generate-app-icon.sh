#!/usr/bin/env bash
# resources/icons altındaki rasterleri (hicolor PNG'leri ve Windows .ico) SVG kaynaklarından yeniden üretir; normal
# derlemede gerekmez, SVG değişince elle koşulur ve çıktılar repoya girer. Host'ta librsvg'li ImageMagick 7 gerekir:
# ImageMagick'in kendi MSVG çizicisi yuvarlak köşe ve yay komutlarını bozar.
set -euo pipefail
cd "$(dirname "$0")/.."

die() { echo "ERROR: $*" >&2; exit 1; }

command -v magick >/dev/null || die "ImageMagick 7 (magick) is required"
# Önce değişkene: `grep -q` erken çıkınca magick SIGPIPE alır, pipefail bunu sahte hataya çevirirdi.
formats="$(magick -list format)"
grep -q 'RSVG' <<<"$formats" || die "ImageMagick is built without librsvg; MSVG renders the icon wrongly"

icons=resources/icons
sizes=(16 24 32 48 64 128 256)
pngs=()
for size in "${sizes[@]}"; do
    # 16 ve 24 px ızgaraya oturan sade çizimden: büyük çizimin düğümleri bu boyutta bulanık lekeye döner.
    if [[ "$size" -le 24 ]]; then
        source_svg="$icons/noteapp-small.svg"
        viewbox=16
    else
        source_svg="$icons/noteapp.svg"
        viewbox=256
    fi
    out="$icons/hicolor/${size}x${size}/apps/noteapp.png"
    mkdir -p "$(dirname "$out")"
    # 96 dpi'da SVG kendi genişliğinde çizilir; yoğunluk tam hedef boyutu verir, yeniden örnekleme yok. PNG32: 8 bit RGBA;
    # -strip: zaman damgası yok, aynı girdi aynı baytı verir.
    magick -background none -density "$((96 * size / viewbox))" "$source_svg" -strip "PNG32:$out"
    actual="$(magick identify -format '%wx%h' "$out")"
    [[ "$actual" == "${size}x${size}" ]] || die "$out: expected ${size}x${size}, got $actual"
    pngs+=("$out")
done

# 256 px giriş PNG olarak saklanır (Windows Vista+ biçimi); yoksa her exe'ye ~270 KB sıkıştırılmamış bitmap girer.
magick "${pngs[@]}" -define icon:png-compression-size=256 "$icons/noteapp.ico"
echo "Generated: ${pngs[*]} $icons/noteapp.ico"
