#include "core/config.hpp"
#include "core/console.hpp"
#include "core/editor.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/note_store.hpp"
#include "detect/detect.hpp"
#include "search/content_index.hpp"
#include "tui/save_dialog.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/terminal.hpp>
#include <iterator>
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
        {&Event::TabReverse, Key::ShiftTab},
        {&Event::ArrowLeft, Key::Left},
        {&Event::ArrowRight, Key::Right},
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

std::string read_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string to_utf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace

int main() {
    namespace core = noteapp::core;
    namespace detect = noteapp::detect;
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

    core::NoteStore store(config.notes_dir, config.tuning.slug_max_length);
    // NoteStore::list() iç önbelleği günceller (iş parçacığı güvenli değil): yeniden tarama iş parçacığı ile
    // editör dönüşü yenilemesi aynı anda çağırmasın.
    std::mutex list_mutex;
    const auto list_notes = [&] {
        const std::scoped_lock lock(list_mutex);
        return store.list();
    };
    tui::TuiState state;
    tui::PreviewCache preview;
    tui::StatusLine status{core::tr("tui.sync_off"), ""};
    auto listing = list_notes();
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

    // Açık kaydet diyaloğu ve ona ait geçici dosya/içerik; diyalog açıkken tüm tuşlar ona gider.
    std::optional<tui::SaveDialogState> dialog;
    std::filesystem::path draft_file;
    std::string draft_content;

    const auto view = ftxui::Renderer([&] {
        return tui::render(state, preview.get(store, state.selected_row()), status, ftxui::Terminal::Size(),
                           dialog ? &*dialog : nullptr);
    });

    // Editör/kayıt sonrası liste, önizleme ve (kuruluysa) içerik indeksi güncellenir. `listing`'e dokunulmaz:
    // onu yalnız yeniden tarama iş parçacığı yazar; bir sonraki taramada yinelenen (zararsız) güncelleme
    // olur.
    const auto refresh_notes = [&] {
        preview.invalidate();
        if (content_index)
            content_index->refresh();
        state.set_notes(list_notes());
    };

    const auto show_error = [&](const core::Error& error) { status.message = core::format_error(error); };

    // Terminali editöre bırakır; dönünce FTXUI ekranı geri yükler.
    const auto edit_file = [&](const std::filesystem::path& file) {
        std::expected<void, core::Error> result;
        app.WithRestoredIO([&] { result = core::run_editor(config, file); })();
        return result;
    };

    const auto open_selected = [&] {
        const tui::Row* row = state.selected_row();
        if (!row)
            return;
        const auto edited = edit_file(config.notes_dir / row->path.value());
        refresh_notes();
        if (!edited)
            show_error(edited.error());
    };

    // Yeni not akışı (SPEC-tui): geçici dosya -> editör -> boşsa sessiz iptal -> değilse kaydet diyaloğu.
    const auto new_note = [&](const std::string& prefill) {
        const tui::Row* selected = state.selected_row();
        const std::string folder =
            tui::initial_folder(selected ? &selected->path : nullptr, config.default_folder);
        const auto temp = core::create_private_temp_file(config.default_type, prefill);
        if (!temp)
            return show_error(temp.error());
        const auto edited = edit_file(*temp);
        const std::string content = read_file(*temp);
        std::error_code ec;
        if (!edited) {
            show_error(edited.error());
            if (tui::is_blank(content))
                std::filesystem::remove(*temp, ec);
            else
                status.message +=
                    "  " + core::tr("tui.temp_kept", {{"path", to_utf8(*temp)}});  // veri kaybı yok
            return;
        }
        if (tui::is_blank(content)) {
            std::filesystem::remove(*temp, ec);
            return;
        }
        const detect::Options options{config.default_type, config.tuning.min_confidence,
                                      config.tuning.max_scan_bytes};
        draft_file = *temp;
        draft_content = content;
        dialog.emplace(tui::SaveDialogInit{content, folder, store.folders(), config.types,
                                           detect::suggest(content, options), config.tuning.slug_max_length,
                                           std::chrono::system_clock::now()});
    };

    // Diyalog açıkken tuş işleme. Kayıt hatasında geçici dosya durur, diyalog açık kalır (ad/klasör
    // düzeltilsin).
    const auto handle_dialog_key = [&](const Key key, const std::string& text) {
        switch (dialog->handle(key, text)) {
        case tui::SaveDialogState::Outcome::Editing:
            break;
        case tui::SaveDialogState::Outcome::Discard: {
            std::error_code ec;
            std::filesystem::remove(draft_file, ec);
            dialog.reset();
            break;
        }
        case tui::SaveDialogState::Outcome::Submit: {
            const auto saved = tui::save_new_note(store, draft_file, dialog->request(), draft_content);
            if (!saved) {
                dialog->set_error(core::format_error(saved.error()) + "  " +
                                  core::tr("tui.temp_kept", {{"path", to_utf8(draft_file)}}));
                break;
            }
            dialog.reset();
            refresh_notes();
            state.reveal(*saved);
            status.message = core::tr("tui.saved", {{"path", saved->string()}});
            break;
        }
        }
    };

    // Ctrl-C'yi de kendimiz işleriz: FTXUI'ye bırakılırsa döngüden sonra SIGINT'i yeniden yükseltir ve süreç
    // yıkıcılar (kod sayfası geri yüklemesi dahil) çalışmadan biter.
    const auto on_event = [&](const ftxui::Event& event) {
        std::string text;
        const auto key = to_key(event, text);
        if (!key)
            return false;
        if (dialog) {
            // Çıkışta geçici dosya silinmez: yazılan metin state dizininde kalır.
            if (*key == Key::CtrlC)
                app.Exit();
            else
                handle_dialog_key(*key, text);
            sync_preview();
            return true;
        }
        status.message.clear();
        switch (state.handle(*key, text)) {
        case tui::Intent::None:
            break;
        case tui::Intent::Quit:
            app.Exit();
            break;
        case tui::Intent::OpenSelected:
            open_selected();
            break;
        case tui::Intent::NewNote:
            new_note("");
            break;
        case tui::Intent::CreateFromQuery:
            new_note(state.query() + "\n");  // nvALT: sorgu yeni notun ilk satırı olur
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
            auto latest = list_notes();
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
