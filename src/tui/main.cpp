#include "core/config.hpp"
#include "core/console.hpp"
#include "core/dll_search.hpp"
#include "core/editor.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/note_store.hpp"
#include "detect/detect.hpp"
#include "search/content_index.hpp"
#include "search/global_search.hpp"
#include "sync/auto_sync.hpp"
#include "sync/sync_service.hpp"
#include "tui/clipboard.hpp"
#include "tui/completion.hpp"
#include "tui/display_text.hpp"
#include "tui/editor_session.hpp"
#include "tui/editor_view.hpp"
#include "tui/list_actions.hpp"
#include "tui/modals.hpp"
#include "tui/mouse.hpp"
#include "tui/save_dialog.hpp"
#include "tui/settings.hpp"
#include "tui/spellcheck.hpp"
#include "tui/sync_indicator.hpp"
#include "tui/sync_status_worker.hpp"
#include "tui/termination.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ftxui/component/animation.hpp>
#include <ftxui/component/app.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/terminal.hpp>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <poll.h>
#include <unistd.h>
#endif

namespace {

using noteapp::tui::Key;

// Kayıtların durulmasını bekler: her tuşta değil, yazım durduktan sonra yeniden okunur.
constexpr auto kWordTableIdle = std::chrono::seconds(1);

// İçerik aramasında listelenecek en çok not; ekran bundan fazlasını zaten gösteremez.
constexpr std::size_t kContentResultLimit = 1000;

// Arka plan sonucundan sonra çizimi tetikler. Event::Custom kullanılmaz: FTXUI'de o `Special({0})`dır ve
// olaylar yalnız baytlarla karşılaştırıldığı için terminalin gönderdiği Ctrl+Space (NUL) ile aynıdır; her
// Post popup açardı.
const ftxui::Event& redraw_event() {
    static const ftxui::Event event = ftxui::Event::Special("\x1B[noteapp-redraw");
    return event;
}

// FTXUI olayını modelin tuşuna çevirir; tanınmayan olay (fare, boyut vb.) nullopt.
std::optional<Key> to_key(const ftxui::Event& event, std::string& text) {
    using ftxui::Event;
    if (event.is_character()) {
        text = event.character();
        return Key::Char;
    }
    // FTXUI bu xterm dizilerini adlandırmaz; ayrıştırıcı özel olay olarak iletir. Terminal göndermezse tuş
    // yoktur.
    static const Event shift_up = Event::Special("\x1B[1;2A");
    static const Event shift_down = Event::Special("\x1B[1;2B");
    static const Event shift_right = Event::Special("\x1B[1;2C");
    static const Event shift_left = Event::Special("\x1B[1;2D");
    static const Event shift_home = Event::Special("\x1B[1;2H");
    static const Event shift_end = Event::Special("\x1B[1;2F");
    static const Event ctrl_home = Event::Special("\x1B[1;5H");
    static const Event ctrl_end = Event::Special("\x1B[1;5F");
    // Alt+ok: xterm ailesi `ESC [1;3C`, eski dizi `ESC [3C`, macOS Terminal sözcük hareketi `ESC f`/`ESC b`.
    // rxvt'nin `ESC ESC [ C` dizisi tuş değildir: FTXUI 7.0.3 onu `ESC ESC` + `[C` diye böler (SPEC-tui
    // "Tuşlar").
    static const Event alt_right = Event::Special("\x1B[1;3C");
    static const Event alt_left = Event::Special("\x1B[1;3D");
    static const Event alt_right_short = Event::Special("\x1B[3C");
    static const Event alt_left_short = Event::Special("\x1B[3D");
    static const Event alt_right_word = Event::Special("\x1B"
                                                       "f");
    static const Event alt_left_word = Event::Special("\x1B"
                                                      "b");
    // Ctrl+Space NUL baytıdır; FTXUI C0 baytını özel olay olarak iletir (terminal_input_parser.cpp).
    static const Event ctrl_space = Event::Special(std::string_view("\0", 1));
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
        {&Event::CtrlQ, Key::CtrlQ},
        {&Event::F1, Key::F1},
        {&Event::F2, Key::F2},
        {&Event::F9, Key::F9},
        {&Event::CtrlU, Key::CtrlU},
        {&Event::CtrlC, Key::CtrlC},
        {&Event::Home, Key::Home},
        {&Event::End, Key::End},
        {&Event::Delete, Key::Delete},
        {&shift_up, Key::ShiftUp},
        {&shift_down, Key::ShiftDown},
        {&shift_right, Key::ShiftRight},
        {&shift_left, Key::ShiftLeft},
        {&shift_home, Key::ShiftHome},
        {&shift_end, Key::ShiftEnd},
        {&ctrl_home, Key::CtrlHome},
        {&ctrl_end, Key::CtrlEnd},
        {&Event::CtrlX, Key::CtrlX},
        {&Event::CtrlV, Key::CtrlV},
        {&Event::CtrlY, Key::CtrlY},
        {&Event::CtrlZ, Key::CtrlZ},
        {&Event::CtrlF, Key::CtrlF},
        {&Event::F3, Key::F3},
        {&ctrl_space, Key::CtrlSpace},
        {&alt_right, Key::AltRight},
        {&alt_left, Key::AltLeft},
        {&alt_right_short, Key::AltRight},
        {&alt_left_short, Key::AltLeft},
        {&alt_right_word, Key::AltRight},
        {&alt_left_word, Key::AltLeft},
    };
    for (const auto& [candidate, key] : table)
        if (event == *candidate)
            return key;
    return std::nullopt;
}

