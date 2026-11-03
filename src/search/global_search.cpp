#include "search/global_search.hpp"

#include "search/fold.hpp"
#include "search/fuzzy.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>

namespace noteapp::search {

namespace {

// Karakter başına fzy puanı. Kelime başı + ardışık eşleşme ~1'e yaklaşır ("plan" -> plan.md 0,96; "wmn" ->
// work/meeting-notes.md 0,8); dağınık alt dizi bonusları seyrek toplar ("plan" -> pilot-kayin.md 0,2).
constexpr double kStrongNameScorePerChar = 0.5;
constexpr std::size_t kReservedLimitDivisor = 4;  // limit'in bu kadarda biri içerik/zayıf sonuçlara ayrılır

enum class Tier { StrongName, Content, WeakName };

struct Row {
    std::size_t index{};
    std::size_t path_size{};
    Tier tier = Tier::WeakName;
    bool name_matched = false;
    double name_score = 0;
    std::vector<std::size_t> positions = {};
    std::size_t content_hits = 0;
};

struct NameTerms {
    std::vector<std::string> terms;
    std::size_t total_chars = 0;  // kod noktası
};

NameTerms name_terms(std::string_view text) {
    NameTerms result{split_terms(text)};
    for (const std::string& term : result.terms)
        result.total_chars += fold(term).text.size();
    return result;
}

// Her terim yolda fuzzy geçmeli; puanlar toplanır, konumlar birleşir.
void match_name(const NameTerms& query, const std::string& path, std::size_t max_candidate, Row& row) {
    double score = 0;
    std::vector<std::size_t> positions;
    for (const std::string& term : query.terms) {
        auto match = fuzzy_match(term, path, max_candidate);
        if (!match)
            return;
        score += match->score;
        positions.insert(positions.end(), match->positions.begin(), match->positions.end());
    }
    std::ranges::sort(positions);
    positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
    row.name_matched = true;
    row.name_score = score;
    row.positions = std::move(positions);
    if (score / static_cast<double>(query.total_chars) >= kStrongNameScorePerChar)
        row.tier = Tier::StrongName;
}

std::vector<Row> matching_rows(std::string_view text, const std::vector<std::size_t>& allowed,
                               std::span<const core::NoteMeta> notes, const ContentIndex& content_index,
                               std::size_t max_candidate) {
    const NameTerms query = name_terms(text);
    std::vector<Row> rows;
    rows.reserve(allowed.size());
    std::unordered_map<std::string, std::size_t> row_by_path;
    row_by_path.reserve(allowed.size());
    for (const std::size_t i : allowed) {
        std::string path = notes[i].path.string();
        Row row{i, path.size()};
        match_name(query, path, max_candidate, row);
        rows.push_back(std::move(row));
        row_by_path.emplace(std::move(path), rows.size() - 1);
    }
    for (const ContentCount& count : content_index.count_matches(text)) {
        const auto found = row_by_path.find(count.path->string());
        if (found == row_by_path.end())
            continue;  // süzüldü ya da indeks listeden yeni
        Row& row = rows[found->second];
        row.content_hits = count.hit_count;
        if (row.tier != Tier::StrongName)
            row.tier = Tier::Content;
    }
    std::erase_if(rows, [](const Row& row) { return !row.name_matched && row.content_hits == 0; });
    return rows;
}

void sort_by_relevance(std::vector<Row>& rows, std::span<const core::NoteMeta> notes) {
    std::ranges::stable_sort(rows, [&](const Row& a, const Row& b) {
        if (a.tier != b.tier)
            return a.tier < b.tier;
        if (a.tier == Tier::Content && a.content_hits != b.content_hits)
            return a.content_hits > b.content_hits;
        if (a.tier != Tier::Content && a.name_score != b.name_score)
            return a.name_score > b.name_score;
        if (a.tier != Tier::Content && a.path_size != b.path_size)
            return a.path_size < b.path_size;
        return notes[a.index].updated_at > notes[b.index].updated_at;
    });
}

// Güçlü ad eşleşmeleri limiti doldurmasın: limit'in bir kısmı geri kalanlara ayrılır, kullanılmayan pay
// adlara kalır.
void keep_within_limit(std::vector<Row>& rows, std::size_t limit) {
    if (rows.size() <= limit)
        return;
    const auto strong = static_cast<std::size_t>(std::ranges::count(rows, Tier::StrongName, &Row::tier));
    const std::size_t others = rows.size() - strong;
    const std::size_t kept_strong = std::min(strong, limit - std::min(others, limit / kReservedLimitDivisor));
    rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(kept_strong),
               rows.begin() + static_cast<std::ptrdiff_t>(strong));
    rows.resize(std::min(rows.size(), limit), Row{0, 0});
}

// Açık sıralama: alaka sırası eşitlik bozucu kalır (sort_notes kararlıdır).
void sort_explicitly(std::vector<Row>& rows, const SortSpec& sort, std::span<const core::NoteMeta> notes) {
    std::vector<std::size_t> indices;
    indices.reserve(rows.size());
    std::vector<std::size_t> row_of_note(notes.size());
    for (std::size_t r = 0; r < rows.size(); ++r) {
        indices.push_back(rows[r].index);
        row_of_note[rows[r].index] = r;
    }
    sort_notes(indices, sort, notes);
    std::vector<Row> sorted;
    sorted.reserve(rows.size());
    for (const std::size_t i : indices)
        sorted.push_back(std::move(rows[row_of_note[i]]));
    rows = std::move(sorted);
}

}  // namespace

