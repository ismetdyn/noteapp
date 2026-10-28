#pragma once

#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "tui/tui_state.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>
#include <optional>
#include <string>
#include <vector>

namespace noteapp::tui {

// SPEC-tui: bu boyutun altında yerleşim anlamsızlaşır, ortalanmış bir uyarı gösterilir.
inline constexpr int kMinWidth = 60;
inline constexpr int kMinHeight = 15;

// Seçili notun önizlemesi; dosya okuması render'dan ayrı tutulur (render saf kalır).
struct Preview {
    enum class Kind { Empty, Text, Binary, Error };
    Kind kind = Kind::Empty;
    std::vector<std::string> lines;  // Kind::Text
    std::string error;               // Kind::Error: çevrilmiş hata metni
};

// Son okunan notu tutar: her yeniden çizimde diske gidilmez. Not değişince (yeniden tarama, editör dönüşü)
// invalidate() çağrılır.
class PreviewCache {
public:
    const Preview& get(const core::NoteStore& store, const Row* row);
    void invalidate() { path_.reset(); }

private:
    std::optional<core::NotePath> path_;
    Preview preview_;
};

struct StatusLine {
    std::string sync;     // üst çubuk: senkron durumu metni
    std::string message;  // geçici ileti; doluysa kısayol çubuğunun yerine geçer
};

// Tüm ekran: üst çubuk, sol liste, sağ önizleme, alt çubuk. `window` terminal boyutu (testte ekran boyutu).
ftxui::Element render(const TuiState& state, const Preview& preview, const StatusLine& status,
                      ftxui::Dimensions window);

}  // namespace noteapp::tui
