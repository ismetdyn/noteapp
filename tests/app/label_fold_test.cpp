// md4c 0.6.0 katlamasının taşınan hâli (label_fold.hpp): tablonun her biçimi (tek kod noktası, aralık, "büyük, küçük"
// çifti aralığı, iki ve üç kod noktalı karşılık, astral düzlem) ve sınırları. Beklenen değerler md4c.c tablolarından
// ve Unicode CaseFolding'den; tablonun tamamı scripts/check-label-fold.py ile md4c kaynağına karşı denetlenir.
#include "app/label_fold.hpp"

#include <doctest/doctest.h>
#include <vector>

namespace {

struct FoldCase {
    char32_t codepoint;
    std::vector<char32_t> folded;
};

std::vector<char32_t> fold(char32_t codepoint) {
    const noteapp::app::FoldedCodepoints folded = noteapp::app::fold_codepoint(codepoint);
    return {folded.codepoints.begin(), folded.codepoints.begin() + static_cast<std::ptrdiff_t>(folded.count)};
}

}  // namespace

TEST_CASE("label folding matches md4c's table at every kind of entry and its bounds") {
    const std::vector<FoldCase> cases = {
        // ASCII: yalnız A-Z; komşu noktalama değişmez.
        {U'@', {U'@'}},
        {U'A', {U'a'}},
        {U'Z', {U'z'}},
        {U'[', {U'['}},
        {U'`', {U'`'}},
        {U'a', {U'a'}},
        {U'{', {U'{'}},
        {0x7F, {0x7F}},
        {0x80, {0x80}},
        // Aralıktan aralığa (U+00C0..00D6), aradaki tek nokta (×) eşlemesiz.
        {0x00C4, {0x00E4}},
        {0x00D6, {0x00F6}},
        {0x00D7, {0x00D7}},
        {0x00DC, {0x00FC}},
        // "Büyük, küçük" çifti aralığı U+0100..012E: çift konum +1, tek konum kendisi; aralığın dışı kendisi.
        {0x0100, {0x0101}},
        {0x0101, {0x0101}},
        {0x011E, {0x011F}},  // Ğ → ğ
        {0x012E, {0x012F}},
        {0x012F, {0x012F}},
        {0x015E, {0x015F}},  // Ş → ş
        // Türkçe: İ iki kod noktasına (i + U+0307) katlanır; ı katlanmaz; I → i.
        {0x0130, {U'i', 0x0307}},
        {0x0131, {0x0131}},
        {U'I', {U'i'}},
        {0x00C7, {0x00E7}},  // Ç → ç
        // İki kod noktalı karşılıklar: ß, aralık U+1F80..1F87 ve U+1F88..1F8F (ilk nokta kayar, ikincisi sabit).
        {0x00DF, {U's', U's'}},
        {0x1F80, {0x1F00, 0x03B9}},
        {0x1F87, {0x1F07, 0x03B9}},
        {0x1F88, {0x1F00, 0x03B9}},
        {0x1F8F, {0x1F07, 0x03B9}},
        // Üç kod noktalı karşılıklar.
        {0x0390, {0x03B9, 0x0308, 0x0301}},
        {0xFB03, {U'f', U'f', U'i'}},
        // Astral düzlem; U+1E900..1E921 tablonun son kaydı.
        {0x10400, {0x10428}},
        {0x10427, {0x1044F}},
        {0x1E900, {0x1E922}},
        {0x1E921, {0x1E943}},
        {0x1E922, {0x1E922}},
        // Eşlemesiz.
        {0x4E00, {0x4E00}},
        {0x10FFFF, {0x10FFFF}},
        // md4c geçersiz UTF-8 baytını işaretli `char` değeriyle çözer: hiçbir kayda düşmez.
        {0xFFFFFFC4, {0xFFFFFFC4}},
    };
    for (const FoldCase& entry : cases) {
        CAPTURE(static_cast<unsigned>(entry.codepoint));
        CHECK(fold(entry.codepoint) == entry.folded);
    }
}
