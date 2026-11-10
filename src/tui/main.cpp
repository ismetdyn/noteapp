#include "app/document.hpp"
#include "app/import_files.hpp"
#include "app/sync_status_worker.hpp"
#include "app/ui_args.hpp"
#include "app/update_controller.hpp"
#include "app/update_worker.hpp"
#include "core/config.hpp"
#include "core/console.hpp"
#include "core/dll_search.hpp"
#include "core/editor.hpp"
#include "core/file_icons.hpp"
#include "core/graph_limits.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/note_store.hpp"
#include "core/note_views.hpp"
#include "core/paths.hpp"
#include "core/process.hpp"
#include "core/text.hpp"
#include "core/version.hpp"
#include "detect/detect.hpp"
#include "edit/clipboard.hpp"
#include "edit/completion.hpp"
#include "edit/display_text.hpp"
#include "edit/editor_session.hpp"
#include "edit/language_servers.hpp"
#include "edit/spellcheck.hpp"
#include "search/content_index.hpp"
#include "search/global_search.hpp"
#include "sync/auto_sync.hpp"
#include "sync/sync_service.hpp"
#include "tui/editor_tabs.hpp"
#include "tui/editor_view.hpp"
#include "tui/frame_diff.hpp"
#include "tui/list_actions.hpp"
#include "tui/modals.hpp"
#include "tui/mouse.hpp"
#include "tui/save_dialog.hpp"
#include "tui/settings.hpp"
#include "tui/sixel.hpp"
#include "tui/sync_indicator.hpp"
#include "tui/termination.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
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
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
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
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

using noteapp::tui::Key;

// Kayıtların durulmasını bekler: her tuşta değil, yazım durduktan sonra yeniden okunur.
constexpr auto kWordTableIdle = std::chrono::seconds(1);

// Arka plan sonucundan sonra çizimi tetikler. Event::Custom kullanılmaz: FTXUI'de o `Special({0})`dır ve
// olaylar yalnız baytlarla karşılaştırıldığı için terminalin gönderdiği Ctrl+Space (NUL) ile aynıdır; her
// Post popup açardı.
const ftxui::Event& redraw_event() {
    static const ftxui::Event event = ftxui::Event::Special("\x1B[noteapp-redraw");
    return event;
}

// Terminal hücresinin piksel boyu (sixel PDF sayfası): TIOCGWINSZ'nin ws_xpixel/ws_ypixel'i, terminal sıfır
// bildiriyorsa nullopt. `CSI 16 t` sorulmaz: FTXUI ayrıştırıcısı yanıtı tuş olayı yapar. Windows konsolu piksel boyu
// vermez: nullopt, sayfa `▀` blokla çizilir (SPEC-tui "PDF sayfa görüntüsü").
std::optional<noteapp::tui::CellPixels> terminal_cell_pixels() {
#ifndef _WIN32
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 && size.ws_row > 0) {
        const noteapp::tui::CellPixels cell{.width = size.ws_xpixel / size.ws_col,
                                            .height = size.ws_ypixel / size.ws_row};
        if (cell.width > 0 && cell.height > 0)
            return cell;
    }
#endif
    return std::nullopt;
}

// Yamalı FTXUI'nin, limits.tui.paste_bytes'ı aşan yapıştırma yerine gönderdiği olay (baytlar tutulmadı).
constexpr std::string_view kPasteTooLarge = "\x1B[noteapp-paste-too-large";

// Yamalı FTXUI bracketed paste'i tek özel olay yapar: girdisi bu önek ve ham metindir. Sınırı aşan
// yapıştırmada kPasteTooLarge. Değilse nullopt.
std::optional<std::string_view> pasted_text(const ftxui::Event& event) {
    constexpr std::string_view kPasteStart = "\x1B[200~";
    const std::string_view input = event.input();
    if (input == kPasteTooLarge)
        return input;
    if (!input.starts_with(kPasteStart))
        return std::nullopt;
    return input.substr(kPasteStart.size());
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
    // Sekmeler: Alt+N / Alt+P `ESC n` / `ESC p`; Ctrl+PgDn / Ctrl+PgUp xterm dizisi (çoğu terminal kendi
    // sekmesi için yutar, Alt+N/P her yerde gelir).
    static const Event alt_n = Event::Special("\x1B"
                                              "n");
    static const Event alt_p = Event::Special("\x1B"
                                              "p");
    // Alt+Z `ESC z`: FTXUI tek özel olay olarak iletir (ESC'den sonraki tek bayt), karakter değildir.
    static const Event alt_z = Event::Special("\x1B"
                                              "z");
    static const Event ctrl_page_down = Event::Special("\x1B[6;5~");
    static const Event ctrl_page_up = Event::Special("\x1B[5;5~");
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
        {&Event::F11, Key::F11},
        {&Event::CtrlU, Key::CtrlU},
        {&Event::CtrlT, Key::CtrlT},
        {&Event::CtrlA, Key::CtrlA},
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
        {&Event::CtrlW, Key::CtrlW},
        {&Event::CtrlK, Key::CtrlK},
        {&alt_n, Key::AltN},
        {&alt_p, Key::AltP},
        {&alt_z, Key::AltZ},
        {&ctrl_page_down, Key::CtrlPageDown},
        {&ctrl_page_up, Key::CtrlPageUp},
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
        case ftxui::Mouse::Right:
            return MouseButton::Right;
        case ftxui::Mouse::WheelUp:
            return MouseButton::WheelUp;
        case ftxui::Mouse::WheelDown:
            return MouseButton::WheelDown;
        case ftxui::Mouse::WheelLeft:
            return MouseButton::WheelLeft;
        case ftxui::Mouse::WheelRight:
            return MouseButton::WheelRight;
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
    return {mouse.x, mouse.y, button, motion, mouse.shift};
}