noteapp::tui::MouseInput to_mouse(const ftxui::Mouse& mouse) {
    using noteapp::tui::MouseButton;
    using noteapp::tui::MouseMotion;
    const auto button = [&] {
        switch (mouse.button) {
        case ftxui::Mouse::Left:
            return MouseButton::Left;
        case ftxui::Mouse::WheelUp:
            return MouseButton::WheelUp;
        case ftxui::Mouse::WheelDown:
            return MouseButton::WheelDown;
        default:
            return MouseButton::Other;
        }
    }();
    const auto motion = [&] {
        switch (mouse.motion) {
        case ftxui::Mouse::Pressed:
            return MouseMotion::Press;
        case ftxui::Mouse::Released:
            return MouseMotion::Release;
        default:
            return MouseMotion::Move;
        }
    }();
    return {mouse.x, mouse.y, button, motion};
}

// Yeni notun klasörü: seçili notun klasörü, yoksa default_folder (kaydet diyaloğuyla aynı kural).
std::string new_note_folder(const noteapp::tui::TuiState& state, const noteapp::core::Config& config) {
    const noteapp::tui::Row* row = state.selected_row();
    return noteapp::tui::initial_folder(row ? &row->path : nullptr, config.default_folder);
}

// Herhangi bir modal açık mı: açıkken tüm tuşlar ona gider.
template <typename... Modals> bool any_open(const Modals&... modals) {
    return (modals.has_value() || ...);
}

// Sinyal, döngünün kurulmadığı bir anda (harici editör, açılış) geldiyse döngüyü burada bitirir.
void exit_if_terminated(ftxui::App& app) {
    if (noteapp::tui::termination_signal() != 0)
        app.Exit();
}

// FTXUI Loop, her turda okuduğundan bağımsız 16 ms uyur ve tek okumada 128 bayt alır: tuş 16 ms'ye kadar
// bekler, biriken girdi 42 tuş/kare ile erir. Burada uyku girdide kesilir, kuyruk boşalana dek turlar uyumaz.
// Windows ve terminal olmayan stdin (FTXUI o zaman /dev/tty okur) eski yolda kalır.
void run_event_loop(ftxui::App& app, const ftxui::Component& component) {
#ifndef _WIN32
    if (isatty(STDIN_FILENO) == 1) {
        constexpr int kIdleWaitMs = 16;
        ftxui::Loop loop(&app, component);
        while (!loop.HasQuitted()) {
            loop.RunOnce();
            pollfd input{STDIN_FILENO, POLLIN, 0};
            const int ready = poll(&input, 1, kIdleWaitMs);
            // Kapanan terminalde poll hemen döner: işlemciyi yememek için eski tempoya düş.
            if (ready > 0 && (input.revents & POLLIN) == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(kIdleWaitMs));
        }
        return;
    }
#endif
    app.Loop(component);
}

void print_exit_note(const std::string& note) {
    if (!note.empty())
        std::cerr << note << std::endl;
}

}  // namespace

