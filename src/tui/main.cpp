#include "core/config.hpp"
#include "core/console.hpp"
#include "core/editor.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/note_store.hpp"
#include "detect/detect.hpp"
#include "search/content_index.hpp"
#include "sync/auto_sync.hpp"
#include "sync/sync_service.hpp"
#include "tui/modals.hpp"
#include "tui/save_dialog.hpp"
#include "tui/settings.hpp"
#include "tui/sync_indicator.hpp"
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
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

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

// Editör dönüşünde "dosya değişti mi" kararı: içeriği okumadan mtime + boyut. Okunamazsa boş damga.
using FileStamp = std::pair<std::filesystem::file_time_type, std::uintmax_t>;
FileStamp stamp_of(const std::filesystem::path& file) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(file, ec);
    const auto size = std::filesystem::file_size(file, ec);
    return ec ? FileStamp{} : FileStamp{time, size};
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
    // Ayarlar ekranı kaydedince canlı güncellenir (yalnız UI iş parçacığı yazar).
    core::Config config = loaded.config;
    core::init_i18n(config.language);
    core::init_log(config.tuning.log_max_bytes);
    for (const auto& warning : loaded.warnings)
        core::log_warn(core::format_error(warning));

    // notes_dir ayarı değişince yeniden kurulur (list_mutex altında).
    std::optional<core::NoteStore> store;
    store.emplace(config.notes_dir, config.tuning.slug_max_length);
    // Yeniden tarama iş parçacığı listelerken UI iş parçacığı store'u yeniden kurmasın (notes_dir değişimi).
    std::mutex list_mutex;
    const auto list_notes = [&] {
        const std::scoped_lock lock(list_mutex);
        return store->list();
    };
    tui::TuiState state;
    tui::PreviewCache preview;
    tui::StatusLine status{core::tr("tui.sync_off"), ""};
    tui::SyncIndicator sync_indicator;
    auto listing = list_notes();
    state.set_notes(listing);

    // İçerik indeksi ilk Tab'da kurulur: ad modunda kalan kullanıcı tüm notları okumanın bedelini ödemez.
    // Yalnız UI iş parçacığından dokunulur (ContentIndex tek iş parçacıklıdır).
    std::optional<noteapp::search::ContentIndex> content_index;
    state.set_content_searcher([&](std::string_view query) {
        if (!content_index) {
            content_index.emplace(*store, config.tuning.max_note_bytes);
            content_index->refresh();
        }
        return content_index->search(query, kContentResultLimit);
    });

    auto app = ftxui::App::Fullscreen();

    // Arka plan senkronu yalnız sync.enabled iken vardır (kapatınca yıkılır; depo olduğu gibi kalır). app'ten
    // sonra bildirildiği için ondan önce yıkılır. Yalnız UI iş parçacığı oluşturur/yıkar.
    std::optional<noteapp::sync::AutoSync> auto_sync;
    const auto show_sync = [&] { status.sync = sync_indicator.text(); };
    const auto notify_changed = [&] {
        if (auto_sync)
            auto_sync->notify_changed();
    };
    // AutoSync geri çağrısı İŞÇİ iş parçacığından gelir: durum UI iş parçacığına Post edilir. Kapatıldıktan
    // sonra gelen geç bildirim yok sayılır.
    const auto start_auto_sync = [&] {
        sync_indicator.set_status(noteapp::sync::SyncService(config).status(), false);
        show_sync();
        auto_sync.emplace(config, noteapp::sync::note_executable_beside_self(),
                          [&](const noteapp::sync::SyncStatus& result, bool running) {
                              app.Post([&, result, running] {
                                  if (!auto_sync)
                                      return;
                                  sync_indicator.set_status(result, running);
                                  show_sync();
                                  if (sync_indicator.state() == tui::SyncIndicator::State::Failed)
                                      status.message = sync_indicator.error_text();
                              });
                          });
        auto_sync->start();
    };
    if (config.sync.enabled)
        start_auto_sync();

    // Önizleme satır sayısı modele bildirilir ki PgDn son satırı aşmasın.
    const auto sync_preview = [&] {
        state.set_preview_line_count(preview.get(*store, state.selected_row()).lines.size());
    };
    sync_preview();

    // Açık kaydet diyaloğu ve ona ait geçici dosya/içerik; diyalog açıkken tüm tuşlar ona gider.
    std::optional<tui::SaveDialogState> dialog;
    std::filesystem::path draft_file;
    std::string draft_content;
    // Ctrl-D onayı, F1 yardımı ve Ctrl-R'nin hedef notu (taşıma/silme sırasında seçim değişmesin diye
    // saklanır).
    std::optional<tui::ConfirmState> confirm;
    std::optional<tui::HelpState> help;
    std::optional<tui::SettingsState> settings;
    std::optional<core::NotePath> target;

    const auto view = ftxui::Renderer([&] {
        return tui::render(state, preview.get(*store, state.selected_row()), status, ftxui::Terminal::Size(),
                           dialog ? &*dialog : nullptr, confirm ? &*confirm : nullptr,
                           help ? &*help : nullptr, settings ? &*settings : nullptr);
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
        const auto file = config.notes_dir / row->path.value();
        const FileStamp before = stamp_of(file);
        const auto edited = edit_file(file);
        refresh_notes();
        if (stamp_of(file) != before)
            notify_changed();
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
        dialog.emplace(tui::SaveDialogInit{content,
                                           folder,
                                           store->folders(),
                                           config.types,
                                           detect::suggest(content, options),
                                           config.tuning.slug_max_length,
                                           std::chrono::system_clock::now(),
                                           /*move=*/false,
                                           {},
                                           {}});
    };

    // Ctrl-R: aynı diyalog taşıma kipinde; editör ve geçici dosya yok.
    const auto start_move = [&] {
        const tui::Row* row = state.selected_row();
        if (!row)
            return;
        target = row->path;
        dialog.emplace(
            tui::move_dialog_init(row->path, store->folders(), config.types, config.tuning.slug_max_length));
    };

    const auto start_delete = [&] {
        const tui::Row* row = state.selected_row();
        if (!row)
            return;
        target = row->path;
        confirm.emplace(core::tr("tui.confirm_delete", {{"path", row->path.string()}}));
    };

    // Silme onaylandı: seçim komşu satırda kalır (silinen satırın yerine gelen).
    const auto delete_target = [&] {
        const std::size_t index = state.selected();
        const auto removed = [&] {
            const std::scoped_lock lock(list_mutex);  // yeniden tarama iş parçacığı aynı store'u listeler
            return store->remove(*target);
        }();
        const std::string removed_path = target->string();
        confirm.reset();
        refresh_notes();
        state.select_index(index);
        if (removed) {
            status.message = core::tr("tui.deleted", {{"path", removed_path}});
            notify_changed();
        } else
            show_error(removed.error());
    };

    // Diyalog açıkken tuş işleme. Kayıt hatasında geçici dosya durur, diyalog açık kalır (ad/klasör
    // düzeltilsin).
    const auto handle_dialog_key = [&](const Key key, const std::string& text) {
        switch (dialog->handle(key, text)) {
        case tui::SaveDialogState::Outcome::Editing:
            break;
        case tui::SaveDialogState::Outcome::Discard: {
            if (!dialog->is_move()) {  // taşımada geçici dosya yok
                std::error_code ec;
                std::filesystem::remove(draft_file, ec);
            }
            dialog.reset();
            break;
        }
        case tui::SaveDialogState::Outcome::Submit: {
            if (dialog->is_move()) {
                const auto moved = [&] {
                    const std::scoped_lock lock(list_mutex);
                    return tui::move_note(*store, *target, dialog->request());
                }();
                if (!moved) {
                    dialog->set_error(core::format_error(moved.error()));  // diyalog açık kalır
                    break;
                }
                const core::NotePath from = *target;
                dialog.reset();
                if (*moved == from)
                    break;  // değişiklik yok
                refresh_notes();
                state.reveal(*moved);
                status.message = core::tr("tui.moved", {{"from", from.string()}, {"to", moved->string()}});
                notify_changed();
                break;
            }
            const auto saved = tui::save_new_note(*store, draft_file, dialog->request(), draft_content);
            if (!saved) {
                dialog->set_error(core::format_error(saved.error()) + "  " +
                                  core::tr("tui.temp_kept", {{"path", to_utf8(draft_file)}}));
                break;
            }
            dialog.reset();
            refresh_notes();
            state.reveal(*saved);
            status.message = core::tr("tui.saved", {{"path", saved->string()}});
            notify_changed();
            break;
        }
        }
    };

    // Kaydedilen ayarlar canlı uygulanır: dil anında, notes_dir değişince store yeniden kurulup liste ve
    // içerik indeksi sıfırlanır; diğer anahtarlar sonraki eylemlerde kullanılan `config`'e yansır. `tuning`
    // alanına dokunulmaz: yeniden tarama iş parçacığı onu okur ve ayar ekranından değişmez.
    const auto apply_settings = [&](const core::Config& next) {
        const bool dir_changed = next.notes_dir != config.notes_dir;
        const bool restart_sync = noteapp::tui::sync_restart_needed(config, next);
        config.language = next.language;
        config.notes_dir = next.notes_dir;
        config.editor = next.editor;
        config.default_type = next.default_type;
        config.default_folder = next.default_folder;
        config.types = next.types;
        config.sync = next.sync;
        core::init_i18n(config.language);
        // AutoSync süreleri ve depo yolu kurulurken okunur: ilgili ayar değiştiyse yeniden kurulur;
        // kapandıysa yıkılır. Yıkıcı stop(0) çağırıp süren birleştirmeyi öldürürdü: önce düzgün durdurulur.
        const auto stop_auto_sync = [&] {
            if (!auto_sync)
                return;
            auto_sync->stop(std::chrono::seconds(config.tuning.exit_timeout_seconds));
            auto_sync.reset();
        };
        if (!config.sync.enabled) {
            stop_auto_sync();
            sync_indicator.disable();
            show_sync();
        } else if (!auto_sync || restart_sync) {
            stop_auto_sync();
            start_auto_sync();
        }
        if (dir_changed) {
            {
                const std::scoped_lock lock(list_mutex);
                content_index.reset();  // ContentIndex eski store'a referans tutar
                store.emplace(config.notes_dir, config.tuning.slug_max_length);
            }
            refresh_notes();
        }
    };

    // Ctrl-C'yi de kendimiz işleriz: FTXUI'ye bırakılırsa döngüden sonra SIGINT'i yeniden yükseltir ve süreç
    // yıkıcılar (kod sayfası geri yüklemesi dahil) çalışmadan biter.
    const auto on_event = [&](const ftxui::Event& event) {
        std::string text;
        const auto key = to_key(event, text);
        if (!key)
            return false;
        if (help || confirm || dialog || settings) {
            // Çıkışta geçici dosya silinmez: yazılan metin state dizininde kalır.
            if (*key == Key::CtrlC)
                app.Exit();
            else if (help) {
                if (!help->handle(*key, text))
                    help.reset();
            } else if (settings) {
                const auto outcome = settings->handle(*key, text);
                switch (outcome) {
                case tui::SettingsState::Outcome::Editing:
                    break;
                case tui::SettingsState::Outcome::Cancel:
                    settings.reset();
                    break;
                case tui::SettingsState::Outcome::Submit:
                case tui::SettingsState::Outcome::Replace:  // origin değiştirme onaylandı
                    // Hata formda satır içi gösterilir; origin farklıysa form onay penceresini açar.
                    if (const auto saved =
                            settings->submit(config, outcome == tui::SettingsState::Outcome::Replace)) {
                        apply_settings(*saved);
                        settings.reset();
                        status.message = core::tr("tui.settings_saved");
                    }
                    break;
                }
            } else if (confirm) {
                switch (confirm->handle(*key, text)) {
                case tui::ConfirmState::Outcome::Pending:
                    break;
                case tui::ConfirmState::Outcome::Cancelled:
                    confirm.reset();
                    break;
                case tui::ConfirmState::Outcome::Confirmed:
                    delete_target();
                    break;
                }
            } else
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
        case tui::Intent::MoveSelected:
            start_move();
            break;
        case tui::Intent::DeleteSelected:
            start_delete();
            break;
        case tui::Intent::ShowHelp:
            help.emplace(tui::help_lines().size());
            break;
        case tui::Intent::ShowSettings:
            settings.emplace(config);
            break;
        case tui::Intent::SyncNow:
            if (auto_sync)
                auto_sync->request_now();
            else
                status.message = core::tr("tui.sync_disabled_hint");
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

    // Çıkış (Ctrl-C dahil, ikisi de app.Exit): bekleyen değişiklik için son bir senkron, en çok exit_timeout.
    // Terminal artık normal kipte; kullanıcı bekleyişin nedenini görsün.
    if (auto_sync) {
        std::cerr << core::tr("tui.sync_exiting") << std::endl;
        auto_sync->stop(std::chrono::seconds(config.tuning.exit_timeout_seconds));
    }
}
