#include "search/fuzzy.hpp"

#include "search/fold.hpp"

#include <algorithm>
#include <limits>

// Algoritma jhawthorn/fzy (MIT) src/match.c'den uyarlandı: iki matrisli (D: eşleşmeyle biten, M: en iyi)
// dinamik programlama + geri izleme. Kod bu projede C++ ile yeniden yazıldı.

namespace noteapp::search {
namespace {

// fzy sabitleri, sapmasız: kullanıcı fzf davranışı istiyor; kelime başlarına düşen kısaltma eşleşmesi ("mtg"
// → my-long-text-generator.md) kelime içindeki sıkı eşleşmeden (meeting.md) önce gelir.
constexpr double kGapLeading = -0.005;
constexpr double kGapTrailing = -0.005;
constexpr double kGapInner = -0.01;
constexpr double kConsecutive = 1.0;
constexpr double kBonusSlash = 0.9;
constexpr double kBonusWord = 0.8;
constexpr double kBonusCapital = 0.7;
constexpr double kBonusDot = 0.6;
constexpr double kNegInf = -std::numeric_limits<double>::infinity();

// fzy uzun adayı DP'ye sokmaz (bellek/zaman); sınır config'tedir (limits.search.fuzzy_candidate_chars). Uzun
// adayda eşleşme alt dizi olarak doğrulanır, düşük ama pozitif puanla döner, konum üretilmez.
constexpr double kLongCandidateScore = 0.001;

// Tam eşleşmenin (sorgu == aday) puanı: DP sonuçlarının hiçbiri bunu aşamaz.
constexpr double kExactScore = 1e9;

bool is_alnum(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || (c >= U'0' && c <= U'9') || c >= 0x80;
}

// Not: büyük/küçük harf özgün metinden bakılır, çünkü katlama bu bilgiyi siler.
bool is_upper(char32_t c) {
    return to_lower(c) != c;
}
bool is_lower(char32_t c) {
    return !is_upper(c) && is_alnum(c) && !(c >= U'0' && c <= U'9');
}

double bonus_after(char32_t last, char32_t cur) {
    if (!is_alnum(cur))
        return 0.0;
    if (last == U'/')
        return kBonusSlash;
    if (last == U'-' || last == U'_' || last == U' ')
        return kBonusWord;
    if (last == U'.')
        return kBonusDot;
    if (is_lower(last) && is_upper(cur))
        return kBonusCapital;
    return 0.0;
}

std::optional<FuzzyMatch> match_folded(const Folded& q, std::string_view candidate,
                                       std::size_t max_candidate) {
    const std::size_t n = q.text.size();
    if (n == 0)
        return FuzzyMatch{0.0, {}};

    const Folded h = fold(candidate);
    const std::size_t m = h.text.size();
    if (n > m)
        return std::nullopt;

    // Alt dizi ön süzgeci: çoğu aday burada elenir, DP yalnız eşleşenlere çalışır.
    {
        std::size_t qi = 0;
        for (std::size_t j = 0; j < m && qi < n; ++j)
            if (h.text[j] == q.text[qi])
                ++qi;
        if (qi < n)
            return std::nullopt;
    }

    if (m > max_candidate)
        return FuzzyMatch{kLongCandidateScore, {}};

    auto offset_of = [&h](std::size_t j) { return h.byte_offsets[j]; };

    if (n == m) {  // alt dizi + eşit uzunluk = tam eşleşme
        FuzzyMatch r{kExactScore, {}};
        for (std::size_t j = 0; j < m; ++j)
            r.positions.push_back(offset_of(j));
        return r;
    }

    std::vector<double> bonus(m);
    char32_t last = U'/';  // dizgi başı '/' sonrası sayılır (fzy)
    for (std::size_t j = 0; j < m; ++j) {
        bonus[j] = bonus_after(last, h.original[j]);
        last = h.original[j];
    }

    std::vector<double> d(n * m, kNegInf), mm(n * m, kNegInf);
    for (std::size_t i = 0; i < n; ++i) {
        double prev = kNegInf;
        const double gap = (i == n - 1) ? kGapTrailing : kGapInner;
        for (std::size_t j = 0; j < m; ++j) {
            double score = kNegInf;
            if (q.text[i] == h.text[j]) {
                if (i == 0) {
                    score = static_cast<double>(j) * kGapLeading + bonus[j];
                } else if (j > 0) {
                    score = std::max(mm[(i - 1) * m + (j - 1)] + bonus[j],
                                     d[(i - 1) * m + (j - 1)] + kConsecutive);
                }
                d[i * m + j] = score;
                prev = std::max(score, prev + gap);
            } else {
                prev = prev + gap;
            }
            mm[i * m + j] = prev;
        }
    }

    FuzzyMatch r{mm[(n - 1) * m + (m - 1)], std::vector<std::size_t>(n)};
    bool match_required = false;
    std::size_t j = m;
    for (std::size_t i = n; i-- > 0;) {
        while (j-- > 0) {
            const double dv = d[i * m + j];
            if (dv != kNegInf && (match_required || dv == mm[i * m + j])) {
                // Bu eşleşme önceki satırın ardışık eşleşmesinden geldiyse, geri izlemede de ardışık kalmalı.
                match_required = i > 0 && j > 0 && mm[i * m + j] == d[(i - 1) * m + (j - 1)] + kConsecutive;
                r.positions[i] = offset_of(j);
                break;
            }
        }
    }
    return r;
}

}  // namespace

std::optional<FuzzyMatch> fuzzy_match(std::string_view query, std::string_view candidate,
                                      std::size_t max_candidate) {
    return match_folded(fold(query), candidate, max_candidate);
}

std::vector<Ranked> rank_notes(std::string_view query, std::span<const core::NoteInfo> notes,
                               std::size_t max_candidate) {
    const Folded q = fold(query);  // sorgu bir kez katlanır
    std::vector<Ranked> out;
    std::vector<std::size_t> path_len(notes.size());  // sıralamada tekrar string üretmemek için
    out.reserve(notes.size());
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const std::string path = notes[i].path.string();
        if (auto m = match_folded(q, path, max_candidate)) {
            path_len[i] = path.size();
            out.push_back({i, std::move(*m)});
        }
    }

    // Boş sorguda puanlar hep eşit; SPEC-search "mtime azalan" der, yol uzunluğu sırayı etkilememeli.
    if (q.text.empty()) {
        std::ranges::sort(out, [&notes](const Ranked& a, const Ranked& b) {
            const auto& na = notes[a.index];
            const auto& nb = notes[b.index];
            return na.mtime != nb.mtime ? na.mtime > nb.mtime : na.path < nb.path;
        });
        return out;
    }

    // Eşitlikte index: girdi sırasından bağımsız, belirlenimci sonuç.
    std::ranges::sort(out, [&notes, &path_len](const Ranked& a, const Ranked& b) {
        if (a.match.score != b.match.score)
            return a.match.score > b.match.score;
        const auto& na = notes[a.index];
        const auto& nb = notes[b.index];
        if (path_len[a.index] != path_len[b.index])
            return path_len[a.index] < path_len[b.index];
        if (na.mtime != nb.mtime)
            return na.mtime > nb.mtime;
        return a.index < b.index;
    });
    return out;
}

}  // namespace noteapp::search