int main() {
    namespace core = noteapp::core;
    namespace detect = noteapp::detect;
    namespace tui = noteapp::tui;
    core::restrict_dll_search_to_system32();
    // FTXUI konsolu UTF-8'e çeker ama geri almaz; eski kod sayfasını biz saklayıp geri yükleriz.
    const core::ConsoleUtf8Scope utf8_console;
    // Dil ve log eşiği config'ten gelir; yükleme uyarıları log'a düşer ve açılışta alt çubukta gösterilir.
    const core::LoadedConfig loaded = core::load_config();
    // Ayarlar ekranı kaydedince canlı güncellenir (yalnız UI iş parçacığı yazar).
    core::Config config = loaded.config;
    core::init_i18n(config.language);
    core::init_log(config.tuning.log_max_bytes);
    for (const auto& warning : loaded.warnings)
        core::log_warn(core::format_error(warning));

    // notes_dir ayarı değişince yeniden kurulur (list_mutex altında).
    std::optional<core::NoteStore> store;
    store.emplace(core::NoteStore::for_config(config));
    // Yeniden tarama iş parçacığı listelerken UI iş parçacığı store'u yeniden kurmasın (notes_dir değişimi).
    std::mutex list_mutex;
    const auto list_notes = [&] {
        const std::scoped_lock lock(list_mutex);
        return store->list();
    };
    // Okunamayan çöp göstergeyi gizler; `T` hatayı söyler.
    const auto trash_count = [&]() -> std::size_t {
        const std::scoped_lock lock(list_mutex);
        const auto items = store->list_trash();
        return items ? items->size() : 0;
    };
    tui::TuiState state;
    tui::PreviewCache preview;
    // Yükleme uyarıları (ör. bozuk config.toml -> varsayılanlar) açılışta alt çubukta görünür.
    tui::StatusLine status{core::tr("tui.sync_off"), tui::config_warnings_message(loaded.warnings)};
    tui::SyncIndicator sync_indicator;
    auto listing = list_notes();
    state.set_notes(listing);
    state.set_trash_count(trash_count());
    // Geçiş bekleyen ya da açılamayan depoda liste boştur: kullanıcı bunu notların kaybı sanmasın.
    if (const auto issue = store->health())
        status.message = noteapp::tui::terminal_safe(core::format_error(*issue));

    // İçerik indeksi ilk içerik aramasında kurulur: ad modunda kalan kullanıcı tüm notları okumanın bedelini
    // ödemez. Yalnız UI iş parçacığından dokunulur (ContentIndex tek iş parçacıklıdır).
    std::optional<noteapp::search::ContentIndex> content_index;
    const auto ensure_content_index = [&]() -> noteapp::search::ContentIndex& {
        if (!content_index) {
            content_index.emplace(*store, config.tuning.max_note_bytes);
            content_index->refresh();
        }
        return *content_index;
    };
    state.set_content_searcher(
        [&](std::string_view query) { return ensure_content_index().search(query, kContentResultLimit); });
    state.set_global_searcher([&](std::string_view query, std::span<const core::NoteMeta> notes) {
        return noteapp::search::search_all(query, notes, ensure_content_index());
    });
    tui::load_list_prefs(state);

    auto app = ftxui::App::Fullscreen();
    // Durumlar işçide hesaplanır, tuş beklemez; sonuç UI iş parçacığına Post edilir. app'ten sonra
    // bildirildiği için ondan önce yıkılır.
    tui::SyncStatusWorker sync_status([&](tui::SyncStates states) {
        app.Post(
            [&state, states = std::move(states)]() mutable { state.set_sync_states(std::move(states)); });
        app.Post(redraw_event());
    });

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
                                  sync_status.request(config, state.metas());
                                  show_sync();
                                  if (sync_indicator.state() == tui::SyncIndicator::State::Failed)
                                      status.message = sync_indicator.error_text();
                              });
                              app.Post(redraw_event());  // kapanış görevi çizimi tetiklemez
                          });
        auto_sync->start();
    };
    if (config.sync.enabled)
        start_auto_sync();
    sync_status.request(config, state.metas());

    // Önizleme satır sayısı modele bildirilir ki PgDn son satırı aşmasın.
    const auto sync_preview = [&]() -> const tui::Preview& {
        const tui::Preview& shown = preview.throttled(state, *store, std::chrono::steady_clock::now());
        // Beklerken eski notun satır sayısı yeni notun kaydırmasını kırpmasın; animasyon tıkı son seçimi
        // okur.
        if (preview.deferred())
            ftxui::animation::RequestAnimationFrame();
        else
            state.set_preview_lines(&shown.lines, preview.generation());
        return shown;
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
    std::optional<tui::NewRowState> new_row;
    std::optional<tui::RenameState> rename;
    std::optional<core::NotePath> target;

    // Diğer notların sözcükleri: ilk editör açılışında, kayıttan bir süre sonra yeniden.
    tui::WordTableBuilder word_tables(kWordTableIdle);
    word_tables.set_store(*store);

    // Editör/kayıt sonrası liste, önizleme ve (kuruluysa) içerik indeksi güncellenir. `listing`'e dokunulmaz:
    // onu yalnız yeniden tarama iş parçacığı yazar; bir sonraki taramada yinelenen (zararsız) güncelleme
    // olur.
    const auto refresh_notes = [&] {
        preview.invalidate();
        word_tables.request();
        if (content_index)
            content_index->refresh();
        state.set_notes(list_notes());
        state.set_trash_count(trash_count());
        sync_status.request(config, state.metas());
    };

    const auto show_error = [&](const core::Error& error) { status.message = core::format_error(error); };

    // Sağ paneldeki gömülü editör. Pano ondan uzun yaşar: bir nottan kopyalanan diğerine yapıştırılır.
    std::string clipboard;
    tui::SystemClipboard system_clipboard;
    tui::ListActions list_actions(tui::ListHost{store, list_mutex, clipboard, system_clipboard, state,
                                                preview, status, refresh_notes, notify_changed});
    tui::EditorSession editor(tui::EditorHost{
        *store, list_mutex, clipboard, tui::terminal_cell_width, config.tuning.max_note_bytes,
        config.tuning.slug_max_length,
        [&](std::string_view content) {
            return detect::suggest(content, detect::Options{config.default_type, config.tuning.min_confidence,
                                                            config.tuning.max_scan_bytes})
                .type;
        },
        [&](const core::NotePath& path) {
            refresh_notes();
            state.select_note(path);
            notify_changed();
        },
        [&] {
            state.enter_editor();
            word_tables.enable();
        },
        [&](std::optional<std::size_t> line) { state.leave_editor(line); }, show_error,
        [&] { settings.emplace(config); },
        [&](std::string_view text) { return tui::copy_message(system_clipboard.copy(text), text.size()); },
        [&](const std::string& internal) { return tui::choose_paste(system_clipboard, internal); },
        [&] { return word_tables.latest(); }});
    const auto make_spell_checker = [&] {
        return std::make_unique<tui::SpellChecker>(tui::dictionary_for_language(config.language),
                                                   [&app] { app.Post(redraw_event()); });
    };
    // Yanıt UI iş parçacığına çizim isteğiyle gelir; dil değişince sözlük de değişir.
    auto spell = make_spell_checker();

    const auto view = ftxui::Renderer([&] {
        const ftxui::Dimensions window = ftxui::Terminal::Size();
        state.set_window(window.dimx, window.dimy);
        const tui::EditorViewport viewport = tui::editor_viewport(window, state.layout());
        editor.layout(viewport.columns, viewport.rows);  // boyut değişince de imleç görünür kalsın
        return tui::render(state, sync_preview(), status, window, dialog ? &*dialog : nullptr,
                           confirm ? &*confirm : nullptr, help ? &*help : nullptr,
                           settings ? &*settings : nullptr, new_row ? &*new_row : nullptr, &editor,
                           spell.get(), rename ? &*rename : nullptr);
    });

    // Terminali editöre bırakır; dönünce FTXUI ekranı geri yükler.
    const auto edit_file = [&](const std::filesystem::path& file) {
        std::expected<void, core::Error> result;
        app.WithRestoredIO([&] { result = core::run_editor(config, file); })();
        exit_if_terminated(app);
        return result;
    };

    const auto open_selected = [&] {
        const tui::Row* row = state.selected_row();
        if (!row)
            return;
        // Editör dosya ister: not özel geçici dosyaya çıkar, çakışma denetimiyle geri yazılır, dosya silinir.
        const core::NotePath path = row->path;
        const auto changed = [&] {
            return store->edit_in_temp_file(
                path, [&](const std::filesystem::path& file) -> std::expected<void, core::Error> {
                    const auto edited = edit_file(file);
                    if (!edited)
                        return std::unexpected(edited.error());
                    return {};
                });
        }();
        refresh_notes();
        if (changed && *changed)
            notify_changed();
        if (!changed)
            show_error(changed.error());
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

    // Taşıma diyaloğu açıkken tuş işleme; hata satır içi gösterilir, diyalog açık kalır.
    const auto handle_dialog_key = [&](const Key key, const std::string& text) {
        switch (dialog->handle(key, text)) {
        case tui::SaveDialogState::Outcome::Editing:
            break;
        case tui::SaveDialogState::Outcome::Discard:
            dialog.reset();
            break;
        case tui::SaveDialogState::Outcome::Submit: {
            const auto moved = [&] {
                const std::scoped_lock lock(list_mutex);
                return tui::move_note(*store, *target, dialog->request());
            }();
            if (!moved) {
                dialog->set_error(core::format_error(moved.error()));
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
        }
    };

    // `a`: Enter boş notu oluşturur ve EDIT'te açar (hatada satır açık kalır, metin kaybolmaz); Esc ve boş
    // girdi hiçbir şey yazmaz.
    const auto handle_new_row_key = [&](const Key key, const std::string& text) {
        switch (new_row->handle(key, text)) {
        case tui::NewRowState::Outcome::Editing:
            break;
        case tui::NewRowState::Outcome::Cancelled:
            new_row.reset();
            break;
        case tui::NewRowState::Outcome::Submit: {
            const auto created = [&] {
                const std::scoped_lock lock(list_mutex);
                return tui::create_named_note(*store, new_row->text(), new_note_folder(state, config),
                                              config.default_type, config.tuning.slug_max_length);
            }();
            if (!created) {
                new_row->set_error(core::format_error(created.error()));
                break;
            }
            new_row.reset();
            refresh_notes();
            state.select_note(created->path);  // süzgeç notu gizliyorsa temizlenir
            editor.open(created->path, 0);
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
        spell = make_spell_checker();
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
                store.emplace(core::NoteStore::for_config(config));
            }
            word_tables.set_store(*store);
            refresh_notes();
        }
    };

    // Ctrl-C'yi de kendimiz işleriz: FTXUI'ye bırakılırsa döngüden sonra SIGINT'i yeniden yükseltir ve süreç
    // yıkıcılar (kod sayfası geri yüklemesi dahil) çalışmadan biter.
    const auto on_event = [&](ftxui::Event event) {
        // Fare olayı FTXUI'ye bırakılmaz: kendi seçim boyamasını başlatırdı. Modal açıkken yok sayılır.
        if (event.is_mouse()) {
            if (!any_open(help, confirm, dialog, settings, new_row, rename))
                list_actions.on_mouse(to_mouse(event.mouse()), editor);
            sync_preview();
            return true;
        }
        std::string text;
        const auto key = to_key(event, text);
        if (!key || tui::passes_to_terminal(*key, state.focus()))
            return false;
        if (any_open(help, confirm, dialog, settings, new_row, rename)) {
            // Çıkışta geçici dosya silinmez: yazılan metin state dizininde kalır.
            if (tui::quits_from_modal(*key, state.focus()))
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
                    list_actions.finish_delete(confirm);
                    break;
                }
            } else if (new_row)
                handle_new_row_key(*key, text);
            else if (rename)
                list_actions.handle_rename_key(rename, *key, text);
            else
                handle_dialog_key(*key, text);
            sync_preview();
            return true;
        }
        status.message.clear();
        const tui::Intent intent = state.handle(*key, text);
        switch (intent) {
        case tui::Intent::None:
            break;
        case tui::Intent::Quit:
            app.Exit();
            break;
        case tui::Intent::OpenSelected:
            open_selected();
            break;
        case tui::Intent::NewRow:
            new_row.emplace();
            break;
        case tui::Intent::NewNote:
            state.open_new_note_view();
            editor.begin_new_note(new_note_folder(state, config));
            break;
        case tui::Intent::CreateFromQuery:
            // nvALT: sorgu yeni notun ilk satırı olur
            editor.begin_draft(state.query() + "\n", new_note_folder(state, config));
            break;
        case tui::Intent::MoveSelected:
            start_move();
            break;
        case tui::Intent::RenameSelected:
            list_actions.begin_rename(rename);
            break;
        case tui::Intent::ShowHelp:
            help.emplace(tui::help_lines().size());
            break;
        case tui::Intent::ShowSettings:
            settings.emplace(config);
            break;
        case tui::Intent::EnterEditor:
            editor.open(state.selected_row()->path, state.editor_start_line());
            break;
        case tui::Intent::EditorKey:
            editor.handle(*key, text);
            break;
        case tui::Intent::SyncNow:
            if (auto_sync)
                auto_sync->request_now();
            else
                status.message = core::tr("tui.sync_disabled_hint");
            break;
        default:
            list_actions.run(intent, confirm);
            break;
        }
        sync_preview();
        return true;
    };

    // Yeniden tarama arka planda yalnız stat yapar; değişiklik varsa model güncellemesi UI iş parçacığına
    // Post edilir; FTXUI kapanış görevinden sonra kareyi geçersiz saymaz, çizimi ardından gelen özel olay
    // tetikler (yoksa değişiklik ilk tuşa dek görünmezdi). Değişmediyse hiçbir şey gönderilmez. jthread,
    // state/preview'dan sonra bildirildiği için onlardan önce yıkılır (durdur + birleş).
    std::jthread rescan([&](const std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any wake;
        const auto period = std::chrono::seconds(config.tuning.rescan_seconds);
        std::size_t seen_trash = trash_count();
        while (true) {
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, period, [] { return false; });  // durdurma isteğinde erken döner
            if (stop.stop_requested())
                return;
            auto latest = list_notes();
            const std::size_t latest_trash = trash_count();  // senkron yalnız çöpü de değiştirebilir
            if (tui::same_listing(listing, latest) && latest_trash == seen_trash)
                continue;
            listing = latest;  // yalnız bu iş parçacığı okur/yazar
            seen_trash = latest_trash;
            app.Post([&state, &preview, &content_index, &sync_preview, &editor, &word_tables, &sync_status,
                      &config, &list_actions, latest_trash, latest = std::move(latest)]() mutable {
                preview.invalidate();
                word_tables.request();
                if (content_index)
                    content_index->refresh();  // yalnız mtime/boyutu değişenler yeniden okunur
                editor.on_listing(latest);     // senkron çekmesi de buradan geçer
                state.set_notes(std::move(latest));
                state.set_trash_count(latest_trash);
                sync_status.request(config, state.metas());
                list_actions.refresh_trash_view();
                sync_preview();
            });
            app.Post(redraw_event());
        }
    });

    // Ctrl-C ve Ctrl-Z'yi biz işleriz: FTXUI varsayılanı işlensin işlenmesin sinyal yükseltir, editörde
    // kopyala ve geri al süreci öldürür ya da askıya alırdı.
    app.ForceHandleCtrlC(false);
    app.ForceHandleCtrlZ(false);
    // FTXUI eski işleyiciyi saklar, döngüden sonra sinyali ona yeniden yükseltir: işleyicimiz süreci
    // öldürmeden döngüden çıkışa ve aşağıdaki kayda izin verir.
    tui::install_termination_handlers();
    app.Post([&app] { exit_if_terminated(app); });
    run_event_loop(app, ftxui::CatchEvent(view, on_event));

    // Dış sonlandırma dahil her çıkış: kirli metin sessizce düşmez. Senkrondan önce: kayıt da gönderilsin.
    print_exit_note(editor.flush_on_exit());

    // Çıkış (Ctrl-C dahil, ikisi de app.Exit): bekleyen değişiklik için son bir senkron, en çok exit_timeout.
    // Terminal artık normal kipte; kullanıcı bekleyişin nedenini görsün.
    if (auto_sync) {
        std::cerr << core::tr("tui.sync_exiting") << std::endl;
        auto_sync->stop(std::chrono::seconds(config.tuning.exit_timeout_seconds));
    }
    return tui::termination_exit_code();
}
