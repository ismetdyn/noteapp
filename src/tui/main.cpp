#include "core/config.hpp"
#include "core/console.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "tui/main_view.hpp"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

int main() {
    // FTXUI konsolu UTF-8'e çeker ama geri almaz; eski kod sayfasını biz saklayıp geri yükleriz.
    const noteapp::core::ConsoleUtf8Scope utf8_console;
    // Dil ve log eşiği config'ten gelir; yükleme uyarıları log'a düşer (TUI ekranına yazılmaz).
    const noteapp::core::LoadedConfig loaded = noteapp::core::load_config();
    noteapp::core::init_i18n(loaded.config.language);
    noteapp::core::init_log(loaded.config.tuning.log_max_bytes);
    for (const auto& warning : loaded.warnings)
        noteapp::core::log_warn(noteapp::core::format_error(warning));
    auto app = ftxui::App::Fullscreen();
    const auto view = ftxui::Renderer([] { return noteapp::tui::main_view(); });
    // Ctrl-C'yi de kendimiz işleriz: FTXUI'ye bırakılırsa döngüden sonra SIGINT'i yeniden yükseltir ve süreç
    // yıkıcılar (kod sayfası geri yüklemesi dahil) çalışmadan biter.
    const auto quit_on_key = [&app](const ftxui::Event& event) {
        if (event == ftxui::Event::Character('q') || event == ftxui::Event::Escape ||
            event == ftxui::Event::CtrlC) {
            app.Exit();
            return true;
        }
        return false;
    };
    app.Loop(ftxui::CatchEvent(view, quit_on_key));
}
