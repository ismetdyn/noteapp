#include "search/content_index.hpp"

#include "search/fold.hpp"

#include <algorithm>
#include <cstddef>

namespace noteapp::search {

namespace {

constexpr std::size_t kSnippetCodePoints = 120;
constexpr std::string_view kEllipsis = "…";

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

}  // namespace

// Boşlukla ayrılmış terimler; "..." arası tek ifade. Kapanmamış tırnak sonuna kadar ifade sayılır.
std::vector<std::string> split_terms(std::string_view query) {
    std::vector<std::string> terms;
    auto push = [&](std::string_view raw) {
        if (!raw.empty())
            terms.emplace_back(raw);
    };
    std::size_t i = 0;
    while (i < query.size()) {
        if (is_space(query[i])) {
            ++i;
        } else if (query[i] == '"') {
            auto end = query.find('"', i + 1);
            if (end == std::string_view::npos)
                end = query.size();
            push(query.substr(i + 1, end - i - 1));
            i = end + 1;
        } else {
            auto end = i;
            while (end < query.size() && !is_space(query[end]) && query[end] != '"')
                ++end;
            push(query.substr(i, end - i));
            i = end;
        }
    }
    return terms;
}

std::vector<std::u32string> parse_terms(std::string_view query) {
    std::vector<std::u32string> terms;
    for (const std::string& raw : split_terms(query))
        terms.push_back(fold(raw).text);
    return terms;
}

namespace {

struct Occurrence {
    std::size_t pos;
    std::size_t len;
    std::size_t term;
};

// Tüm terimlerin geçişleri (konuma göre sıralı); bir terim hiç yoksa boş döner (AND).
std::vector<Occurrence> scan(const std::u32string& text, const std::vector<std::u32string>& terms) {
    std::vector<Occurrence> all;
    for (std::size_t t = 0; t < terms.size(); ++t) {
        const auto& term = terms[t];
        auto pos = text.find(term);
        if (pos == std::u32string::npos)
            return {};
        for (; pos != std::u32string::npos; pos = text.find(term, pos + 1))
            all.push_back({pos, term.size(), t});
    }
    std::sort(all.begin(), all.end(), [](const Occurrence& a, const Occurrence& b) { return a.pos < b.pos; });
    return all;
}

// 1 tabanlı satır numarası: kod noktası konumunu içeren satır.
template <typename EntryT> std::size_t line_of(const EntryT& e, std::size_t pos) {
    return static_cast<std::size_t>(std::upper_bound(e.line_starts.begin(), e.line_starts.end(), pos) -
                                    e.line_starts.begin());
}

// Sonraki satırın başı (son satırda metin sonu + 1).
template <typename EntryT> std::size_t next_line_start(const EntryT& e, std::size_t line) {
    return line < e.line_starts.size() ? e.line_starts[line] : e.folded.size() + 1;
}

// occ[first]'in satırındaki son geçişin bir sonrası.
template <typename EntryT>
std::size_t end_of_line_group(const EntryT& e, const std::vector<Occurrence>& occ, std::size_t first) {
    const std::size_t next_ls = next_line_start(e, line_of(e, occ[first].pos));
    std::size_t last = first;
    while (last < occ.size() && occ[last].pos < next_ls)
        ++last;
    return last;
}

// En az bir terim geçen satır sayısı.
template <typename EntryT> std::size_t count_hit_lines(const EntryT& e, const std::vector<Occurrence>& occ) {
    std::size_t count = 0, last_line = 0;  // satır no 1'den başlar, 0 "henüz yok"
    for (const auto& o : occ) {
        const auto line = line_of(e, o.pos);
        if (line != last_line)
            ++count;
        last_line = line;
    }
    return count;
}

// occ[first]'in satırı için kesit ve vurgu aralıkları.
template <typename EntryT>
Hit make_hit(const EntryT& e, const std::vector<Occurrence>& occ, std::size_t first) {
    const auto n = e.folded.size();
    auto byte_at = [&](std::size_t cp) -> std::size_t { return cp < n ? e.byte_offsets[cp] : e.text.size(); };
    const auto line = line_of(e, occ[first].pos);
    const std::size_t ls = e.line_starts[line - 1];
    std::size_t le = next_line_start(e, line) - 1;  // '\n' hariç
    if (le > ls && e.folded[le - 1] == U'\r')
        --le;
    const std::size_t group_end =
        end_of_line_group(e, occ, first);  // bu satırdaki geçişler [first, group_end)

    // Kesit penceresi: ilk geçişin ortalandığı ~120 kod noktası, satır sınırına kenetli.
    std::size_t start = ls, end = le;
    if (le - ls > kSnippetCodePoints) {
        const auto mid = occ[first].pos + occ[first].len / 2;
        start = mid > ls + kSnippetCodePoints / 2 ? mid - kSnippetCodePoints / 2 : ls;
        start = std::min(start, le - kSnippetCodePoints);
        end = start + kSnippetCodePoints;
    }
    Hit hit{line, {}, {}};
    if (start > ls)
        hit.snippet += kEllipsis;
    const std::size_t prefix = hit.snippet.size();
    const std::size_t base = byte_at(start);
    hit.snippet.append(e.text, base, byte_at(end) - base);
    if (end < le)
        hit.snippet += kEllipsis;
    for (std::size_t k = first; k < group_end; ++k) {
        const auto s = std::max(occ[k].pos, start), t = std::min(occ[k].pos + occ[k].len, end);
        if (s < t)
            hit.ranges.emplace_back(prefix + byte_at(s) - base, prefix + byte_at(t) - base);
    }
    return hit;
}

}  // namespace

ContentIndex::ContentIndex(const core::NoteStore& store, std::size_t max_note_bytes, std::size_t max_hits)
    : store_(store), max_note_bytes_(max_note_bytes), max_hits_(max_hits) {}

void ContentIndex::refresh() {
    std::map<core::NotePath, Entry> next;
    for (const auto& info : store_.list()) {
        if (info.size > max_note_bytes_)
            continue;
        auto old = entries_.find(info.path);
        if (old != entries_.end() && old->second.revision == info.revision && old->second.size == info.size) {
            next.insert(entries_.extract(old));
            continue;
        }
        auto content = store_.read(info.path);
        ++read_count_;
        if (!content || content->text.size() > max_note_bytes_)
            continue;
        // İkili not boş kayıtla tutulur: hiçbir terimle eşleşmez ve değişmedikçe yeniden okunmaz.
        if (core::looks_binary(content->text)) {
            next.emplace(info.path, Entry{info.mtime, info.revision, info.size, {}, {}, {}, {}});
            continue;
        }

        Entry e{info.mtime, info.revision, info.size, std::move(content->text), {}, {}, {}};
        auto folded = fold(e.text);
        e.folded = std::move(folded.text);
        e.byte_offsets.assign(folded.byte_offsets.begin(), folded.byte_offsets.end());
        e.line_starts.push_back(0);
        for (std::size_t i = 0; i < e.folded.size(); ++i)
            if (e.folded[i] == U'\n')
                e.line_starts.push_back(static_cast<std::uint32_t>(i + 1));
        next.emplace(info.path, std::move(e));
    }
    entries_ = std::move(next);
}

std::vector<ContentResult> ContentIndex::search(std::string_view query, std::size_t limit,
                                                const Accept& accept) const {
    auto terms = parse_terms(query);
    if (terms.empty() || limit == 0)
        return {};

    struct Candidate {
        const std::pair<const core::NotePath, Entry>* note;
        std::size_t hit_count;
    };
    std::vector<Candidate> candidates;
    for (const auto& kv : entries_) {
        if (accept && !accept(kv.first))
            continue;
        auto occ = scan(kv.second.folded, terms);
        if (!occ.empty())
            candidates.push_back({&kv, count_hit_lines(kv.second, occ)});
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.hit_count != b.hit_count)
            return a.hit_count > b.hit_count;
        if (a.note->second.mtime != b.note->second.mtime)
            return a.note->second.mtime > b.note->second.mtime;
        return a.note->first < b.note->first;
    });
    if (candidates.size() > limit)
        candidates.resize(limit);

    std::vector<ContentResult> results;
    for (const auto& c : candidates) {
        const Entry& e = c.note->second;
        ContentResult res{c.note->first, c.hit_count, {}};
        auto occ = scan(e.folded, terms);  // yalnız dönen sayfa için yeniden hesaplanır
        for (std::size_t i = 0; i < occ.size() && res.first_hits.size() < max_hits_;
             i = end_of_line_group(e, occ, i))
            res.first_hits.push_back(make_hit(e, occ, i));
        results.push_back(std::move(res));
    }
    return results;
}