// Yeni notun klasörü: GUI ile ortak kural (app::new_note_folder).
std::string new_note_folder(const noteapp::tui::TuiState& state, const noteapp::core::Config& config) {
    const noteapp::tui::Row* folder_row = state.selected_folder_row();
    const std::string folder = folder_row ? folder_row->path.string() : std::string{};
    const noteapp::tui::Row* row = state.selected_row();
    const std::string scope = state.name_base_folder();
    return noteapp::app::new_note_folder({.selected_folder = folder_row ? &folder : nullptr,
                                          .scope_folder = scope,
                                          .note = row ? &row->path : nullptr,
                                          .default_folder = config.default_folder});
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
// Windows ve terminal olmayan stdin (FTXUI o zaman /dev/tty okur) FTXUI'nin temposunda düz uyur.
// Zamanlayıcılar (autosave, dil sunucusu, önizleme ertelemesi) kare istemez: `wake_due` süre dolunca true
// döner ve tek yeniden çizim gönderilir. RequestAnimationFrame beklerken her 15 ms'de tam kare kurardı.
void run_event_loop(ftxui::App& app, const ftxui::Component& component,
                    const std::function<bool()>& wake_due) {
    constexpr int kIdleWaitMs = 16;
    ftxui::Loop loop(&app, component);
#ifndef _WIN32
    const bool poll_stdin = isatty(STDIN_FILENO) == 1;
#endif
    while (!loop.HasQuitted()) {
        if (wake_due())
            app.Post(redraw_event());
        loop.RunOnce();
#ifndef _WIN32
        if (poll_stdin) {
            pollfd input{STDIN_FILENO, POLLIN, 0};
            const int ready = poll(&input, 1, kIdleWaitMs);
            // Kapanan terminalde poll hemen döner: işlemciyi yememek için eski tempoya düş.
            if (ready > 0 && (input.revents & POLLIN) == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(kIdleWaitMs));
            continue;
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(kIdleWaitMs));
    }
}

// FTXUI terminali bıraktıktan sonra yazılır: not yolu (senkrondan gelebilir) kaçış dizisi ya da bidi taşır.
void print_exit_note(const std::string& note) {
    if (!note.empty())
        std::cerr << noteapp::tui::terminal_safe(note) << '\n';
}

namespace app = noteapp::app;
namespace core = noteapp::core;
namespace detect = noteapp::detect;
namespace tui = noteapp::tui;

// Güncelleme işçisinin ayarları (config.limits.update); sürüm derlenen sürümdür.
app::UpdateSettings update_settings(const core::Config& config) {
    app::UpdateSettings settings;
    settings.limits = config.limits.update;
    return settings;
}

// `notes import`'un açılışa bıraktığı: REVIEW'da gösterilecek ilk not ve durum satırı.
struct Opening {
    core::NotePath note;
    std::string status;
};

// TUI oturumu: üyeler bildirim sırasıyla kurulur, tersiyle yıkılır ve bu sıra yüklüdür. İş parçacıkları
// (sync_status_, auto_sync_, word_tables_, spell_, language_servers_, rescan_) dokundukları
// app_, status_, state_ ve store_'dan sonra bildirilir: onlardan önce durdurulup katılır. Üye eklerken sırayı
// koru.
class TuiSession {
public:
    TuiSession(const core::LoadedConfig& loaded, const std::optional<Opening>& opening);
    TuiSession(const TuiSession&) = delete;
    TuiSession& operator=(const TuiSession&) = delete;
    TuiSession(TuiSession&&) = delete;
    TuiSession& operator=(TuiSession&&) = delete;
    // PDF önizleme işçisi app_'e Post eder; preview_ app_'ten önce bildirildiği için ondan sonra yıkılırdı.
    ~TuiSession() { preview_.close_pdf(); }

    int run();

private:
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- kurucu doldurur, yalnız emplace yeniler
    core::NoteStore& store() { return *store_; }
    std::vector<core::NoteInfo> list_notes();
    std::vector<std::string> list_folders();
    std::size_t trash_count();
    tui::StoreScan scan();
    noteapp::search::ContentIndex& ensure_content_index();
    // İçerik, genel ve ilişki aramasını depoya bağlar (depo erişimi liste kilidiyle).
    void connect_search();
    void load_prefs();
    void show_sync();
    void notify_changed();
    void start_auto_sync();
    const tui::Preview& sync_preview();
    void refresh_notes();
    void show_error(const core::Error& error);
    std::unique_ptr<tui::SpellChecker> make_spell_checker();
    ftxui::Element render();
    std::expected<void, core::Error> edit_file(const std::filesystem::path& file);
    void open_selected();
    void start_move();
    void handle_dialog_key(tui::SaveDialogState& dialog, Key key, const std::string& text);
    void handle_new_row_key(tui::NewRowState& row, Key key, const std::string& text);
    void apply_settings(const core::Config& next);
    void handle_settings_key(tui::SettingsState& settings, Key key, const std::string& text);
    void start_connection_test(core::Config candidate);
    void handle_modal_key(Key key, const std::string& text);
    void request_sync_now();
    void handle_update_key(Key key);
    void run_intent(tui::Intent intent, Key key, const std::string& text);
    std::unique_ptr<tui::EditorSession> make_editor(std::uint64_t id);
    void enter_editor();
    void open_tab(const core::NotePath& path, std::size_t start_line, bool new_tab);
    bool start_draft(const std::function<void(tui::EditorSession&)>& start, bool new_tab);
    void show_tab_result(tui::EditorTabs::Result result);
    void show_active_tab();
    void report_closed_tabs(const std::vector<tui::EditorTabs::Closed>& closed, std::uint64_t focused);
    void settle_tabs();
    bool on_event(ftxui::Event event);
    // En yakın zamanlayıcının (autosave, dil sunucusu gönderimi, önizleme ertelemesi) süresi dolduysa true;
    // anı son çizim hesaplar (render), çağrı onu tüketir: bir süre bir yeniden çizim.
    bool wake_due(std::chrono::steady_clock::time_point now);
    void rescan_loop(const std::stop_token& stop, std::vector<core::NoteInfo> listing,
                     std::uint64_t listing_version);

    // Ayarlar ekranı kaydedince canlı güncellenir (yalnız UI iş parçacığı yazar).
    core::Config config_;
    // notes_dir ayarı değişince yeniden kurulur (list_mutex_ altında).
    std::optional<core::NoteStore> store_;
    // Yeniden tarama iş parçacığı listelerken UI iş parçacığı store'u yeniden kurmasın (notes_dir değişimi).
    std::mutex list_mutex_;
    tui::TuiState state_;
    tui::PreviewCache preview_;
    tui::StatusLine status_;
    tui::SyncIndicator sync_indicator_;
    // İçerik indeksi ilk içerik aramasında kurulur: ad modunda kalan kullanıcı tüm notları okumanın bedelini
    // ödemez. Yalnız UI iş parçacığından dokunulur (ContentIndex tek iş parçacıklıdır).
    std::optional<noteapp::search::ContentIndex> content_index_;
    // `view:` görünümü depoda çalıştırır; sonucu deponun görüntüsü değişene dek saklanır (core::view_notes).
    ftxui::App app_ = ftxui::App::Fullscreen();
    tui::FrameDiff frame_diff_;
    // Hücre piksel boyu bu pencere boyutunda soruldu (terminal_cell_pixels); boyut değişince yeniden sorulur.
    ftxui::Dimensions cell_window_{.dimx = -1, .dimy = -1};
    std::optional<tui::CellPixels> cell_pixels_;
    std::optional<std::chrono::steady_clock::time_point> wake_at_;  // render yazar, wake_due tüketir
    // Tanı: NOTEAPP_TUI_DRAW_LOG bir dosya yoluysa her çizim bir satır (terminale yazılan bayt) ekler. Boşta
    // çizim olmadığını tui_pty testleri bununla sayar; değişken yoksa dosya açılmaz.
    std::ofstream draw_log_;
    // Durumlar işçide hesaplanır, tuş beklemez; sonuç UI iş parçacığına Post edilir. app_'ten sonra
    // bildirildiği için ondan önce yıkılır.
    app::SyncStatusWorker sync_status_;
    // Güncelleme denetimi ve kurulumu işçide (app::UpdateController, GUI ile ortak akış); durum değişikliği
    // UI iş parçacığına Post edilir. app_'ten sonra bildirildiği için ondan önce yıkılır (iptal + bekleme;
    // kurulum değiştirme evresindeyse biter, run() bunu söyler). Pencere açıkken tüm tuşlar ona gider.
    app::UpdateController update_;
    std::optional<tui::UpdateDialogState> update_dialog_;
    // Ayarlardaki bağlantı testi: tek senkron işçide; sonuç UI iş parçacığına Post edilir. app_'ten sonra
    // bildirildiği için ondan önce katılır (senkron kendi zaman aşımıyla biter). Yalnız UI iş parçacığı
    // `connection_testing_`e dokunur.
    std::jthread connection_test_;
    bool connection_testing_ = false;
    // Arka plan senkronu yalnız sync.enabled iken vardır (kapatınca yıkılır; depo olduğu gibi kalır).
    // app_'ten sonra bildirildiği için ondan önce yıkılır. Yalnız UI iş parçacığı oluşturur/yıkar.
    std::optional<noteapp::sync::AutoSync> auto_sync_;
    // Açık kaydet diyaloğu; diyalog açıkken tüm tuşlar ona gider.
    std::optional<tui::SaveDialogState> dialog_;
    // Ctrl-D onayı, F1 yardımı ve Ctrl-R'nin hedef notu (taşıma/silme sırasında seçim değişmesin diye
    // saklanır).
    std::optional<tui::ConfirmState> confirm_;
    std::optional<tui::HelpState> help_;
    std::optional<tui::SettingsState> settings_;
    std::optional<tui::NewRowState> new_row_;
    std::optional<tui::RenameState> rename_;
    std::optional<core::NotePath> target_;
    // Diğer notların sözcükleri: ilk editör açılışında, kayıttan bir süre sonra yeniden.
    tui::WordTableBuilder word_tables_;
    // Sağ paneldeki gömülü editör. Pano ondan uzun yaşar: bir nottan kopyalanan diğerine yapıştırılır.
    std::string clipboard_;
    tui::SystemClipboard system_clipboard_;
    tui::ListActions list_actions_;
    // Editör sekmeleri: her sekme kendi EditorSession'ı (SPEC-tui "Sekmeler"). Oturumların geri çağrıları
    // sekme kimliğiyle gelir; arka plandaki sekme odağa dokunmaz.
    tui::EditorTabs tabs_;
    bool notes_refreshed_ = false;  // refresh_notes sonrası sekmeler silinen notları denetlesin
    // Yanıt UI iş parçacığına çizim isteğiyle gelir; dil değişince sözlük de değişir.
    std::unique_ptr<tui::SpellChecker> spell_;
    ftxui::Component view_;
    // Dil sunucuları ve çalışma alanı yansıtması; ayar kapalıyken boştur (süreç ve dizin yok). İstemci
    // işçileri ve yansıtma iş parçacığı app_, store_ ve tabs_'tan sonra bildirildiği için onlardan önce
    // durur; bildirimleri kapanışta da app_'e çizim isteği gönderebilir.
    tui::LanguageServers language_servers_;
    std::string lsp_status_;  // gösterilmeyi bekleyen dil sunucusu iletisi (yalnız UI iş parçacığı)
    // Yeniden tarama arka planda yalnız stat yapar; değişiklik varsa model güncellemesi UI iş parçacığına
    // Post edilir; FTXUI kapanış görevinden sonra kareyi geçersiz saymaz, çizimi ardından gelen özel olay
    // tetikler (yoksa değişiklik ilk tuşa dek görünmezdi). Değişmediyse hiçbir şey gönderilmez. jthread,
    // state_/preview_'dan sonra bildirildiği için onlardan önce yıkılır (durdur + birleş).
    std::jthread rescan_;
};

TuiSession::TuiSession(const core::LoadedConfig& loaded, const std::optional<Opening>& opening)
    : config_(loaded.config), store_(core::NoteStore::for_config(config_)),
      preview_(tui::view_limits_of(config_.limits.tui), tui::syntax_limits_of(config_.limits.tui)),
      // Yükleme uyarıları (ör. bozuk config.toml -> varsayılanlar) açılışta alt çubukta görünür.
      status_{.sync = core::tr("app.sync_off"),
              .message = tui::config_warnings_message(loaded.warnings),
              .notices = app::NoticeQueue(app::NoticeQueue::lifetime_of(loaded.config.tuning.notice_ms.value))},
      sync_status_([this](app::SyncStates states, std::uint64_t generation) {
          // Aynı durumlar (her kayıttan sonraki istek, senkron kapalıyken hep) çizim istemez.
          app_.Post([this, states = std::move(states), generation]() mutable {
              if (sync_status_.current(generation) && state_.set_sync_states(std::move(states)))
                  app_.Post(redraw_event());
          });
      }),
      update_(update_settings(config_),
              [this] {
                  app::UpdateWorkerDeps deps = app::default_update_deps();
                  // İşçi iş parçacığından: yalnız Post (check/install çağrılmaz; UpdateWorkerDeps).
                  deps.on_change = [this] {
                      // Kapanış görevi çizimi tetiklemez: görünür değişiklik varsa özel olay çizer.
                      app_.Post([this] {
                          if (update_.changed(status_.notices, std::chrono::steady_clock::now()) ||
                              update_dialog_)
                              app_.Post(redraw_event());
                      });
                  };
                  return deps;
              }()),
      word_tables_(kWordTableIdle,
                   tui::completion_limits_of(config_.limits.tui, config_.tuning.completion_min_prefix_bytes)),
      system_clipboard_(tui::SystemClipboard::write_to_terminal, tui::SystemClipboard::kToolTimeout,
                        config_.limits.tui.paste_bytes.cap()),
      list_actions_(tui::ListHost{
          store_, list_mutex_, clipboard_, system_clipboard_, state_, preview_, status_,
          [this] { refresh_notes(); }, [this] { notify_changed(); }, &config_,
          [this](const std::string& from, const std::string& to) { tabs_.follow_moved(from, to); },
          // Kapanış görevi kareyi geçersiz saymaz: çizimi ardından gelen özel olay tetikler.
          [this](std::function<void()> work) {
              app_.Post([this, work = std::move(work)] {
                  work();
                  app_.Post(redraw_event());
              });
          },
          // Yalnız etkin sekme kirli kalabilir: ayrılan sekme kendi notuna yazılır (leave_active).
          [this](const core::NotePath& note) { return tabs_.active().path() != note || tabs_.active().park(); }}),
      tabs_([this](std::uint64_t id) { return make_editor(id); }, config_.limits.app.tabs),
      view_(ftxui::Renderer([this] { return render(); })),
      language_servers_(
          config_.lsp, tui::lsp_workspace_dir(), store(), [this] { app_.Post(redraw_event()); },
          tui::LanguageServers::kChangeIdle, tui::LanguageServers::kMirrorIdle, tui::kBlockNetworkPaths,
          tui::lsp_limits_of(config_.limits.lsp)) {
    state_.set_icon_style(core::terminal_icon_style(config_.icons));
    // PDF notunun metni arka planda okunur; her sayfa çizimi tetikler (kapanış görevi tetiklemez: özel olay).
    preview_.set_pdf(config_.limits.pdf, core::note_executable_beside_self(), [this] { app_.Post(redraw_event()); });
    state_.layout().set_share(config_.tuning.tui_list_share);
    state_.set_recent_commands_limit(config_.limits.app.recent_commands);  // load_list_prefs'ten önce
    state_.set_map_node_limit(config_.limits.app.map_nodes);
    state_.set_map_depth_limit(config_.limits.app.map_depth);
    state_.set_query_limits(core::query_limits_of(config_.limits.query));  // F3 sorgu: kullanıcının profili
    state_.set_query_cell_bytes(config_.limits.app.query_cell_bytes.cap());
    state_.set_inspector_cell_bytes(config_.limits.app.query_cell_bytes.cap());
    state_.set_search_history_limits({.entries = config_.limits.tui.search_history.cap(),
                                      .entry_bytes = config_.limits.tui.search_entry_bytes.cap()});
    auto listing = list_notes();
    const std::uint64_t listing_version = store().data_version();
    state_.set_notes(listing);
    state_.set_folders(list_folders());
    state_.set_views(tui::view_names(store()));  // sonrakileri yeniden tarama getirir (graph_version)
    state_.set_trash_count(trash_count());
    // Açılamayan depoda liste boştur: kullanıcı bunu notların kaybı sanmasın.
    if (const auto issue = store().health())
        status_.message = noteapp::tui::terminal_safe(core::format_error(*issue));

    connect_search();
    load_prefs();
    // Geri gelen sekmelerin içeriği ilk etkinleşmede okunur; açılış Listede kalır.
    tabs_.restore(state_.list_prefs().tabs,
                  [this](const core::NotePath& path) { return state_.meta_of(path) != nullptr; });
    if (opening) {
        state_.open_note_in_review(opening->note);
        status_.message = opening->status;
    }

    if (const auto draw_log = core::get_env_utf8("NOTEAPP_TUI_DRAW_LOG"))
        draw_log_.open(std::filesystem::path(std::u8string(draw_log->begin(), draw_log->end())),
                       std::ios::app);
    app_.SetPasteTimeout(std::chrono::milliseconds(  // 0 = hiç bitmez
        static_cast<std::int64_t>(std::min<std::uint64_t>(config_.limits.tui.paste_timeout.value, INT64_MAX))));
    // Sınırı aşan baytlar ayrıştırıcıda da tutulmaz: dev yapıştırma belleği doldurmaz (0 = sınırsız).
    app_.SetPasteLimit(static_cast<std::size_t>(
        std::min<std::uint64_t>(config_.limits.tui.paste_bytes.value, std::numeric_limits<std::size_t>::max())));
    app_.SetFrameWriter([this](const ftxui::Screen& screen, bool repaint, std::string& output) {
        const std::size_t before = output.size();
        frame_diff_.write(screen, repaint, output, preview_.shown().sixels);
        // flush: test sayarken satır yarım kalmasın.
        if (draw_log_.is_open())
            draw_log_ << (output.size() - before) << '\n' << std::flush;
    });

    if (config_.sync.enabled)
        start_auto_sync();
    if (opening)
        notify_changed();  // içe alınanlar senkrona gitsin
    sync_status_.request(config_, state_.metas());

    sync_preview();

    word_tables_.set_store(store());
    spell_ = make_spell_checker();

    rescan_ = std::jthread(
        [this, listing = std::move(listing), listing_version](const std::stop_token& stop) mutable {
            rescan_loop(stop, std::move(listing), listing_version);
        });
}

// Sekmenin oturumu. Geri çağrılar yalnız etkin sekmeden odağı ve iletiyi değiştirir: arka plandaki sekmenin
// kapanışını ileti report_closed_tabs'ta söyler.
std::unique_ptr<tui::EditorSession> TuiSession::make_editor(std::uint64_t id) {
    return std::make_unique<tui::EditorSession>(tui::EditorHost{
        store(), list_mutex_, clipboard_, tui::terminal_cell_width, config_.tuning.max_note_bytes.cap(),
        config_.tuning.slug_max_length,
        [this](std::string_view content) {
            return detect::suggest(content,
                                   detect::Options{config_.default_type, config_.tuning.min_confidence,
                                                   config_.tuning.max_scan_bytes})
                .type;
        },
        [this](const core::NotePath& path) {
            refresh_notes();
            state_.select_note(path);
            notify_changed();
        },
        [this, id] {
            if (id == tabs_.active_id())
                enter_editor();
        },
        [this, id](std::optional<std::size_t> line) {
            if (id == tabs_.active_id() && state_.focus() == tui::Focus::Editor)
                state_.leave_editor(line);
        },
        [this, id](const core::Error& error) {
            if (id == tabs_.active_id())
                show_error(error);
        },
        [this, id] {
            if (id == tabs_.active_id())
                settings_.emplace(config_);
        },
        [this](std::string_view text) {
            return tui::copy_message(system_clipboard_.copy(text), text.size());
        },
        [this](const std::string& internal) { return tui::choose_paste(system_clipboard_, internal); },
        [this] { return word_tables_.latest(); }, std::chrono::milliseconds(config_.tuning.autosave_ms),
        tui::syntax_limits_of(config_.limits.tui), config_.limits.tui.undo_bytes.cap(),
        tui::completion_limits_of(config_.limits.tui, config_.tuning.completion_min_prefix_bytes)});
}

void TuiSession::enter_editor() {
    state_.enter_editor();
    word_tables_.enable();
}

void TuiSession::show_tab_result(tui::EditorTabs::Result result) {
    if (result == tui::EditorTabs::Result::Limit)
        status_.message = core::tr("app.tab_limit", {{"max", tabs_.limit().text}});
}

// Not etkin sekmede ya da kurala göre yeni sekmede açılır; zaten açık sekme öne gelir.
void TuiSession::open_tab(const core::NotePath& path, std::size_t start_line, bool new_tab) {
    const tui::EditorTabs::Result result = tabs_.open_note(path, start_line, new_tab);
    show_tab_result(result);
    if (result == tui::EditorTabs::Result::Done && tabs_.active().is_open())
        enter_editor();
}

bool TuiSession::start_draft(const std::function<void(tui::EditorSession&)>& start, bool new_tab) {
    const tui::EditorTabs::Result result = tabs_.begin_note(start, new_tab);
    show_tab_result(result);
    return result == tui::EditorTabs::Result::Done;
}

// Etkin sekme editörde açılır. Notu artık yoksa (taramalar arasında gitti) sekme iletiyle kapanır, sıradaki
// denenir; hiçbiri açılamazsa odak Listeye döner.
void TuiSession::show_active_tab() {
    while (!tabs_.empty()) {
        tabs_.load_active();
        if (tabs_.active().is_open()) {
            enter_editor();
            return;
        }
        tabs_.sweep();
    }
    if (state_.focus() == tui::Focus::Editor)
        state_.leave_editor(std::nullopt);
}

// Odaktaki sekmenin kapanışını oturum kendisi söyler (on_error, mevcut akış); diğerleri GUI iletisiyle.
void TuiSession::report_closed_tabs(const std::vector<tui::EditorTabs::Closed>& closed,
                                    std::uint64_t focused) {
    for (const tui::EditorTabs::Closed& tab : closed)
        if (tab.id != focused)
            status_.message =
                core::tr("app.tab_closed_missing", {{"path", tui::terminal_safe(tab.path.string())}});
}

// Her olaydan sonra: kapanan oturumların sekmeleri toplanır (Ctrl+W ise komşu öne gelir), liste eylemi notu
// silmişse arka plandaki sekme kapanır, açık sekmeler durum dosyasına yazılacak tercihlere girer.
void TuiSession::settle_tabs() {
    if (std::exchange(notes_refreshed_, false)) {
        const std::uint64_t focused = state_.focus() == tui::Focus::Editor ? tabs_.active_id() : 0;
        report_closed_tabs(tabs_.on_listing(list_notes()), focused);
    }
    if (tabs_.sweep().reopen)
        show_active_tab();
    state_.set_saved_tabs(tabs_.saved());
}

std::vector<core::NoteInfo> TuiSession::list_notes() {
    const std::scoped_lock lock(list_mutex_);
    return store().list();
}

std::vector<std::string> TuiSession::list_folders() {
    const std::scoped_lock lock(list_mutex_);
    return store().folders();
}

// Okunamayan çöp göstergeyi gizler; `T` hatayı söyler.
std::size_t TuiSession::trash_count() {
    const std::scoped_lock lock(list_mutex_);
    const auto items = store().list_trash();
    return items ? items->size() : 0;
}

tui::StoreScan TuiSession::scan() {
    const std::scoped_lock lock(list_mutex_);
    return tui::scan_store(store());
}

// tui-state.toml: liste tercihleri, sonra haritanın görünümü (açılışta salt-okunur yeniden çalışır).
void TuiSession::load_prefs() {
    if (std::string notice = tui::load_list_prefs(state_, config_.tuning.max_note_bytes); !notice.empty())
        status_.message += (status_.message.empty() ? "" : "; ") + noteapp::tui::terminal_safe(notice);
    list_actions_.restore_map_view();
}

void TuiSession::connect_search() {
    // GUI ile tek yol: içerik sonuç sınırı süzgeçten sonra; depo yeniden tarama iş parçacığıyla paylaşılır.
    state_.connect_store(
        store(), config_, [this]() -> const noteapp::search::ContentIndex& { return ensure_content_index(); },
        list_mutex_);
}

noteapp::search::ContentIndex& TuiSession::ensure_content_index() {
    if (!content_index_) {
        content_index_.emplace(store(), config_.tuning.max_note_bytes.cap(),
                               config_.limits.search.content_hits.cap());
        content_index_->refresh();
    }
    return *content_index_;
}

void TuiSession::show_sync() {
    status_.sync = tui::sync_text(sync_indicator_);
    status_.sync_state = sync_indicator_.state();
    state_.set_sync_enabled(config_.sync.enabled);
}

void TuiSession::notify_changed() {
    if (auto_sync_)
        auto_sync_->notify_changed();
}

// AutoSync geri çağrısı İŞÇİ iş parçacığından gelir: durum UI iş parçacığına Post edilir. Kapatıldıktan
// sonra gelen geç bildirim yok sayılır.
void TuiSession::start_auto_sync() {
    sync_indicator_.restore(noteapp::sync::SyncService(config_).status());
    show_sync();
    auto_sync_.emplace(config_, noteapp::core::note_executable_beside_self(),
                       [this](const noteapp::sync::SyncStatus& result, bool running) {
                           app_.Post([this, result, running] {
                               if (!auto_sync_)
                                   return;
                               const auto notice = sync_indicator_.update(result, running);
                               sync_status_.request(config_, state_.metas());
                               show_sync();
                               // Hatanın tam metni ileti satırına da yazılır: üst çubuktaki kısadır.
                               if (notice && notice->kind == app::NoticeKind::Error)
                                   status_.notify(notice->kind, notice->text);
                               else if (notice)
                                   status_.notices.post(notice->kind, notice->text, std::chrono::steady_clock::now());
                           });
                           app_.Post(redraw_event());  // kapanış görevi çizimi tetiklemez
                       });
    auto_sync_->start();
}

// Önizleme satır sayısı modele bildirilir ki PgDn son satırı aşmasın.
const tui::Preview& TuiSession::sync_preview() {
    const tui::Preview& shown = preview_.throttled(state_, store(), std::chrono::steady_clock::now());
    // Beklerken eski notun satır sayısı yeni notun kaydırmasını kırpmasın; erteleme bitince çizim (render'ın
    // uyanma anı) son seçimi okur.
    if (!preview_.deferred())
        state_.set_preview_lines(&shown.lines, preview_.generation(), shown.partial,
                                 shown.rendered ? &shown.styled : nullptr, &shown.wiki_targets);
    return shown;
}

// Editör/kayıt sonrası liste, önizleme ve (kuruluysa) içerik indeksi güncellenir. Taramanın görüntüsüne
// (`seen`) dokunulmaz: onu yalnız yeniden tarama iş parçacığı yazar; bir sonraki taramada yinelenen
// (zararsız) güncelleme olur.
void TuiSession::refresh_notes() {
    notes_refreshed_ = true;
    state_.map_edges_changed();  // ilişki eklemek listeyi değiştirmez
    preview_.invalidate();
    word_tables_.request();
    language_servers_.notes_changed();
    if (content_index_)
        content_index_->refresh();
    state_.set_notes(list_notes());
    state_.set_folders(list_folders());
    state_.set_trash_count(trash_count());
    sync_status_.request(config_, state_.metas());
    list_actions_.refresh_inspector();  // çekilen senkron özellik yazmış olabilir
}

void TuiSession::show_error(const core::Error& error) {
    status_.message = core::format_error(error);
}

std::unique_ptr<tui::SpellChecker> TuiSession::make_spell_checker() {
    return std::make_unique<tui::SpellChecker>(
        tui::dictionary_for_language(config_.language), [this] { app_.Post(redraw_event()); },
        tui::SpellChecker::kDefaultIdle, tui::SpellChecker::kDefaultTimeout,
        config_.limits.tui.spellcheck_line_bytes.cap());
}

ftxui::Element TuiSession::render() {
    const ftxui::Dimensions window = ftxui::Terminal::Size();
    state_.set_window(window.dimx, window.dimy);
    // PDF sayfa görünümünün çizimi: DA1'i FTXUI açılışta alır (geç gelirse sonraki çizimde geçerli); hücre boyu
    // yalnız pencere boyutu değişince sorulur (yazı tipi büyütme sütun sayısını da değiştirir).
    if (window.dimx != cell_window_.dimx || window.dimy != cell_window_.dimy) {
        cell_window_ = window;
        cell_pixels_ = terminal_cell_pixels();
    }
    preview_.set_sixel_cell(
        tui::sixel_cell(config_.tui.pdf_image, app_.TerminalCapabilities(), cell_pixels_));
    const tui::EditorViewport viewport = tui::editor_viewport(window, state_.layout());
    tui::EditorSession& editor = tabs_.active();
    editor.layout(viewport.columns, viewport.rows);  // boyut değişince de imleç görünür kalsın
    // Tanılar ve öneriler çizimden hemen önce alınır. Boşta bekleyen değişiklik, otomatik kayıt ve önizleme
    // ertelemesi kare istemez: en yakın süre uyanma anı olur (wake_due), o an tek çizim yazar/gönderir.
    const auto now = std::chrono::steady_clock::now();
    // Süresi geçmiş ama kipi bekleyen kayıt uyandırmaz: sıradaki tuş onu yapar.
    editor.tick(now);
    std::optional<std::chrono::steady_clock::time_point> wake_at;
    const auto wake_by = [&wake_at](std::optional<std::chrono::steady_clock::time_point> deadline) {
        if (deadline && (!wake_at || *deadline < *wake_at))
            wake_at = deadline;
    };
    if (const auto deadline = editor.autosave_deadline(); deadline && *deadline > now)
        wake_by(deadline);
    status_.notices.expire(now);  // bildirimin düşme anı bir uyanmadır, yoklama değil
    wake_by(status_.notices.next_deadline());
    // Gün dönünce listedeki "bugün" saatleri (HH:MM) tarihe döner (core::short_time_text): bir çizim.
    const auto wall = std::chrono::system_clock::now();
    wake_by(now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      core::next_local_midnight(wall) - wall));
    // Harita görünürse: kenarlar yalnız eskiyince okunur, yerleşim kare başına bir adım; durunca kare
    // istenmez. Çevrenin merkezi etkin sekmenin notu (GUI gibi), sekme yoksa seçili not.
    std::optional<core::NotePath> map_center = editor.path();
    if (!map_center && state_.selected_row())
        map_center = state_.selected_row()->path;
    state_.update_map(
        [this] {
            const std::scoped_lock lock(list_mutex_);
            return store().edges();
        },
        map_center);
    if (state_.step_map())
        ftxui::animation::RequestAnimationFrame();
    const tui::LanguageServers::Update lsp = language_servers_.update(editor, now);
    wake_by(lsp.send_at);
    if (!lsp.status.empty())
        lsp_status_ = lsp.status;
    // Editörün kendi iletisi ("Kaydedildi", hata) önce gelir: dil sunucusu iletisi satır boşalınca çıkar.
    // Taslak ilk kayıtta yol alır ve belge o karede açılır; ileti hemen yazılsaydı kaydın sonucunu örterdi.
    // Liste açıkken beklemede kalır: ileti editörle ilgilidir, liste çubuğundaki tuş ipuçlarını örtmez.
    if (!lsp_status_.empty() && editor.is_open() && editor.message().empty())
        editor.show_notice(std::exchange(lsp_status_, {}));
    const std::vector<tui::TabLabel> tabs = tabs_.labels();
    const tui::Preview& preview = sync_preview();
    preview.sixels.clear();  // bu karenin görüntülerini çizim yazar, kare yazıcısı basar
    wake_by(preview_.deferred_until());
    wake_at_ = wake_at;
    if (update_dialog_)
        update_dialog_->show(update_.view());
    return tui::render(state_, preview, status_, window, dialog_ ? &*dialog_ : nullptr,
                       confirm_ ? &*confirm_ : nullptr, help_ ? &*help_ : nullptr,
                       settings_ ? &*settings_ : nullptr, new_row_ ? &*new_row_ : nullptr, &editor,
                       spell_.get(), rename_ ? &*rename_ : nullptr, tabs,
                       update_dialog_ ? &*update_dialog_ : nullptr);
}

