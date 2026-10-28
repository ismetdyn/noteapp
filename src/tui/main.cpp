#include "tui/main_view.hpp"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#ifdef _WIN32
#ifndef NOMINMAX  // MinGW'nin libstdc++'ı zaten tanımlar; MSVC tanımlamaz.
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
// FTXUI'nin Screen kurucusu konsolu UTF-8'e çeker ama çıkışta geri almaz; kullanıcının konsolunu (ör. cmd'de
// 857) değişmiş bırakmamak için eski kod sayfalarını App kurulmadan önce saklar, çıkışta geri yükleriz.
class ConsoleUtf8Scope {
public:
    ConsoleUtf8Scope() : input_cp_(GetConsoleCP()), output_cp_(GetConsoleOutputCP()) {
        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);
    }
    ~ConsoleUtf8Scope() {
        // 0: süreç bir konsola bağlı değildi; geri yüklenecek bir şey yok.
        if (input_cp_ != 0) {
            SetConsoleCP(input_cp_);
        }
        if (output_cp_ != 0) {
            SetConsoleOutputCP(output_cp_);
        }
    }
    ConsoleUtf8Scope(const ConsoleUtf8Scope&) = delete;
    ConsoleUtf8Scope& operator=(const ConsoleUtf8Scope&) = delete;

private:
    UINT input_cp_;
    UINT output_cp_;
};
#endif

}  // namespace

int main() {
#ifdef _WIN32
    const ConsoleUtf8Scope utf8_console;
#endif
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
