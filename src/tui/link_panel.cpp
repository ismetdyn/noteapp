#include "tui/link_panel.hpp"

#include <algorithm>

namespace noteapp::tui {

std::vector<WikiSpan> wiki_spans(std::string_view line) {
    std::vector<WikiSpan> spans;
    for (std::size_t at = 0;;) {
        const std::size_t open = line.find("[[", at);
        if (open == std::string_view::npos)
            break;
        const std::size_t close = line.find("]]", open + 2);
        if (close == std::string_view::npos)
            break;
        const std::string_view target = core::link_target(line.substr(open + 2, close - open - 2));
        if (!target.empty())
            spans.push_back({open, close + 2, target});
        at = close + 2;
    }
    return spans;
}

std::vector<WikiSpan> wiki_spans(std::span<const StyledSpan> spans, std::span<const std::string> targets) {
    std::vector<WikiSpan> links;
    std::size_t previous = 0;
    for (const StyledSpan& span : spans) {
        const std::size_t link = span.look.wiki_link;
        if (link == 0)
            continue;
        if (link == previous && links.back().end == span.begin)
            links.back().end = span.end;
        else
            links.push_back({span.begin, span.end, targets[link - 1]});
        previous = link;
    }
    return links;
}

void LinkPanel::set(core::NoteLinks links, bool same_note) {
    links_ = std::move(links);
    lines_ = core::link_lines(links_);
    const std::size_t wanted = same_note ? selected_ : 0;
    selected_ = 0;
    if (lines_.empty())
        return;
    selected_ = std::min(wanted, lines_.size() - 1);
    if (lines_[selected_].is_header)  // başlıktan sonra hep öğe vardır
        ++selected_;
}

const core::NotePath* LinkPanel::selected_path() const {
    return lines_.empty() ? nullptr : core::link_path(links_, lines_[selected_]);
}

void LinkPanel::move(int delta) {
    if (lines_.empty())
        return;
    const auto last = static_cast<int>(lines_.size()) - 1;
    int target = std::clamp(static_cast<int>(selected_) + delta, 0, last);
    if (lines_[static_cast<std::size_t>(target)].is_header)
        target += delta < 0 && target > 0 ? -1 : 1;
    selected_ = static_cast<std::size_t>(target);
}

void LinkPanel::select(std::size_t line) {
    if (line < lines_.size() && !lines_[line].is_header)
        selected_ = line;
}

std::size_t LinkPanel::item_count() const {
    return links_.incoming.size() + links_.outgoing.size() + links_.related.size() + links_.missing.size();
}

std::size_t LinkPanel::selected_ordinal() const {
    if (lines_.empty())
        return 0;
    const core::LinkLine& line = lines_[selected_];
    std::size_t before = 0;
    if (line.group != core::LinkGroup::Incoming)
        before += links_.incoming.size();
    if (line.group == core::LinkGroup::Related || line.group == core::LinkGroup::Missing)
        before += links_.outgoing.size();
    if (line.group == core::LinkGroup::Missing)
        before += links_.related.size();
    return before + line.index + 1;
}

int LinkPanel::visible_rows(int pane_rows) const {
    const int bound = std::clamp(pane_rows / 3, kMinRows, kMaxRows);
    return static_cast<int>(std::min(lines_.size(), static_cast<std::size_t>(bound)));
}

int LinkPanel::reserved_rows(int pane_rows) const {
    return lines_.empty() && links_.tags.empty() ? 0 : visible_rows(pane_rows) + 1;
}

std::size_t LinkPanel::top(std::size_t visible) const {
    return selected_ < visible ? 0 : selected_ - visible + 1;
}

}  // namespace noteapp::tui
