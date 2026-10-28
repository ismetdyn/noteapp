#include "search/content_index.hpp"

#include "search/fold.hpp"

#include <algorithm>
#include <cstddef>

namespace noteapp::search {

namespace {

constexpr std::size_t kMaxHits = 3;
constexpr std::size_t kSnippetCodePoints = 120;
constexpr std::string_view kEllipsis = "…";

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Boşlukla ayrılmış terimler; "..." arası tek ifade. Kapanmamış tırnak sonuna kadar ifade sayılır.
std::vector<std::u32string> parse_terms(std::string_view query) {
    std::vector<std::u32string> terms;
    auto push = [&](std::string_view raw) {
        if (raw.empty())
            return;
        auto folded = fold(raw).text;
        if (!folded.empty())
            terms.push_back(std::move(folded));
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

struct Occurrence {
    std::size_t pos;
    std::size_t len;
};

// Tüm terimlerin geçişleri (konuma göre sıralı); bir terim hiç yoksa boş döner (AND).
std::vector<Occurrence> scan(const std::u32string& text, const std::vector<std::u32string>& terms) {
    std::vector<Occurrence> all;
    for (const auto& term : terms) {
        auto pos = text.find(term);
        if (pos == std::u32string::npos)
            return {};
        for (; pos != std::u32string::npos; pos = text.find(term, pos + 1))
            all.push_back({pos, term.size()});
    }
    std::sort(all.begin(), all.end(), [](const Occurrence& a, const Occurrence& b) { return a.pos < b.pos; });
    return all;
}

}  // namespace

ContentIndex::ContentIndex(const core::NoteStore& store, std::size_t max_note_bytes)
    : store_(store), max_note_bytes_(max_note_bytes) {}

void ContentIndex::refresh() {
    std::map<core::NotePath, Entry> next;
    for (const auto& info : store_.list()) {
        if (info.binary || info.size > max_note_bytes_)
            continue;
        auto old = entries_.find(info.path);
        if (old != entries_.end() && old->second.mtime == info.mtime && old->second.size == info.size) {
            next.insert(entries_.extract(old));
            continue;
        }
        auto content = store_.read(info.path);
        ++read_count_;
        if (!content || content->text.size() > max_note_bytes_)
            continue;

        Entry e{info.mtime, info.size, std::move(content->text), {}, {}, {}};
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

std::vector<ContentResult> ContentIndex::search(std::string_view query, std::size_t limit) const {
    auto terms = parse_terms(query);
    if (terms.empty() || limit == 0)
        return {};

    // 1 tabanlı satır numarası: kod noktası konumunu içeren satır.
    auto line_of = [](const Entry& e, std::size_t pos) {
        return static_cast<std::size_t>(std::upper_bound(e.line_starts.begin(), e.line_starts.end(), pos) -
                                        e.line_starts.begin());
    };

    struct Candidate {
        const std::pair<const core::NotePath, Entry>* note;
        std::size_t hit_count;
    };
    std::vector<Candidate> candidates;
    for (const auto& kv : entries_) {
        auto occ = scan(kv.second.folded, terms);
        if (occ.empty())
            continue;
        std::size_t count = 0, last_line = 0;  // satır no 1'den başlar, 0 "henüz yok"
        for (const auto& o : occ) {
            auto line = line_of(kv.second, o.pos);
            if (line != last_line)
                ++count;
            last_line = line;
        }
        candidates.push_back({&kv, count});
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
        const auto n = e.folded.size();
        auto byte_at = [&](std::size_t cp) -> std::size_t {
            return cp < n ? e.byte_offsets[cp] : e.text.size();
        };
        ContentResult res{c.note->first, c.hit_count, {}};

        auto occ = scan(e.folded, terms);  // yalnız dönen sayfa için yeniden hesaplanır
        for (std::size_t i = 0; i < occ.size() && res.first_hits.size() < kMaxHits;) {
            const auto line = line_of(e, occ[i].pos);
            const std::size_t ls = e.line_starts[line - 1];
            const std::size_t next_ls = line < e.line_starts.size() ? e.line_starts[line] : n + 1;
            std::size_t le = next_ls - 1;  // '\n' hariç
            if (le > ls && e.folded[le - 1] == U'\r')
                --le;

            std::size_t j = i;
            while (j < occ.size() && occ[j].pos < next_ls)
                ++j;  // bu satırdaki geçişler [i, j)

            // Kesit penceresi: ilk geçişin ortalandığı ~120 kod noktası, satır sınırına kenetli.
            std::size_t start = ls, end = le;
            if (le - ls > kSnippetCodePoints) {
                const auto mid = occ[i].pos + occ[i].len / 2;
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
            for (std::size_t k = i; k < j; ++k) {
                const auto s = std::max(occ[k].pos, start), t = std::min(occ[k].pos + occ[k].len, end);
                if (s < t)
                    hit.ranges.emplace_back(prefix + byte_at(s) - base, prefix + byte_at(t) - base);
            }
            res.first_hits.push_back(std::move(hit));
            i = j;
        }
        results.push_back(std::move(res));
    }
    return results;
}

}  // namespace noteapp::search
