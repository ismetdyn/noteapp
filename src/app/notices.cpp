#include "app/notices.hpp"

#include <algorithm>
#include <utility>

namespace noteapp::app {

void NoticeQueue::post(NoticeKind kind, std::string text, Clock::time_point now, std::optional<std::string> folder) {
    const auto same = std::ranges::find_if(
        items_, [&](const Notice& notice) { return notice.kind == kind && notice.text == text; });
    if (same != items_.end()) {
        Notice repeated = std::move(*same);
        items_.erase(same);
        ++repeated.repeats;
        repeated.posted = now;
        if (folder)
            repeated.folder = std::move(folder);
        items_.push_back(std::move(repeated));  // en yeni sonda
        return;
    }
    items_.push_back(Notice{.id = next_id_++,
                            .kind = kind,
                            .text = std::move(text),
                            .posted = now,
                            .repeats = 1,
                            .folder = std::move(folder)});
}

void NoticeQueue::dismiss(std::uint64_t id) {
    std::erase_if(items_, [id](const Notice& notice) { return notice.id == id; });
}

void NoticeQueue::dismiss_all(NoticeKind kind) {
    std::erase_if(items_, [kind](const Notice& notice) { return notice.kind == kind; });
}

bool NoticeQueue::expires(const Notice& notice) const {
    return notice.kind != NoticeKind::Error && lifetime_.count() > 0;
}

std::optional<NoticeQueue::Clock::time_point> NoticeQueue::due(const Notice& notice) const {
    if (!expires(notice))
        return std::nullopt;
    // Kalan aralık milisaniyeye aşağı yuvarlanır: karşılaştırma ms -> ns dönüşümünde taşmaz.
    const auto room = std::chrono::floor<std::chrono::milliseconds>(Clock::time_point::max() - notice.posted);
    if (lifetime_ >= room)
        return std::nullopt;
    return notice.posted + lifetime_;
}

bool NoticeQueue::expire(Clock::time_point now) {
    return std::erase_if(items_, [&](const Notice& notice) {
               const auto at = due(notice);
               return at && now >= *at;
           }) > 0;
}

std::optional<NoticeQueue::Clock::time_point> NoticeQueue::next_deadline() const {
    std::optional<Clock::time_point> earliest;
    for (const Notice& notice : items_) {
        const auto at = due(notice);
        if (at && (!earliest || *at < *earliest))
            earliest = at;
    }
    return earliest;
}

}  // namespace noteapp::app