std::vector<ContentCount> ContentIndex::count_matches(std::string_view query) const {
    const auto terms = parse_terms(query);
    if (terms.empty())
        return {};
    std::vector<ContentCount> counts;
    for (const auto& [path, entry] : entries_) {
        const auto occ = scan(entry.folded, terms);
        if (!occ.empty())
            counts.push_back({&path, count_hit_lines(entry, occ)});
    }
    return counts;
}

std::optional<Hit> ContentIndex::best_hit(const core::NotePath& path, std::string_view query) const {
    const auto found = entries_.find(path);
    if (found == entries_.end())
        return std::nullopt;
    const Entry& e = found->second;
    const auto terms = parse_terms(query);
    const auto occ = scan(e.folded, terms);
    if (occ.empty())
        return std::nullopt;

    std::size_t best = 0, best_terms = 0, best_count = 0;
    for (std::size_t i = 0; i < occ.size();) {
        const std::size_t group_end = end_of_line_group(e, occ, i);
        std::vector<bool> seen(terms.size());
        for (std::size_t k = i; k < group_end; ++k)
            seen[occ[k].term] = true;
        const auto distinct = static_cast<std::size_t>(std::count(seen.begin(), seen.end(), true));
        const std::size_t count = group_end - i;
        if (std::pair(distinct, count) > std::pair(best_terms, best_count)) {
            best = i;
            best_terms = distinct;
            best_count = count;
        }
        i = group_end;
    }
    return make_hit(e, occ, best);
}

}  // namespace noteapp::search
