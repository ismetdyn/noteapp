#include "tui/modals.hpp"

#include "core/i18n.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <utility>

namespace noteapp::tui {

namespace {

// Yardım penceresinde kenarlık (2) + boşluk ve ipucu satırı (2) çıkınca en küçük pencerede kalan satır.
constexpr std::size_t kMinHelpVisible = static_cast<std::size_t>(kMinHeight) - 4;

}  // namespace

ConfirmState::Outcome ConfirmState::handle(Key key, std::string_view) {
    switch (key) {
    case Key::Left:
    case Key::Right:
    case Key::Tab:
    case Key::ShiftTab:
    case Key::Up:
    case Key::Down:
        yes_selected_ = !yes_selected_;
        return Outcome::Pending;
    case Key::Enter:
        return yes_selected_ ? Outcome::Confirmed : Outcome::Cancelled;
    case Key::Escape:
        return Outcome::Cancelled;
    default:
        return Outcome::Pending;  // "y" gibi kısayol yok: yanlışlıkla silmeyi önler
    }
}

std::size_t HelpState::max_scroll() const {
    return line_count_ > kMinHelpVisible ? line_count_ - kMinHelpVisible : 0;
}

bool HelpState::handle(Key key, std::string_view text) {
    switch (key) {
    case Key::Escape:
    case Key::F1:
        return false;
    case Key::Char:
        return text != "q";
    case Key::Up:
        scroll_ = scroll_ > 0 ? scroll_ - 1 : 0;
        return true;
    case Key::Down:
        scroll_ = std::min(scroll_ + 1, max_scroll());
        return true;
    case Key::PageUp:
        scroll_ = scroll_ > kPageLines ? scroll_ - kPageLines : 0;
        return true;
    case Key::PageDown:
        scroll_ = std::min(scroll_ + kPageLines, max_scroll());
        return true;
    default:
        return true;
    }
}

void UpdateDialogState::show(app::UpdateView view) {
    if (view.actions != view_.actions) {
        const auto close = std::ranges::find(view.actions, app::UpdateAction::Close);
        index_ = close != view.actions.end() ? static_cast<std::size_t>(close - view.actions.begin()) : 0;
    }
    view_ = std::move(view);
}

app::UpdateAction UpdateDialogState::selected() const {
    return index_ < view_.actions.size() ? view_.actions[index_] : app::UpdateAction::Close;
}

std::optional<app::UpdateAction> UpdateDialogState::handle(Key key) {
    const std::size_t count = view_.actions.size();
    switch (key) {
    case Key::Left:
    case Key::Up:
    case Key::ShiftTab:
        if (count > 0)
            index_ = (index_ + count - 1) % count;
        return std::nullopt;
    case Key::Right:
    case Key::Down:
    case Key::Tab:
        if (count > 0)
            index_ = (index_ + 1) % count;
        return std::nullopt;
    case Key::Enter:
        return selected();
    case Key::Escape:
        return app::UpdateAction::Close;
    default:
        return std::nullopt;  // harf kısayolu yok: yanlışlıkla kurulumu önler
    }
}

std::vector<std::string> help_lines() {
    std::vector<std::string> lines;
    const std::string body = core::tr("tui.help_body");
    std::size_t start = 0;
    while (start < body.size()) {
        std::size_t end = body.find('\n', start);
        if (end == std::string::npos)
            end = body.size();
        lines.push_back(body.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

}  // namespace noteapp::tui
