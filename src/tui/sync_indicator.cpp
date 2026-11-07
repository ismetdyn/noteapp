#include "tui/sync_indicator.hpp"

#include "core/i18n.hpp"

#include <cstddef>

namespace noteapp::tui {

namespace {

// Üst çubukta hata nedenine ayrılan en çok kod noktası: 60 sütunda sayaç ve mod kesilmesin. Tam metin
// durum satırındadır.
constexpr std::size_t kShortReasonCodePoints = 16;

// UTF-8'i kod noktası sınırında keser (devam baytı 10xxxxxx).
std::string shorten(const std::string& text, std::size_t limit) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80)
            continue;
        if (count++ == limit)
            return text.substr(0, i) + "…";
    }
    return text;
}

}  // namespace

std::string sync_text(const SyncIndicator& indicator) {
    switch (indicator.state()) {
    case SyncIndicator::State::Disabled:
        return core::tr("app.sync_off");
    case SyncIndicator::State::Running:
        return "⟳";
    case SyncIndicator::State::Ok:
        return "✓ " + indicator.success_time();
    case SyncIndicator::State::Failed:
        return "⚠ " + shorten(indicator.error_text(), kShortReasonCodePoints);
    }
    return {};
}

}  // namespace noteapp::tui
