// TUI açılışının vekili (T35): `notes` gerçek terminal ister, testte çalıştırılamaz. Taze bir süreçte
// main.cpp'nin açılış yolu koşar: config + dil, soğuk NoteStore::list(), set_notes, seçili notun önizlemesi
// ve ilk çizim. Süreç içi süre argv[1] dosyasına yazılır; çağıran testi süreç süresini ayrıca ölçer.
#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/note_store.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <chrono>
#include <fstream>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <string>

int main(int argc, char* argv[]) {
    namespace core = noteapp::core;
    namespace tui = noteapp::tui;
    if (argc != 2)
        return 2;
    constexpr int kWidth = 120;
    constexpr int kHeight = 40;

    const auto start = std::chrono::steady_clock::now();
    const core::LoadedConfig loaded = core::load_config();
    core::init_i18n(loaded.config.language);
    const core::NoteStore store = core::NoteStore::for_config(loaded.config);
    tui::TuiState state;
    state.set_notes(store.list());
    tui::PreviewCache preview;
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(screen,
                  tui::render(state, preview.get(store, state.selected_row()), {"", ""}, {kWidth, kHeight}));
    const std::string frame = screen.ToString();
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    std::ofstream(argv[1]) << ms << ' ' << state.total() << '\n';
    return frame.empty() ? 1 : 0;
}
