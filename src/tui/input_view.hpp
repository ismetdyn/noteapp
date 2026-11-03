#pragma once

#include "edit/input_line.hpp"

#include <ftxui/dom/elements.hpp>
#include <span>
#include <string>
#include <vector>

namespace noteapp::tui {

// Metin, imleç ve soluk öneri; `room` hücreye sığar, imleç görünür kalır. `width` kullanılan hücre sayısıdır.
struct InputCells {
    ftxui::Elements cells;
    int width = 0;
};
InputCells input_text_cells(const InputLine& line, std::span<const InputSpan> spans, const std::string& ghost,
                            int room);

// Alt satırdaki girdi: etiket, metin, sonra `counter` ya da `error`, sığan ipuçları.
ftxui::Element input_line_bar(const std::string& label, const InputLine& line,
                              std::span<const InputSpan> spans, const std::string& ghost,
                              const std::string& hints, int width, const std::string& counter = {},
                              const std::string& error = {});

// Süzgeç satırı, adaylar, ipucu: editörün `Ctrl+Space` listesiyle aynı pencere.
ftxui::Element completion_popup(const std::string& query, const std::vector<std::string>& items,
                                std::size_t selected);
ftxui::Element input_popup_box(const InputLine::Popup& popup);

}  // namespace noteapp::tui
