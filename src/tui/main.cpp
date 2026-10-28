#include "core/config.hpp"
#include "core/console.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/note_store.hpp"
#include "search/content_index.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/terminal.hpp>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace {

using noteapp::tui::Key;

// İçerik aramasında listelenecek en çok not; ekran bundan fazlasını zaten gösteremez.
constexpr std::size_t kContentResultLimit = 1000;

// FTXUI olayını modelin tuşuna çevirir; tanınmayan olay (fare, boyut vb.) nullopt.
std::optional<Key> to_key(const ftxui::Event& event, std::string& text) {
    using ftxui::Event;
    if (event.is_character()) {
        text = event.character();
        return Key::Char;
    }
    const std::pair<const Event*, Key> table[] = {
        {&Event::Backspace, Key::Backspace},
        {&Event::ArrowUp, Key::Up},
        {&Event::ArrowDown, Key::Down},
        {&Event::CtrlP, Key::CtrlP},
        {&Event::CtrlN, Key::CtrlN},
        {&Event::PageUp, Key::PageUp},
        {&Event::PageDown, Key::PageDown},
        {&Event::Tab, Key::Tab},
        {&Event::Return, Key::Enter},
        {&Event::Escape, Key::Escape},
        {&Event::CtrlO, Key::CtrlO},
        {&Event::CtrlR, Key::CtrlR},
        {&Event::CtrlD, Key::CtrlD},
        {&Event::CtrlS, Key::CtrlS},
        {&Event::F1, Key::F1},
        {&Event::F2, Key::F2},
        {&Event::CtrlC, Key::CtrlC},
    };
    for (const auto& [candidate, key] : table)
        if (event == *candidate)
            return key;
    return std::nullopt;
}

}  // namespace

int main() {
    namespace core = noteapp::core;
    namespace tui = noteapp::tui;
    // FTXUI konsolu UTF-8'e çeker ama geri almaz; eski kod sayfasını biz saklayıp geri yükleriz.
    const core::ConsoleUtf8Scope utf8_console;
    // Dil ve log eşiği config'ten gelir; yükleme uyarıları log'a düşer (TUI ekranına yazılmaz).
    const core::LoadedConfig loaded = core::load_config();
    const core::Config& config = loaded.config;
    core::init_i18n(config.language);
    core::init_log(config.tuning.log_max_bytes);
    for (const auto& warning : loaded.warnings)
        core::log_warn(core::format_error(warning));

    const core::NoteStore store(config.notes_dir, config.tuning.slug_max_length);
    tui::TuiState state;
    tui::PreviewCache preview;
    tui::StatusLine status{core::tr("tui.sync_off"), ""};
    auto listing = store.list();
    state.set_notes(listing);

    // İçerik indeksi ilk Tab'da kurulur: ad modunda kalan kullanıcı tüm notları okumanın bedelini ödemez.
    // Yalnız UI iş parçacığından dokunulur (ContentIndex tek iş parçacıklıdır).
    std::optional<noteapp::search::ContentIndex> content_index;
    state.set_content_searcher([&](std::string_view query) {
        if (!content_index) {
            content_index.emplace(store, config.tuning.max_note_bytes);
            content_index->refresh();
        }
        return content_index->search(query, kContentResultLimit);
    });

    auto app = ftxui::App::Fullscreen();

    // Önizleme satır sayısı modele bildirilir ki PgDn son satırı aşmasın.
    const auto sync_preview = [&] {
        state.set_preview_line_count(preview.get(store, state.selected_row()).lines.size());
    };
    sync_preview();

    const auto view = ftxui::Renderer([&] {
        return tui::render(state, preview.get(store, state.selected_row()), status, ftxui::Terminal::Size());
    });

    // Ctrl-C'yi de kendimiz işleriz: FTXUI'ye bırakılırsa döngüden sonra SIGINT'i yeniden yükseltir ve süreç
    // yıkıcılar (kod sayfası geri yüklemesi dahil) çalışmadan biter.
    const auto on_event = [&](const ftxui::Event& event) {
        std::string text;
        const auto key = to_key(event, text);
        if (!key)
            return false;
        status.message.clear();
        switch (state.handle(*key, text)) {
        case tui::Intent::None:
            break;
        case tui::Intent::Quit:
            app.Exit();
            break;
        default:
            // T21 (editör), T22 (diyalog/yardım), T23 (ayarlar), T28 (senkron) bağlanana dek yer tutucu.
            status.message = core::tr("tui.not_yet");
            break;
        }
        sync_preview();
        return true;
    };

    // Yeniden tarama arka planda yalnız stat yapar; değişiklik varsa model güncellemesi UI iş parçacığına
    // Post edilir (Post aynı zamanda yeniden çizimi tetikler). Değişmediyse hiçbir şey gönderilmez.
    // jthread, state/preview'dan sonra bildirildiği için onlardan önce yıkılır (durdur + birleş).
    std::jthread rescan([&](const std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any wake;
        const auto period = std::chrono::seconds(config.tuning.rescan_seconds);
        while (true) {
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, period, [] { return false; });  // durdurma isteğinde erken döner
            if (stop.stop_requested())
                return;
            auto latest = store.list();
            if (tui::same_listing(listing, latest))
                continue;
            listing = latest;  // yalnız bu iş parçacığı okur/yazar
            app.Post([&state, &preview, &content_index, &sync_preview, latest = std::move(latest)]() mutable {
                preview.invalidate();
                if (content_index)
                    content_index->refresh();  // yalnız mtime/boyutu değişenler yeniden okunur
                state.set_notes(std::move(latest));
                sync_preview();
            });
        }
    });

    app.Loop(ftxui::CatchEvent(view, on_event));
}