namespace {

std::expected<std::vector<GlobalHit>, QueryError>
search_within(std::string_view query, std::span<const core::NoteMeta> notes,
              const ContentIndex& content_index, std::size_t limit, std::size_t max_candidate,
              std::chrono::system_clock::time_point now, const ViewLookup& views) {
    const auto parsed = parse_query(query, views);
    if (!parsed)
        return std::unexpected(parsed.error());
    const std::vector<std::size_t> allowed = apply(parsed->filters, std::nullopt, notes, now);

    std::vector<GlobalHit> hits;
    if (split_terms(parsed->text).empty()) {  // yalnız argüman: süzülmüş liste, eşleşme bilgisi yok
        std::vector<std::size_t> indices = allowed;
        if (parsed->sort)
            sort_notes(indices, *parsed->sort, notes);
        indices.resize(std::min(indices.size(), limit));
        for (const std::size_t i : indices)
            hits.push_back(GlobalHit{i, notes[i].path});
        return hits;
    }

    std::vector<Row> rows = matching_rows(parsed->text, allowed, notes, content_index, max_candidate);
    sort_by_relevance(rows, notes);
    if (parsed->sort) {
        sort_explicitly(rows, *parsed->sort, notes);
        rows.resize(std::min(rows.size(), limit), Row{0, 0});
    } else {
        keep_within_limit(rows, limit);
    }

    hits.reserve(rows.size());
    for (Row& row : rows) {
        const core::NotePath& path = notes[row.index].path;
        hits.push_back(
            GlobalHit{row.index, path, row.name_matched, row.content_hits > 0, std::move(row.positions),
                      row.content_hits,
                      row.content_hits > 0 ? content_index.best_hit(path, parsed->text) : std::nullopt});
    }
    return hits;
}

}  // namespace

std::expected<std::vector<GlobalHit>, QueryError>
search_all(std::string_view query, std::span<const core::NoteMeta> notes, const ContentIndex& content_index,
           const core::SearchLimits& limits, std::chrono::system_clock::time_point now,
           const ViewLookup& views) {
    return search_within(query, notes, content_index, limits.global_results.cap(),
                         limits.fuzzy_candidate_chars.cap(), now, views);
}

}  // namespace noteapp::search