// Terminali editöre bırakır; dönünce FTXUI ekranı geri yükler.
std::expected<void, core::Error> TuiSession::edit_file(const std::filesystem::path& file) {
    std::expected<void, core::Error> result;
    app_.WithRestoredIO([&] { result = core::run_editor(config_, file); })();
    exit_if_terminated(app_);
    return result;
}

void TuiSession::open_selected() {
    const tui::Row* row = state_.selected_row();
    if (!row)
        return;
    // Editör dosya ister: not özel geçici dosyaya çıkar, çakışma denetimiyle geri yazılır, dosya silinir.
    const core::NotePath path = row->path;
    const auto changed = [&] {
        return store().edit_in_temp_file(
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
}

// Ctrl-R: aynı diyalog taşıma kipinde; editör ve geçici dosya yok.
void TuiSession::start_move() {
    const tui::Row* row = state_.selected_row();
    if (!row)
        return;
    target_ = row->path;
    tui::SaveDialogInit init =
        tui::move_dialog_init(row->path, store().folders(), config_.types, config_.tuning.slug_max_length);
    init.max_suggestions = config_.limits.tui.save_suggestions.cap();
    init.fuzzy_candidate_chars = config_.limits.search.fuzzy_candidate_chars.cap();
    dialog_.emplace(std::move(init));
}

// Taşıma diyaloğu açıkken tuş işleme; hata satır içi gösterilir, diyalog açık kalır.
void TuiSession::handle_dialog_key(tui::SaveDialogState& dialog, const Key key, const std::string& text) {
    switch (dialog.handle(key, text)) {
    case tui::SaveDialogState::Outcome::Editing:
        break;
    case tui::SaveDialogState::Outcome::Discard:
        dialog_.reset();
        break;
    case tui::SaveDialogState::Outcome::Submit: {
        const auto moved = [&] {
            const std::scoped_lock lock(list_mutex_);
            const tui::SaveRequest& request = dialog.request();
            return app::move_note(store(), *target_, request.folder, request.name, request.type);
        }();
        if (!moved) {
            dialog.set_error(core::format_error(moved.error()));
            break;
        }
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- start_move diyaloğu target_ ile açar
        const core::NotePath from = *target_;
        dialog_.reset();
        if (*moved == from)
            break;  // değişiklik yok
        tabs_.follow_moved(from.string(), moved->string());
        refresh_notes();
        state_.reveal(*moved);
        status_.notify(app::NoticeKind::Success,
                       core::tr("tui.moved", {{"from", from.string()}, {"to", moved->string()}}));
        notify_changed();
        break;
    }
    }
}

// `a`: Enter boş notu oluşturur ve EDIT'te açar (hatada satır açık kalır, metin kaybolmaz); Esc ve boş
// girdi hiçbir şey yazmaz.
void TuiSession::handle_new_row_key(tui::NewRowState& row, const Key key, const std::string& text) {
    switch (row.handle(key, text)) {
    case tui::NewRowState::Outcome::Editing:
        break;
    case tui::NewRowState::Outcome::Cancelled:
        new_row_.reset();
        break;
    case tui::NewRowState::Outcome::Submit:
        list_actions_.submit_new_row(
            new_row_,
            {new_note_folder(state_, config_), config_.default_type, config_.tuning.slug_max_length},
            [this](const core::NotePath& path) { open_tab(path, 0, false); });
        break;
    }
}

// Kaydedilen ayarlar canlı uygulanır: dil anında, notes_dir değişince store yeniden kurulup liste ve
// içerik indeksi sıfırlanır; diğer anahtarlar sonraki eylemlerde kullanılan `config`'e yansır. `tuning`
// alanına dokunulmaz: yeniden tarama iş parçacığı onu okur ve ayar ekranından değişmez.
void TuiSession::apply_settings(const core::Config& next) {
    const bool dir_changed = next.notes_dir != config_.notes_dir;
    const bool restart_sync = noteapp::tui::sync_restart_needed(config_, next);
    config_.language = next.language;
    config_.notes_dir = next.notes_dir;
    config_.editor = next.editor;
    config_.default_type = next.default_type;
    config_.default_folder = next.default_folder;
    config_.types = next.types;
    config_.icons = next.icons;
    state_.set_icon_style(core::terminal_icon_style(config_.icons));
    config_.sync = next.sync;
    config_.update = next.update;  // açılış denetimi sonraki açılışta okur; form yeniden açılınca görünür
    config_.limits.app.map_nodes = next.limits.app.map_nodes;  // app::adopt_live_limits: dosyadan gelir
    config_.limits.app.map_depth = next.limits.app.map_depth;
    state_.set_map_node_limit(config_.limits.app.map_nodes);
    state_.set_map_depth_limit(config_.limits.app.map_depth);
    // Tarama iş parçacığı tuning'in başka alanlarını okur; bu alan yalnız UI iş parçacığındadır.
    config_.limits.search.relation_depth = next.limits.search.relation_depth;
    config_.tuning.search_relations = next.tuning.search_relations;
    state_.set_relation_depth(noteapp::app::relation_depth(config_));
    core::init_i18n(config_.language);
    spell_ = make_spell_checker();
    // AutoSync süreleri ve depo yolu kurulurken okunur: ilgili ayar değiştiyse yeniden kurulur;
    // kapandıysa yıkılır. Yıkıcı stop(0) çağırıp süren birleştirmeyi öldürürdü: önce düzgün durdurulur.
    const auto stop_auto_sync = [this] {
        if (!auto_sync_)
            return;
        auto_sync_->stop(std::chrono::seconds(config_.tuning.exit_timeout_seconds));
        auto_sync_.reset();
    };
    if (!config_.sync.enabled) {
        stop_auto_sync();
        sync_indicator_.disable();
        show_sync();
    } else if (!auto_sync_ || restart_sync) {
        stop_auto_sync();
        start_auto_sync();
    }
    if (dir_changed) {
        {
            const std::scoped_lock lock(list_mutex_);
            content_index_.reset();  // ContentIndex eski store'a referans tutar
            store_.emplace(core::NoteStore::for_config(config_));
        }
        state_.clear_map_query();  // eski deponun sonucu ve yolda olan açılış görünümü yeni depoya uygulanmaz
        word_tables_.set_store(store());
        language_servers_.set_store(store());
        refresh_notes();
    }
}

void TuiSession::handle_settings_key(tui::SettingsState& settings, const Key key, const std::string& text) {
    const auto outcome = settings.handle(key, text);
    switch (outcome) {
    case tui::SettingsState::Outcome::Editing:
        break;
    case tui::SettingsState::Outcome::Cancel:
        settings_.reset();
        break;
    case tui::SettingsState::Outcome::Test:
        // Önceki işçi sürerken yenisi başlamaz: atama onu beklerdi, arayüz donardı.
        if (connection_testing_)
            break;
        if (auto candidate = settings.begin_test(config_))
            start_connection_test(std::move(*candidate));
        break;
    case tui::SettingsState::Outcome::CheckUpdates:
        run_intent(tui::Intent::CheckUpdates, key, text);  // palet `update` ile aynı akış; form açık kalır
        break;
    case tui::SettingsState::Outcome::Quit:
        app_.Exit();
        break;
    case tui::SettingsState::Outcome::Submit:
    case tui::SettingsState::Outcome::SubmitAndQuit:
    case tui::SettingsState::Outcome::Replace:  // origin değiştirme onaylandı
        // Hata formda satır içi gösterilir (çıkılmaz); origin farklıysa form onay penceresini açar.
        if (const auto saved = settings.submit(config_, outcome == tui::SettingsState::Outcome::Replace)) {
            apply_settings(*saved);
            settings_.reset();
            status_.notify(app::NoticeKind::Success, core::tr("tui.settings_saved"));
            if (outcome == tui::SettingsState::Outcome::SubmitAndQuit)
                app_.Exit();
        }
        break;
    }
}

void TuiSession::start_connection_test(core::Config candidate) {
    connection_testing_ = true;
    connection_test_ = std::jthread([this, candidate = std::move(candidate)] {
        auto outcome = app::run_connection_test(candidate, app::system_sync_enabler());
        app_.Post([this, outcome = std::move(outcome)] {
            connection_testing_ = false;
            if (settings_)
                settings_->finish_test(outcome);
            status_.notices.post(outcome ? app::NoticeKind::Success : app::NoticeKind::Error,
                                 outcome ? core::tr("app.test_ok")
                                         : core::tr("app.test_failed", {{"detail", core::format_error(outcome.error())}}),
                                 std::chrono::steady_clock::now());
            if (outcome)
                refresh_notes();  // çekilen notlar listeye yansısın
        });
        app_.Post(redraw_event());
    });
}

void TuiSession::handle_modal_key(const Key key, const std::string& text) {
    // Çıkışta geçici dosya silinmez: yazılan metin state dizininde kalır. Kaydedilmemiş Ayarlar önce sorar
    // (GUI pencere kapanışı gibi); soru açıkken Ctrl+C onu açık tutar.
    if (tui::quits_from_modal(key, state_.focus())) {
        if (!settings_ || !settings_->ask_before_quit()) {
            app_.Exit();
        } else if (update_dialog_) {  // soru formdadır: üstteki güncelleme penceresi onu örtmesin
            update_.act(app::UpdateAction::Close);
            update_dialog_.reset();
        }
    } else if (help_) {
        if (!help_->handle(key, text))
            help_.reset();
    } else if (update_dialog_)  // Ayarlar'ın düğmesiyle formun üstünde açılır (render'daki sıra)
        handle_update_key(key);
    else if (settings_)
        handle_settings_key(*settings_, key, text);
    else if (confirm_) {
        switch (confirm_->handle(key, text)) {
        case tui::ConfirmState::Outcome::Pending:
            break;
        case tui::ConfirmState::Outcome::Cancelled:
            confirm_.reset();
            break;
        case tui::ConfirmState::Outcome::Confirmed:
            list_actions_.finish_delete(confirm_);
            break;
        }
    } else if (new_row_)
        handle_new_row_key(*new_row_, key, text);
    else if (rename_)
        list_actions_.handle_rename_key(rename_, key, text);
    else
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- any_open: kalan tek açık modal diyalogdur
        handle_dialog_key(*dialog_, key, text);
}

// Güncelleme penceresinin tuşu (handle_modal_key, pencere açıkken): içerik işçinin son durumundan; bağlantıyı
// kopyalamak arayüzün işidir (pano).
void TuiSession::handle_update_key(const Key key) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- handle_modal_key yalnız açıkken çağırır
    tui::UpdateDialogState& dialog = *update_dialog_;
    dialog.show(update_.view());
    const std::optional<app::UpdateAction> action = dialog.handle(key);
    if (!action)
        return;
    if (*action == app::UpdateAction::CopyLink) {
        const std::string& link = dialog.view().link;
        status_.message = tui::copy_message(system_clipboard_.copy(link), link.size());
        return;
    }
    update_.act(*action);
    if (!update_.is_open())
        update_dialog_.reset();
}

void TuiSession::request_sync_now() {
    if (auto_sync_)
        auto_sync_->request_now();
    else
        status_.message = core::tr("tui.sync_disabled_hint");
}

// Ctrl-C'yi de kendimiz işleriz: FTXUI'ye bırakılırsa döngüden sonra SIGINT'i yeniden yükseltir ve süreç
// yıkıcılar (kod sayfası geri yüklemesi dahil) çalışmadan biter.
bool TuiSession::on_event(ftxui::Event event) {
    if (event == ftxui::Event::CtrlL) {
        frame_diff_.invalidate();
        return true;
    }
    // Fare olayı FTXUI'ye bırakılmaz: kendi seçim boyamasını başlatırdı. Modal açıkken yok sayılır.
    if (event.is_mouse()) {
        if (!any_open(help_, confirm_, dialog_, settings_, new_row_, rename_, update_dialog_))
            list_actions_.on_mouse(to_mouse(event.mouse()), tabs_.active());
        settle_tabs();
        list_actions_.save_prefs_if_changed();
        sync_preview();
        return true;
    }
    std::string text;
    std::optional<Key> key;
    if (const auto pasted = pasted_text(event)) {
        const std::size_t limit = config_.limits.tui.paste_bytes.cap();
        tui::PasteChoice choice = *pasted == kPasteTooLarge
                                      ? tui::PasteChoice{{}, tui::paste_too_large_message(limit)}
                                      : tui::terminal_paste(*pasted, limit);
        if (!choice.message.empty()) {  // Ctrl+V gibi: metin değişmez, neden editörde ya da altta
            if (state_.focus() == tui::Focus::Editor)
                tabs_.active().show_notice(std::move(choice.message));
            else
                status_.message = std::move(choice.message);
            return true;
        }
        key = Key::Paste;
        text = std::move(choice.text);
    } else {
        key = to_key(event, text);
    }
    if (!key || tui::passes_to_terminal(*key, state_.focus()))
        return false;
    if (any_open(help_, confirm_, dialog_, settings_, new_row_, rename_, update_dialog_)) {
        handle_modal_key(*key, text);
        settle_tabs();
        list_actions_.save_prefs_if_changed();
        sync_preview();
        return true;
    }
    if (list_actions_.cancel_import_on(*key, state_.focus()))  // durduruluyor iletisi kalsın
        return true;
    status_.message.clear();
    status_.notice.clear();
    status_.notices.dismiss_all(app::NoticeKind::Error);  // hata bildirimi sonraki tuşla kapanır
    run_intent(state_.handle(*key, text), *key, text);
    settle_tabs();
    list_actions_.save_prefs_if_changed();
    sync_preview();
    return true;
}

void TuiSession::run_intent(const tui::Intent intent, const Key key, const std::string& text) {
    switch (intent) {
    case tui::Intent::None:
        break;
    case tui::Intent::Quit:
        app_.Exit();
        break;
    case tui::Intent::OpenSelected:
        open_selected();
        break;
    case tui::Intent::NewRow:
        new_row_.emplace(state_.name_input_sources());
        break;
    case tui::Intent::NewFolderRow:
        // GUI ile aynı hedef: seçili klasör, yoksa notun ya da listenin klasörü (app::new_note_folder).
        new_row_.emplace(tui::NewRowState::FolderName{},
                         state_.name_prefix_for(new_note_folder(state_, config_)),
                         state_.name_input_sources());
        break;
    case tui::Intent::NewNote: {
        const std::string folder = new_note_folder(state_, config_);
        state_.open_new_note_view();  // on_opened'dan önce: odak boş editöre, panel yakınlaşır
        if (!start_draft([&](tui::EditorSession& editor) { editor.begin_new_note(folder); }, false))
            state_.leave_editor(std::nullopt);
        break;
    }
    case tui::Intent::CreateFromQuery: {
        // nvALT: sorgu yeni notun ilk satırı olur
        const std::string first = state_.query() + "\n";
        const std::string folder = new_note_folder(state_, config_);
        start_draft([&](tui::EditorSession& editor) { editor.begin_draft(first, folder); }, false);
        break;
    }
    case tui::Intent::MoveSelected:
        start_move();
        break;
    case tui::Intent::RenameSelected:
        list_actions_.begin_rename(rename_);
        break;
    case tui::Intent::MoveTo:
        list_actions_.begin_move_to(rename_);
        break;
    case tui::Intent::ImportFiles:
        list_actions_.begin_import(rename_, new_note_folder(state_, config_));  // GUI ile aynı hedef
        break;
    case tui::Intent::ShowHelp:
        help_.emplace(tui::help_lines().size());
        break;
    case tui::Intent::ShowSettings:
        settings_.emplace(config_);
        break;
    case tui::Intent::EnterEditor:
        open_tab(state_.selected_row()->path, state_.editor_start_line(), false);
        break;
    case tui::Intent::EditorKey:
        tabs_.active().handle(key, text, state_.input_sources());
        tabs_.mark_edited();
        break;
    case tui::Intent::TabOpenSelected:
        if (const tui::Row* row = state_.selected_row())
            open_tab(row->path, 0, true);
        break;
    case tui::Intent::TabNew: {
        const std::string folder = new_note_folder(state_, config_);
        start_draft([&](tui::EditorSession& editor) { editor.begin_new_note(folder); }, true);
        break;
    }
    case tui::Intent::TabNext:
    case tui::Intent::TabPrevious:
        // Soru ya da girdi açıkken sekme değişmez: tuş oturuma gider (bul satırında etkisizdir).
        if (state_.focus() == tui::Focus::Editor &&
            tabs_.active().mode() != tui::EditorSession::Mode::Editing)
            break;
        if (tabs_.cycle(intent == tui::Intent::TabNext ? 1 : -1) || !tabs_.active().is_open())
            show_active_tab();
        break;
    case tui::Intent::TabClose:
        if (tabs_.active().mode() == tui::EditorSession::Mode::Editing)
            tabs_.close_active();
        break;
    case tui::Intent::OpenMapNode:
        if (const auto path = state_.map_selected())
            open_tab(*path, 0, false);
        break;
    case tui::Intent::ParkEditor:
        if (tabs_.active().mode() == tui::EditorSession::Mode::Editing && tabs_.leave_active())
            state_.search_from_editor();
        break;
    case tui::Intent::SyncNow:
        request_sync_now();
        break;
    case tui::Intent::CheckUpdates:
        update_.open(status_.notices);
        update_dialog_.emplace();
        break;
    default:
        list_actions_.run(intent, confirm_, rename_);
        break;
    }
}

bool TuiSession::wake_due(std::chrono::steady_clock::time_point now) {
    if (!wake_at_ || now < *wake_at_)
        return false;
    wake_at_.reset();
    return true;
}

void TuiSession::rescan_loop(const std::stop_token& stop, std::vector<core::NoteInfo> listing,
                             const std::uint64_t listing_version) {
    std::mutex mutex;
    std::condition_variable_any wake;
    const auto period = std::chrono::seconds(config_.tuning.rescan_seconds);
    // Yalnız bu iş parçacığı okur/yazar.
    tui::StoreScan seen{std::move(listing), trash_count(), list_folders(), listing_version};
    while (true) {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, stop, period, [] { return false; });  // durdurma isteğinde erken döner
        if (stop.stop_requested())
            return;
        tui::StoreScan latest = scan();
        if (tui::same_scan(seen, latest))
            continue;
        seen = latest;
        app_.Post([this, latest = std::move(latest)]() mutable {
            preview_.invalidate();
            word_tables_.request();
            language_servers_.notes_changed();
            if (content_index_)
                content_index_->refresh();  // yalnız mtime/boyutu değişenler yeniden okunur
            // Senkron çekmesi de buradan geçer: her sekme kendi notunu denetler.
            const std::uint64_t focused = state_.focus() == tui::Focus::Editor ? tabs_.active_id() : 0;
            report_closed_tabs(tabs_.on_listing(latest.notes), focused);
            state_.set_notes(std::move(latest.notes));
            state_.set_folders(std::move(latest.folders));
            state_.set_views(std::move(latest.views));
            state_.map_edges_changed();  // yalnız ilişki değişmiş olabilir (graph_version)
            state_.set_trash_count(latest.trash);
            sync_status_.request(config_, state_.metas());
            list_actions_.refresh_trash_view();
            list_actions_.refresh_inspector();  // senkron ya da başka süreç özellik yazmış olabilir
            settle_tabs();
            list_actions_.save_prefs_if_changed();
            sync_preview();
        });
        app_.Post(redraw_event());
    }
}

