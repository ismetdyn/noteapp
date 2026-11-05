#pragma once

#include <array>
#include <cstddef>

namespace noteapp::app {

// md4c 0.6.0'ın bağlantı etiketi katlaması (md4c.c: md_get_unicode_fold_info), tablosuyla birebir: parçalı
// önizleme etiketleri md4c'nin bütün belgede eşleyeceği gibi eşlemeli. md4c bu işlevi dışa vermez; sürümü
// cmake/Dependencies.cmake'te sabittir. md4c sabiti değişince `python3 -I scripts/check-label-fold.py
// <build>/_deps/md4c-src` yeniden çalıştırılır; fark varsa tablo yenilenir. Bir kod noktası en çok üç kod
// noktasına katlanır (ß → ss, İ → i + U+0307); eşlemesi olmayan kendisidir.
struct FoldedCodepoints {
    std::array<char32_t, 3> codepoints{};
    std::size_t count = 0;
};
FoldedCodepoints fold_codepoint(char32_t codepoint);

}  // namespace noteapp::app
