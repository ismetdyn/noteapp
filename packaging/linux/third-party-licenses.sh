# AppImage ve Windows paketinin ortak lisans toplama adımı; konteynerde `source` edilir. Çağıran $licenses hedefini ve
# die() işlevini tanımlar. Eksik dosya derlemeyi kırar: lisanssız paket çıkmamalı.
add_license() {
    local component="$1" source_file="$2"
    [[ -s "$source_file" ]] || die "license file missing for $component: $source_file"
    mkdir -p "$licenses/$component"
    cp -L "$source_file" "$licenses/$component/"
}

# Kaynak yorumlarındaki lisans ifadeleri. Büyük harf bilinçli dar: kod içindeki SDL_PROP_APP_METADATA_COPYRIGHT_STRING
# gibi adlar eşleşmesin.
notice_marker='[Cc]opyright|COPYRIGHT [(0-9]|[Pp]ublic [Dd]omain|PUBLIC DOMAIN|SPDX-License-Identifier|[Ll]icensed under|under the [^ ]* *[Ll]icense|[Uu]nlicense|Permission to use|Permission is (hereby )?granted|Redistribution and use'

# Derlenen her _deps dosyasının (ninja bağımlılık günlüğü: kaynaklar + başlıklar) lisans yorumları, yinelenenler tek
# blok olacak biçimde <bileşen>/SOURCE-NOTICES.txt'e yazılır. Liste elle tutulmaz: libgit2/SDL/libharu içine gömülü kod,
# hangi platformda derleniyorsa o pakette kendiliğinden görünür. Kapsam dışı: _deps dışındaki sistem başlıkları ve dosyanın
# hiçbir kalıba uymayan lisans metni; kalıba uyan satır yorum dışındaysa dosya bazında yakalanır.
add_compiled_source_notices() {
    local deps="$1" inputs component notices
    inputs="$(ninja -C "${deps%/_deps}" -t deps | LC_ALL=C sed -nE 's#^ +.*_deps/([^/]+-src/.*)#\1#p' | LC_ALL=C sort -u)"
    for component in $(cut -d/ -f1 <<<"$inputs" | sort -u); do
        notices="$licenses/${component%-src}/SOURCE-NOTICES.txt"
        mkdir -p "${notices%/*}"
        grep "^$component/" <<<"$inputs" | sed "s#^#$deps/#" | while IFS= read -r input; do
            if [[ -f "$input" ]]; then printf '%s\0' "$input"; fi
        done | LC_ALL=C xargs -0 awk -v marker="$notice_marker" -v prefix="$deps/" '
            function flush_block() {
                if (block ~ marker) {
                    if (!(block in files))
                        order[++count] = block
                    files[block] = files[block] "  " name "\n"
                }
                block = ""
            }
            FNR == 1 {
                flush_block()
                in_block = 0
                name = substr(FILENAME, length(prefix) + 1)
            }
            {
                line = $0
                sub(/\r$/, "", line)
                if (in_block) {
                    block = block line "\n"
                    if (line ~ /\*\//) {
                        in_block = 0
                        flush_block()
                    }
                } else if (line ~ /^[[:space:]]*\/\//) {
                    block = block line "\n"
                } else {
                    flush_block()
                    if (line ~ /\/\*/) {
                        block = line "\n"
                        if (line ~ /\*\//)
                            flush_block()
                        else
                            in_block = 1
                    }
                }
            }
            END {
                flush_block()
                for (i = 1; i <= count; i++)
                    printf "%s\nFiles:\n%s\n", order[i], files[order[i]]
            }' >"$notices"
    done
    for component in sdl3 libgit2 libharu sqlite curl; do
        [[ -s "$licenses/$component/SOURCE-NOTICES.txt" ]] || die "no compiled source notices for $component"
    done
    # SQLite'ın lisans dosyası yoktur: kamu malı bildirimi yalnız kaynağın başındadır.
    grep -q "The author disclaims copyright" "$licenses/sqlite/SOURCE-NOTICES.txt" ||
        die "SQLite public domain notice missing"
    # Yorum ayrıştırıcısının kaçırdığı (ör. kod sonundaki yorum) lisans satırı sessizce paketten düşmesin.
    local missed
    missed="$(sed "s#^#$deps/#" <<<"$inputs" | while IFS= read -r input; do
        if [[ -f "$input" ]] && LC_ALL=C grep -Eq "$notice_marker" "$input" &&
            ! grep -qF "  ${input#"$deps"/}" "$licenses/$(cut -d/ -f1 <<<"${input#"$deps"/}" | sed 's/-src$//')/SOURCE-NOTICES.txt"; then
            echo "$input"
        fi
    done)"
    [[ -z "$missed" ]] || die "license text outside the comments the notice scan reads: $missed"
}

# SDL wayland-protocols/*.xml'den kod üretir ve xml'ler bildirimin kopyalarda kalmasını ister. Hangi protokollerin
# derlendiğini üretilen başlıklar söyler. Wayland derlenmemişse (Windows) bildirim de yoktur.
add_wayland_protocol_notices() {
    local deps="$1" config header name xml block notices
    config="$(find "$deps/sdl3-build" -name SDL_build_config.h -print -quit)"
    [[ -n "$config" ]] || die "SDL_build_config.h not found"
    if ! grep -q "^#define SDL_VIDEO_DRIVER_WAYLAND\b" "$config"; then
        return 0
    fi
    notices="$licenses/sdl3/wayland-protocols-NOTICES.txt"
    mkdir -p "${notices%/*}"
    : >"$notices"
    for header in "$deps"/sdl3-build/wayland-generated-protocols/*-client-protocol.h; do
        name="${header##*/}"
        name="${name%-client-protocol.h}"
        xml="$deps/sdl3-src/wayland-protocols/$name.xml"
        [[ -f "$xml" ]] || die "no protocol XML for generated Wayland header: $header"
        block="$(sed -n '/<copyright>/,/<\/copyright>/p' "$xml")"
        if [[ -z "$block" ]]; then
            # pointer-gestures-unstable-v1.xml kaynağında hiç bildirim yok; başka biçimde yazılmış metin kaçmasın.
            if LC_ALL=C grep -Eq "$notice_marker" "$xml"; then
                die "license text outside <copyright> in $xml"
            fi
            block="(the source file carries no copyright notice)"
        fi
        printf '%s\n%s\n\n' "wayland-protocols/$name.xml" "$block" >>"$notices"
    done
}