int TuiSession::run() {
    // Ctrl-C ve Ctrl-Z'yi biz işleriz: FTXUI varsayılanı işlensin işlenmesin sinyal yükseltir, editörde
    // kopyala ve geri al süreci öldürür ya da askıya alırdı.
    app_.ForceHandleCtrlC(false);
    app_.ForceHandleCtrlZ(false);
    // FTXUI eski işleyiciyi saklar, döngüden sonra sinyali ona yeniden yükseltir: işleyicimiz süreci
    // öldürmeden döngüden çıkışa ve aşağıdaki kayda izin verir.
    tui::install_termination_handlers();
    app_.Post([this] { exit_if_terminated(app_); });
    // Açılış güncelleme denetimi arayüz açıldıktan sonra, işçide: açılışı ve tuşları beklemez.
    app_.Post([this] {
        update_.check_on_startup(std::chrono::system_clock::now(), config_.update.check_interval);
    });
    run_event_loop(
        app_, ftxui::CatchEvent(view_, [this](ftxui::Event event) { return on_event(std::move(event)); }),
        [this] { return wake_due(std::chrono::steady_clock::now()); });

    // Dış sonlandırma dahil her çıkış: kirli metin sessizce düşmez. Senkrondan önce: kayıt da gönderilsin.
    for (const std::string& note : tabs_.flush_on_exit())
        print_exit_note(note);
    // Son sekme durumu: çıkışta yazılan taslak artık not, sekmesi kalır.
    state_.set_saved_tabs(tabs_.saved());
    list_actions_.save_prefs_if_changed();

    // Süren denetim ve indirme iptal edilir; değiştirme evresindeki kurulum iptali yok sayar, çıkış onu
    // bekler (update_'in yıkıcısı): kullanıcı nedenini görsün.
    if (update_.stop_for_exit())
        std::cerr << core::tr("tui.update_exiting") << '\n';

    // Çıkış (Ctrl-C dahil, ikisi de app.Exit): bekleyen değişiklik için son bir senkron, en çok exit_timeout.
    // Terminal artık normal kipte; kullanıcı bekleyişin nedenini görsün.
    if (auto_sync_) {
        std::cerr << core::tr("tui.sync_exiting") << '\n';
        auto_sync_->stop(std::chrono::seconds(config_.tuning.exit_timeout_seconds));
    }
    return tui::termination_exit_code();
}

