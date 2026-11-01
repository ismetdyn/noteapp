#include "tui/note_find.hpp"

#include "search/fold.hpp"

#include <algorithm>

namespace noteapp::tui {

namespace {

void append_line_matches(std::string_view line, std::size_t line_offset, const std::u32string& folded_query,
                         std::vector<ByteRange>& out) {
    const search::Folded folded = search::fold(line);
    const std::size_t length = folded_query.size();
    for (std::size_t at = folded.text.find(folded_query); at != std::u32string::npos;
         at = folded.text.find(folded_query, at + length)) {
        const std::size_t end_index = at + length;
        const std::size_t end =
            end_index < folded.byte_offsets.size() ? folded.byte_offsets[end_index] : line.size();
        out.push_back({line_offset + folded.byte_offsets[at], line_offset + end});
    }
}

}  // namespace

std::vector<ByteRange> find_folded(std::string_view text, std::string_view query) {
    std::vector<ByteRange> matches;
    if (query.empty())
        return matches;
    const std::u32string folded_query = search::fold(query).text;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        append_line_matches(text.substr(start, end - start), start, folded_query, matches);
        if (newline == std::string_view::npos)
            break;
        start = newline + 1;
    }
    return matches;
}

std::string match_label(std::optional<std::size_t> current, std::size_t count) {
    if (!current || count == 0)
        return {};
    return std::to_string(*current + 1) + "/" + std::to_string(count);
}

void PreviewFind::set_query(std::string query, std::span<const std::string> lines) {
    query_ = std::move(query);
    index_ = 0;
    recompute(lines);
}

void PreviewFind::recompute(std::span<const std::string> lines) {
    matches_.clear();
    if (!query_.empty()) {
        const std::u32string folded_query = search::fold(query_).text;
        std::vector<ByteRange> on_line;
        for (std::size_t line = 0; line < lines.size(); ++line) {
            on_line.clear();
            append_line_matches(lines[line], 0, folded_query, on_line);
            for (const ByteRange& range : on_line)
                matches_.push_back({line, range});
        }
    }
    index_ = matches_.empty() ? 0 : std::min(index_, matches_.size() - 1);
}

void PreviewFind::clear() {
    query_.clear();
    matches_.clear();
    index_ = 0;
}

void PreviewFind::next() {
    if (!matches_.empty())
        index_ = (index_ + 1) % matches_.size();
}

void PreviewFind::previous() {
    if (!matches_.empty())
        index_ = (index_ + matches_.size() - 1) % matches_.size();
}

std::optional<std::size_t> PreviewFind::current() const {
    return matches_.empty() ? std::nullopt : std::optional(index_);
}

std::optional<std::size_t> PreviewFind::current_line() const {
    return matches_.empty() ? std::nullopt : std::optional(matches_[index_].line);
}

std::vector<ByteRange> PreviewFind::ranges_on_line(std::size_t line) const {
    std::vector<ByteRange> ranges;
    const auto first = std::ranges::lower_bound(matches_, line, {}, &Match::line);
    for (auto it = first; it != matches_.end() && it->line == line; ++it)
        ranges.push_back(it->range);
    return ranges;
}

std::optional<ByteRange> PreviewFind::current_range_on_line(std::size_t line) const {
    if (matches_.empty() || matches_[index_].line != line)
        return std::nullopt;
    return matches_[index_].range;
}

}  // namespace noteapp::tui