# Her iki pakete statik giren bileşenler. $1: FetchContent _deps dizini.
add_static_component_licenses() {
    local deps="$1"
    add_compiled_source_notices "$deps"
    add_wayland_protocol_notices "$deps"
    add_license dejavu /src/resources/fonts/LICENSE-DejaVu.txt
    add_license noteapp-icons /src/resources/fonts/LICENSE-NoteappIcons.txt
    add_license inter /src/resources/fonts/LICENSE-Inter.txt
    add_license jetbrains-mono /src/resources/fonts/LICENSE-JetBrainsMono.txt
    add_license sdl3 "$deps/sdl3-src/LICENSE.txt"
    add_license sdl3-yuv2rgb "$deps/sdl3-src/src/video/yuv2rgb/LICENSE"
    add_license imgui "$deps/imgui-src/LICENSE.txt"
    add_license libharu "$deps/libharu-src/LICENSE"
    add_license md4c "$deps/md4c-src/LICENSE.md"
    add_license ftxui "$deps/ftxui-src/LICENSE"
    add_license tomlplusplus "$deps/tomlplusplus-src/LICENSE"
    # COPYING: GPLv2 + linking exception ve ntlmclient, wildmatch, sha1dc gibi gömülü kodun metinleri.
    add_license libgit2 "$deps/libgit2-src/COPYING"
    add_license libgit2-zlib "$deps/libgit2-src/deps/zlib/LICENSE"
    add_license libgit2-pcre2 "$deps/libgit2-src/deps/pcre2/LICENCE.md"
    add_license libgit2-llhttp "$deps/libgit2-src/deps/llhttp/LICENSE-MIT"
    # xdiff COPYING'de geçmez; LGPL-2.1 bildirimi yalnız kaynak başlığındadır.
    add_license libgit2-xdiff "$deps/libgit2-src/deps/xdiff/xdiff.h"
    add_license libgit2-xdiff "$deps/libgit2-src/deps/winhttp/COPYING.LGPL"
    # curl lisansı (MIT türevi); derlenen dosyaların bildirimleri curl/SOURCE-NOTICES.txt'te.
    add_license curl "$deps/curl-src/COPYING"
}