constexpr int kExitNothingImported = 1;
constexpr int kExitUsage = 2;

// `notes import`: CLI'nin raporu terminale, TUI açılmadan. Hiçbir not alınamadıysa TUI açılmaz, çıkış kodu
// döner; alındıysa ilk not açılışta gösterilir.
std::expected<Opening, int> import_at_startup(const app::UiArgs& args, const core::Config& config) {
    std::string folder =
        args.folder.value_or(app::new_note_folder({.selected_folder = nullptr,
                                                   .scope_folder = {},
                                                   .note = nullptr,
                                                   .default_folder = config.default_folder}));
    if (!folder.empty()) {
        const auto parsed = core::NotePath::parse(folder);
        if (!parsed) {
            std::cerr << tui::terminal_safe(core::format_error(parsed.error())) << '\n';
            return std::unexpected(kExitUsage);
        }
        folder = parsed->string();
    }
    core::NoteStore store = core::NoteStore::for_config(config);
    if (const auto issue = store.health()) {
        std::cerr << tui::terminal_safe(core::format_error(*issue)) << '\n';
        return std::unexpected(kExitNothingImported);
    }
    app::ImportOptions options = app::import_options(config, std::move(folder));
    options.name = args.name;
    options.overwrite = args.overwrite;
    const app::ImportReport report = app::import_paths(store, args.sources, options);
    for (const app::ImportLine& line : app::import_report_lines(report))
        (line.problem ? std::cerr : std::cout) << tui::terminal_safe(line.text) << '\n';
    if (report.imported.empty())
        return std::unexpected(kExitNothingImported);
    return Opening{report.imported.front().note, tui::terminal_safe(app::import_status(report))};
}

}  // namespace

