#pragma once

#include "tui/tui_state.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::tui {

// Evet/hayır onayı (Ctrl-D silme). Varsayılan seçim her zaman "hayır": yalnız açıkça "evet"e gidip Enter
// basmak onaylar (SPEC.md §9). FTXUI'ye bağlı değildir.
class ConfirmState {
public:
    enum class Outcome { Pending, Confirmed, Cancelled };

    explicit ConfirmState(std::string message) : message_(std::move(message)) {}

    Outcome handle(Key key, std::string_view text = {});

    const std::string& message() const { return message_; }
    bool yes_selected() const { return yes_selected_; }

private:
    std::string message_;
    bool yes_selected_ = false;
};

// F1 yardım ekranının kaydırma durumu. Satırlar katalogdan gelir (help_lines); görünür satır sayısı
// pencereye bağlıdır, bu yüzden üst sınır en küçük pencereye (kMinHeight) göre hesaplanır, çizim kenetler.
class HelpState {
public:
    static constexpr std::size_t kPageLines = 5;

    explicit HelpState(std::size_t line_count) : line_count_(line_count) {}

    // false: yardım kapandı (Esc / F1 / q).
    bool handle(Key key, std::string_view text = {});

    std::size_t scroll() const { return scroll_; }
    std::size_t max_scroll() const;

private:
    std::size_t line_count_;
    std::size_t scroll_ = 0;
};

// Katalogdaki yardım metni, satır satır (tek anahtar: tui.help_body).
std::vector<std::string> help_lines();

}  // namespace noteapp::tui