int main(int argc, char** argv) {
    core::restrict_dll_search_to_system32();
    // FTXUI konsolu UTF-8'e çeker ama geri almaz; eski kod sayfasını biz saklayıp geri yükleriz.
    const core::ConsoleUtf8Scope utf8_console;
    // Dil ve log eşiği config'ten gelir; yükleme uyarıları log'a düşer ve açılışta alt çubukta gösterilir.
    const core::LoadedConfig loaded = core::load_config();
    core::init_i18n(loaded.config.language);
    core::init_log(loaded.config.tuning.log_max_bytes);
    for (const auto& warning : loaded.warnings)
        core::log_warn(core::format_error(warning));
    app::cleanup_previous_update();  // önceki güncellemenin artıkları

    const app::UiArgs args = app::parse_ui_args(core::utf8_arguments(argc, argv), true);
    std::optional<Opening> opening;
    switch (args.kind) {
    case app::UiArgs::Kind::Run:
        break;
    case app::UiArgs::Kind::Help:
        std::cout << core::tr("tui.usage");
        return 0;
    case app::UiArgs::Kind::Version:
        std::cout << "notes " << core::version() << '\n';
        return 0;
    case app::UiArgs::Kind::Invalid:
        if (!args.error_key.empty())
            std::cerr << tui::terminal_safe(core::tr(args.error_key, args.error_args)) << '\n';
        std::cerr << core::tr("tui.usage");
        return kExitUsage;
    case app::UiArgs::Kind::Import: {
        auto imported = import_at_startup(args, loaded.config);
        if (!imported)
            return imported.error();
        opening = std::move(*imported);
        break;
    }
    }
    // Oturum (FTXUI app_ dahil) utf8_console'dan önce yıkılır: kod sayfası en son geri yüklenir.
    TuiSession session(loaded, opening);
    return session.run();
}
