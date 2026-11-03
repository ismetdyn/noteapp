#include "core/config.hpp"
#include "core/i18n.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/editor_view.hpp"
#include "tui/language_servers.hpp"
#include "tui/pane_layout.hpp"
#include "tui/server_settings.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <doctest/doctest.h>
#include <fstream>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace noteapp;
using tui::EditorSession;
using tui::Key;
using tui::LanguageServers;
namespace fs = std::filesystem;
namespace lsp = core::lsp;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kWidth = 100;
constexpr int kHeight = 24;
constexpr auto kArrival = 10s;  // sanitizer altında süreç başlatma yavaş
constexpr std::size_t kLimit = 4 * 1024 * 1024;

using Argv = std::vector<std::string>;

constexpr std::string_view kNetworkPathStatus =
    R"(LSP off for this note: it references a network path (\\host or //host))";

// Sunucu yalnız mutlak yolla aranır; Wine'da derleme tanımı sürücüsüz "/build/..." yoludur, Windows'ta göreli
// sayılır.
std::string fake_server_path() {
    return fs::absolute(NOTEAPP_FAKE_LSP_SERVER).string();
}

core::LspConfig fake_servers(const char* mode, std::vector<std::string> languages = {"python"}) {
    core::LspConfig config;
    config.enabled = true;
    for (std::string& language : languages)
        config.servers.emplace(std::move(language), std::vector<Argv>{{fake_server_path(), mode}});
    return config;
}

struct Editor {
    test::TempDir dir;
    fs::path workspace = dir.path() / "state" / "lsp" / "workspace";
    core::NoteStore store{dir.path() / "data"};
    std::mutex mutex;
    std::string clipboard;
    tui::TuiState state;
    std::atomic<int> redraws{0};
    EditorSession session{
        tui::EditorHost{store, mutex, clipboard, tui::terminal_cell_width, kLimit,
                        core::kDefaultSlugMaxLength, [](std::string_view) { return std::string("py"); },
                        [](const core::NotePath&) {}, [this] { state.enter_editor(); },
                        [this](std::optional<std::size_t> line) { state.leave_editor(line); },
                        [](const core::Error&) {}, [] {}}};
    std::optional<LanguageServers> servers;

    // Ağ yolu kuralı varsayılanda yalnız Windows'ta; engelleme testleri Linux'ta açıkça açar.
    explicit Editor(core::LspConfig config, bool block_network_paths = tui::kBlockNetworkPaths) {
        core::init_i18n("en");
        servers.emplace(
            std::move(config), workspace, store, [this] { ++redraws; }, 50ms, 0ms, block_network_paths);
    }

    void open(std::string_view path, std::string_view content) {
        REQUIRE(test::seed_note(store, path, content));
        state.set_notes(store.list());
        auto parsed = core::NotePath::parse(path);
        REQUIRE(parsed.has_value());
        session.open(*parsed);
        REQUIRE(session.is_open());
        REQUIRE(session.read_only() == EditorSession::ReadOnly::No);
    }

    LanguageServers::Update update() { return servers->update(session, Clock::now()); }

    template <class Predicate>
    bool pump_until(Predicate predicate, std::chrono::milliseconds limit = kArrival) {
        const auto deadline = Clock::now() + limit;
        while (Clock::now() < deadline) {
            update();
            if (predicate())
                return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    ftxui::Screen frame() {
        const auto viewport = tui::editor_viewport({kWidth, kHeight}, state.layout());
        session.layout(viewport.columns, viewport.rows);
        auto screen =
            ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
        ftxui::Render(screen, tui::render(state, {}, {"sync off", ""}, {kWidth, kHeight}, nullptr, nullptr,
                                          nullptr, nullptr, nullptr, &session));
        return screen;
    }
};

std::string row_text(const ftxui::Screen& screen, int y) {
    std::string out;
    for (int x = 0; x < screen.dimx(); ++x)
        out += screen.PixelAt(x, y).character;
    return out;
}

// `needle`ın ilk karakterinin hücresi; yoksa (-1, -1).
std::pair<int, int> find_cell(const ftxui::Screen& screen, std::string_view needle) {
    for (int y = 0; y < screen.dimy(); ++y)
        for (int x = 0; x < screen.dimx(); ++x) {
            std::string tail;
            for (int at = x; at < screen.dimx() && tail.size() < needle.size(); ++at)
                tail += screen.PixelAt(at, y).character;
            if (tail.starts_with(needle))
                return {x, y};
        }
    return {-1, -1};
}

bool has_item(const EditorSession& session, std::string_view label) {
    return std::ranges::find(session.completion_items(), label) != session.completion_items().end();
}

std::string selected_item(const EditorSession& session) {
    const auto& items = session.completion_items();
    return items.empty() ? std::string() : items[session.completion_selected()];
}

}  // namespace

TEST_CASE("the default config without installed servers starts nothing, mirrors nothing, tells once per "
          "language") {
    const test::TempDir empty_bin;
    const test::ScopedEnv path("PATH", empty_bin.path().string());  // hiçbir sunucu ikilisi bulunmaz
    Editor editor(core::parse_config("").config.lsp);
    const auto statuses = [&] {
        std::vector<std::string> out;
        for (int i = 0; i < 10; ++i) {
            if (std::string status = editor.update().status; !status.empty())
                out.push_back(std::move(status));
            std::this_thread::sleep_for(5ms);
        }
        return out;
    };
    editor.open("a.py", "import os\n");
    editor.session.handle(Key::Char, "x");
    CHECK(statuses() == std::vector<std::string>{"pyright-langserver not found, LSP is off for python"});
    editor.open("b.py", "x = 1\n");
    CHECK(statuses().empty());  // aynı dil: ikinci kez söylenmez
    editor.open("c.cpp", "int x;\n");
    CHECK(statuses() == std::vector<std::string>{"clangd not found, LSP is off for cpp"});
    editor.open("n.txt", "plain\n");
    CHECK(statuses().empty());
    CHECK(editor.servers->started_clients() == 0);
    CHECK_FALSE(fs::exists(editor.workspace.parent_path()));  // ne yansıtma ne dizin
    CHECK(editor.session.diagnostics().empty());
}

// Varsayılan açık LSP'de klonlanmış bir deponun node_modules/.bin'i ya da "." PATH girdisi sunucu yerine
// geçmemeli: göreli PATH girdisi atlanır, mutlak olan kullanılır.
TEST_CASE("a server found only through a relative PATH entry is not started, an absolute entry is") {
    const test::TempDir cwd;
    const fs::path bin = cwd.path() / "bin";
    fs::create_directories(bin);
    const fs::path fake(NOTEAPP_FAKE_LSP_SERVER);
    fs::path copy = bin / "noteapp-relative-ls";
    copy += fake.extension();  // Windows: .exe
    fs::copy_file(fake, copy);
    fs::permissions(copy, fs::perms::owner_all, fs::perm_options::replace);
    core::LspConfig config;
    config.enabled = true;
    config.servers.emplace("python", std::vector<Argv>{{"noteapp-relative-ls", "normal"}});
    const fs::path old_cwd = fs::current_path();
    fs::current_path(cwd.path());
    {
        const test::ScopedEnv path("PATH", "bin");
        Editor editor(config);
        editor.open("a.py", "x = 1\n");
        std::vector<std::string> statuses;
        for (int i = 0; i < 10; ++i) {
            if (std::string status = editor.update().status; !status.empty())
                statuses.push_back(std::move(status));
            std::this_thread::sleep_for(5ms);
        }
        CHECK(statuses == std::vector<std::string>{"noteapp-relative-ls not found, LSP is off for python"});
        CHECK(editor.servers->started_clients() == 0);
    }
    {
        const test::ScopedEnv path("PATH", bin.string());
        Editor editor(config);
        editor.open("a.py", "x = 1\n");
        CHECK(editor.pump_until([&] { return !editor.session.diagnostics().empty(); }));
        CHECK(editor.servers->started_clients() == 1);
    }
    fs::current_path(old_cwd);
}

TEST_CASE("a disabled or empty lsp config starts nothing and creates no workspace directory") {
    for (const core::LspConfig& config :
         {core::LspConfig{.enabled = true, .servers = {}},
          core::LspConfig{.enabled = false, .servers = fake_servers("normal").servers}}) {
        Editor editor(config);
        editor.open("a.py", "import os\n");
        editor.session.handle(Key::Char, "x");
        for (int i = 0; i < 20; ++i) {
            CHECK_FALSE(editor.update().wants_frame);
            std::this_thread::sleep_for(10ms);
        }
        CHECK(editor.servers->started_clients() == 0);
        CHECK_FALSE(fs::exists(editor.workspace));
        CHECK_FALSE(fs::exists(editor.workspace.parent_path()));
        CHECK(editor.session.diagnostics().empty());
    }
}

TEST_CASE("diagnostics are underlined and the message under the cursor shows in the message line") {
    Editor editor(fake_servers("normal"));
    editor.open("pkg/a.py", "import os\nx = 1\n");
    REQUIRE(editor.pump_until([&] { return !editor.session.diagnostics().empty(); }));
    CHECK(editor.servers->started_clients() == 1);

    const ftxui::Screen screen = editor.frame();
    const auto [x, y] = find_cell(screen, "import os");
    REQUIRE(x >= 0);
    CHECK(screen.PixelAt(x, y).underlined);
    CHECK(screen.PixelAt(x + 8, y).underlined);
    const auto [x2, y2] = find_cell(screen, "x = 1");
    REQUIRE(x2 >= 0);
    CHECK_FALSE(screen.PixelAt(x2, y2).underlined);
    // İmleç tanının üstünde: ileti en alt satırda.
    CHECK(row_text(screen, kHeight - 1).find("count=1 text=import os") != std::string::npos);

    // İmleç tanıdan çıkınca ileti gider.
    editor.session.handle(Key::Down);
    CHECK(row_text(editor.frame(), kHeight - 1).find("count=") == std::string::npos);

    // Çalışma alanı yansıtması aynı dosyayı yazar.
    REQUIRE(editor.pump_until([&] { return fs::exists(editor.workspace / "pkg" / "a.py"); }));
}

TEST_CASE("typing sends the full text after the idle debounce with a growing version") {
    Editor editor(fake_servers("normal"));
    editor.open("a.py", "abc\n");
    REQUIRE(editor.pump_until([&] { return !editor.session.diagnostics().empty(); }));
    editor.session.handle(Key::Char, "x");
    CHECK(editor.update().wants_frame);  // boşta süre dolmadı: çizim tekrar istenir
    editor.session.handle(Key::Char, "y");
    REQUIRE(editor.pump_until([&] {
        const auto marks = editor.session.diagnostics();
        return !marks.empty() && marks.front().message.starts_with("count=2 text=xyabc");
    }));
    CHECK_FALSE(editor.update().wants_frame);
}

TEST_CASE("escape and bidi characters in a server message never reach the terminal") {
    Editor editor(fake_servers("normal"));
    // Sunucu ilk satırı iletide geri yansıtır: kötü niyetli bir sunucunun yapabileceği gibi.
    editor.open("a.py", "x = 1 # \x1b]0;owned\x07 \x1b[2J \xE2\x80\xAE evil\n");
    REQUIRE(editor.pump_until([&] { return !editor.session.diagnostics().empty(); }));
    REQUIRE(editor.session.diagnostic_at_cursor() != nullptr);
    REQUIRE(editor.session.diagnostic_at_cursor()->message.find('\x1b') != std::string::npos);

    const ftxui::Screen screen = editor.frame();
    const std::string status = row_text(screen, kHeight - 1);
    CHECK(status.find("count=1") != std::string::npos);
    CHECK(status.find("\xEF\xBF\xBD") != std::string::npos);  // U+FFFD yerine konur
    CHECK(status.find("\xE2\x80\xAE") == std::string::npos);
    for (int y = 0; y < kHeight; ++y) {
        const std::string row = row_text(screen, y);
        CHECK(row.find('\x1b') == std::string::npos);
        CHECK(row.find('\x07') == std::string::npos);
    }
}

TEST_CASE("completion items from the server join the popup and an answer for an older request is dropped") {
    Editor editor(fake_servers("slow-completion"));
    editor.open("a.py", "print \n");
    REQUIRE(editor.pump_until([&] { return !editor.session.diagnostics().empty(); }));
    editor.session.handle(Key::End);
    editor.session.handle(Key::CtrlSpace);
    editor.update();  // istek: konum 0:6
    editor.session.handle(Key::Escape);
    editor.session.handle(Key::Char, " ");
    editor.session.handle(Key::CtrlSpace);  // yeni açılış, yeni metin: konum 0:7

    bool stale_seen = false;
    // Liste beklerken tuş almaya devam eder.
    editor.session.handle(Key::Down);
    editor.session.handle(Key::Up);
    REQUIRE(editor.pump_until([&] {
        stale_seen = stale_seen || has_item(editor.session, "pos:0:6");
        return has_item(editor.session, "pos:0:7");
    }));
    std::this_thread::sleep_for(500ms);  // eski cevap geç gelse de listeye girmez
    editor.update();
    CHECK_FALSE(stale_seen);
    CHECK_FALSE(has_item(editor.session, "pos:0:6"));
    REQUIRE(has_item(editor.session, "alpha"));
    CHECK(editor.session.completion_items().front() == "pos:0:7");  // sunucu önerileri önce
    // Cevap geldiğinde seçili aday ("print") seçili kalır: gelen liste kullanıcının seçimini kaydırmaz.
    CHECK(selected_item(editor.session) == "print");

    for (int i = 0; i < 8; ++i)
        editor.session.handle(Key::Up);
    editor.session.handle(Key::Down);
    REQUIRE(selected_item(editor.session) == "alpha");
    editor.session.handle(Key::Enter);
    CHECK(editor.session.buffer()->line_text(0) == "print  alpha()");
}

TEST_CASE("a server answer for a closed popup changes nothing") {
    Editor editor(fake_servers("slow-completion"));
    editor.open("a.py", "x\n");
    REQUIRE(editor.pump_until([&] { return !editor.session.diagnostics().empty(); }));
    editor.session.handle(Key::CtrlSpace);
    editor.update();
    editor.session.handle(Key::Escape);
    std::this_thread::sleep_for(600ms);
    editor.update();
    CHECK(editor.session.mode() == EditorSession::Mode::Editing);
    CHECK(editor.session.buffer()->text() == "x\n");
}

TEST_CASE("completion insertion uses the insert text only when it is one plain line") {
    Editor editor(core::LspConfig{});
    editor.open("a.py", "\n");
    editor.session.handle(Key::CtrlSpace);
    const std::uint64_t generation = editor.session.completion_generation();
    const std::vector<lsp::CompletionItem> items = {
        {.label = "plain", .insert_text = "plain()", .kind = 0},
        {.label = "multi", .insert_text = "a\nb", .kind = 0},
        {.label = "esc\x1b[2J", .insert_text = "x\x1b[2Jy", .kind = 0},
        {.label = "tab", .insert_text = "a\tb", .kind = 0},
        {.label = "bidi",
         .insert_text = "a\xE2\x80\xAE"
                        "b",
         .kind = 0},
    };
    editor.session.add_completion_items(generation - 1, items);  // eski açılışın cevabı
    CHECK_FALSE(has_item(editor.session, "plain"));
    editor.session.add_completion_items(generation, items);
    // Etiket temizlenerek gösterilir.
    CHECK(has_item(editor.session, "esc\xEF\xBF\xBD[2J"));
    editor.session.handle(Key::Escape);

    const auto insert = [&](std::string_view label) {
        editor.session.handle(Key::CtrlZ);  // önceki eklemeyi geri al: hep boş satırdan başla
        editor.session.handle(Key::CtrlSpace);
        editor.session.add_completion_items(editor.session.completion_generation(), items);
        for (std::size_t i = 0; i < items.size() && selected_item(editor.session) != label; ++i)
            editor.session.handle(Key::Down);
        editor.session.handle(Key::Enter);
        return std::string(editor.session.buffer()->line_text(0));
    };
    CHECK(insert("plain") == "plain()");
    CHECK(insert("multi") == "multi");
    CHECK(insert("esc\xEF\xBF\xBD[2J") == "esc\xEF\xBF\xBD[2J");
    CHECK(insert("tab") == "tab");
    CHECK(insert("bidi") == "bidi");
    CHECK(editor.session.buffer()->text().find('\x1b') == std::string::npos);
}

TEST_CASE("a crashed server is reported once and not restarted") {
    Editor editor(fake_servers("crash"));
    editor.open("a.py", "x = 1\n");
    std::string status;
    const auto deadline = Clock::now() + kArrival;
    while (status.empty() && Clock::now() < deadline) {
        status = editor.update().status;
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE_FALSE(status.empty());
    CHECK(status.find("fake_lsp_server") != std::string::npos);
    CHECK(status.find("stopped: crashed or exited") != std::string::npos);
    for (int i = 0; i < 10; ++i) {
        CHECK(editor.update().status.empty());
        std::this_thread::sleep_for(10ms);
    }
    CHECK(editor.servers->started_clients() == 1);
    CHECK(editor.session.diagnostics().empty());
}

TEST_CASE("Env and Properties notes never reach a server, whatever language id is configured") {
    Editor editor(fake_servers("normal", {"env", "dotenv", "properties", "ini", "plaintext", "shellscript"}));
    for (const char* path : {"prod.env", "db.properties"}) {
        editor.open(path, "TOKEN=secret\n");
        for (int i = 0; i < 20; ++i) {
            editor.update();
            std::this_thread::sleep_for(10ms);
        }
        CHECK(editor.servers->started_clients() == 0);
    }
}

TEST_CASE("a diagnostics flood posts a single redraw until the next update") {
    Editor editor(fake_servers("diagnostic-flood"));
    editor.open("a.py", "x = 1\n");
    editor.update();  // belge açılır: sunucu tek seferde yüzlerce bildirim yollar
    const auto deadline = Clock::now() + kArrival;
    while (editor.redraws.load() == 0 && Clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    REQUIRE(editor.redraws.load() == 1);
    std::this_thread::sleep_for(500ms);
    CHECK(editor.redraws.load() == 1);
    // Çizim (update) bayrağı indirir; bundan sonraki bildirim yine en çok bir çizim ister.
    REQUIRE(editor.pump_until([&] { return editor.session.diagnostic_at_cursor() != nullptr; }));
    CHECK(editor.session.diagnostics().size() == 1);
}

TEST_CASE("diagnostic ranges are clamped to the current text, reversed ranges included") {
    const tui::EditorBuffer buffer("ab\ncdé", tui::terminal_cell_width);
    const auto range = [](std::uint32_t l1, std::uint32_t c1, std::uint32_t l2, std::uint32_t c2) {
        return lsp::Range{.start = {.line = l1, .character = c1}, .end = {.line = l2, .character = c2}};
    };
    const std::vector<lsp::Diagnostic> diagnostics = {
        {.range = range(9, 0, 4'000'000'000U, 99),
         .severity = lsp::DiagnosticSeverity::Warning,
         .message = "past"},
        {.range = range(0, 10, 0, 1),
         .severity = lsp::DiagnosticSeverity::Unspecified,
         .message = "reversed"},
        {.range = range(1, 1, 1, 1), .severity = lsp::DiagnosticSeverity::Hint, .message = "empty"},
        {.range = range(1, 0, 0, 0), .severity = lsp::DiagnosticSeverity::Error, .message = "backwards line"},
    };
    const auto marks = tui::diagnostic_marks(buffer, diagnostics);
    REQUIRE(marks.size() == diagnostics.size());
    for (const tui::DiagnosticMark& mark : marks) {
        CHECK(mark.range.begin <= mark.range.end);
        CHECK(mark.range.end <= buffer.text().size());
        CHECK(mark.severity != lsp::DiagnosticSeverity::Unspecified);
    }
    CHECK(marks[0].range.begin == 3);  // satır son satıra iner
    CHECK(marks[0].range.end == buffer.text().size());
    CHECK(marks[1].range.begin == 2);
    CHECK(marks[1].range.end == 2);
    CHECK(marks[1].severity == lsp::DiagnosticSeverity::Error);
    CHECK(marks[2].range.begin == 4);
    CHECK(marks[2].range.end == 5);  // sıfır genişlik bir karaktere açılır
    CHECK(marks[3].range.begin == 3);
    CHECK(marks[3].range.end == 4);
}

namespace {

// Sahte sunucunun `stem` adlı kopyası: gömülü sertleştirme tablosu sunucuyu program adından tanır.
fs::path fake_named(const fs::path& dir, std::string_view stem) {
    fs::create_directories(dir);
    const fs::path fake(NOTEAPP_FAKE_LSP_SERVER);
    fs::path copy = dir / std::string(stem);
    copy += fake.extension();  // Windows: .exe
    fs::copy_file(fake, copy, fs::copy_options::overwrite_existing);
    fs::permissions(copy, fs::perms::owner_all, fs::perm_options::replace);
    return copy;
}

// `record` kipinin dosyası: satır başına bir JSON olayı ({"event": ...}).
std::vector<core::Json> recorded(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<core::Json> events;
    for (std::string line; std::getline(in, line);) {
        constexpr core::JsonLimits kLimits{.max_depth = 64, .max_values = 100000};
        if (auto json = core::JsonParser(line, kLimits).parse())
            events.push_back(std::move(*json));
    }
    return events;
}

const core::Json* event_named(const std::vector<core::Json>& events, std::string_view name) {
    for (const core::Json& event : events)
        if (const auto found = event.fields.find("event");
            found != event.fields.end() && found->second.text == name)
            return &event;
    return nullptr;
}

// Nesne yolunu izler; bir adım yoksa null.
const core::Json* at(const core::Json* json, std::initializer_list<std::string_view> path) {
    for (const std::string_view key : path) {
        if (json == nullptr || json->kind != core::Json::Kind::Object)
            return nullptr;
        const auto found = json->fields.find(std::string(key));
        json = found == json->fields.end() ? nullptr : &found->second;
    }
    return json;
}

std::string text_at(const core::Json* json, std::initializer_list<std::string_view> path) {
    const core::Json* value = at(json, path);
    return value == nullptr || value->kind == core::Json::Kind::Object ||
                   value->kind == core::Json::Kind::Array
               ? std::string("<none>")
               : value->text;
}

// Sunucuyu `record` kipinde başlatır, notu açar; sunucu belgeyi ve yapılandırma cevabını görene dek bekler.
std::vector<core::Json> run_recorded(std::string_view language, std::string_view program,
                                     std::string_view note, std::map<std::string, core::Json> settings = {},
                                     const std::function<void(const fs::path&)>& inspect_lsp_dir = {},
                                     std::string* recorded_cwd = nullptr) {
    const test::TempDir bin;
    const fs::path out = bin.path() / "record.jsonl";
    core::LspConfig config;
    config.enabled = true;
    config.servers.emplace(std::string(language), std::vector<Argv>{{fake_named(bin.path(), program).string(),
                                                                     "record", out.string()}});
    config.settings = std::move(settings);
    Editor editor(config);
    editor.open(note, "x\n");
    std::vector<core::Json> events;
    REQUIRE(editor.pump_until([&] {
        events = recorded(out);
        return event_named(events, "didOpen") != nullptr && event_named(events, "configuration") != nullptr;
    }));
    if (recorded_cwd != nullptr)
        *recorded_cwd = text_at(event_named(events, "initialize"), {"cwd"});
    if (inspect_lsp_dir)
        inspect_lsp_dir(editor.workspace.parent_path());  // geçici dizin silinmeden
    return events;
}

core::Json json_of(std::string_view text) {
    auto json = core::JsonParser(text).parse();
    REQUIRE(json);
    return *json;
}

}  // namespace

TEST_CASE("rust-analyzer gets build scripts, proc macros, cargo check and dependency fetching off") {
    const auto events = run_recorded("rust", "rust-analyzer", "src/main.rs");
    const core::Json* init = at(event_named(events, "initialize"), {"initializationOptions"});
    CHECK(text_at(init, {"cargo", "buildScripts", "enable"}) == "false");
    CHECK(text_at(init, {"cargo", "noDeps"}) == "true");
    CHECK(text_at(init, {"procMacro", "enable"}) == "false");
    // Nesne biçimi her sürümde okunur: 2023-01 öncesi yalnız checkOnSave.enable'ı tanır (bool'u yok sayıp
    // cargo check çalıştırırdı), sonrası onu takma ad olarak önce okur.
    CHECK(text_at(init, {"checkOnSave", "enable"}) == "false");
    // workspace/configuration aynı ayarı verir: null cevap istemci ayarını varsayılana sıfırlardı.
    const core::Json* answer = at(event_named(events, "configuration"), {"answers", "rust-analyzer"});
    REQUIRE(answer != nullptr);
    CHECK(*answer == *init);
    const core::Json* env = at(event_named(events, "initialize"), {"env"});
    CHECK(text_at(env, {"CARGO_NET_OFFLINE"}) == "true");
    CHECK(text_at(env, {"RUSTUP_AUTO_INSTALL"}) == "0");
}

TEST_CASE("gopls gets no toolchain or module downloads, no cgo and no command code lenses") {
    const auto events = run_recorded("go", "gopls", "main.go");
    const core::Json* init = at(event_named(events, "initialize"), {"initializationOptions"});
    for (const char* lens :
         {"generate", "regenerate_cgo", "run_govulncheck", "tidy", "upgrade_dependency", "vendor"})
        CHECK_MESSAGE(text_at(init, {"codelenses", lens}) == "false", lens);
    CHECK(text_at(init, {"vulncheck"}) == "Off");
    CHECK(*at(event_named(events, "configuration"), {"answers", "gopls"}) == *init);
    const core::Json* env = at(event_named(events, "initialize"), {"env"});
    CHECK(text_at(env, {"GOTOOLCHAIN"}) == "local");
    CHECK(text_at(env, {"GOPROXY"}) == "off");
    CHECK(text_at(env, {"GOVCS"}) == "*:off");
    CHECK(text_at(env, {"GOFLAGS"}) == "-mod=readonly");
    CHECK(text_at(env, {"CGO_ENABLED"}) == "0");
}

TEST_CASE("jdtls gets Maven and Gradle import off and a private data directory of its own") {
    fs::path lsp_dir;
    bool data_private = false;
    const auto events = run_recorded("java", "jdtls", "App.java", {}, [&](const fs::path& dir) {
        lsp_dir = dir;
#ifdef _WIN32
        data_private = fs::is_directory(dir / "jdtls-data");
#else
        data_private = fs::status(dir / "jdtls-data").permissions() == fs::perms::owner_all;
#endif
    });
    CHECK(data_private);
    const core::Json* java =
        at(event_named(events, "initialize"), {"initializationOptions", "settings", "java"});
    CHECK(text_at(java, {"import", "maven", "enabled"}) == "false");
    CHECK(text_at(java, {"import", "gradle", "enabled"}) == "false");
    CHECK(text_at(java, {"import", "gradle", "wrapper", "enabled"}) == "false");
    CHECK(text_at(java, {"maven", "downloadSources"}) == "false");
    CHECK(text_at(java, {"eclipse", "downloadSources"}) == "false");
    CHECK(text_at(java, {"configuration", "updateBuildConfiguration"}) == "disabled");
    CHECK(*at(event_named(events, "didChangeConfiguration"), {"settings", "java"}) == *java);
    const core::Json* argv = at(event_named(events, "initialize"), {"argv"});
    REQUIRE(argv != nullptr);
    REQUIRE(argv->items.size() == 5);  // program record <dosya> -data <dizin>
    CHECK(argv->items[3].text == "-data");
    CHECK(fs::path(argv->items[4].text) == lsp_dir / "jdtls-data");
}

TEST_CASE("typescript-language-server gets automatic type acquisition off") {
    for (const auto& [language, note] : {std::pair{"typescript", "a.ts"}, std::pair{"javascript", "a.js"}}) {
        CAPTURE(language);
        const auto events = run_recorded(language, "typescript-language-server", note);
        CHECK(text_at(event_named(events, "initialize"),
                      {"initializationOptions", "disableAutomaticTypingAcquisition"}) == "true");
    }
}

TEST_CASE("yaml-language-server gets schema stores off and remote schemas routed to the client") {
    const auto events = run_recorded("yaml", "yaml-language-server", "a.yaml");
    const core::Json* yaml = at(event_named(events, "configuration"), {"answers", "yaml"});
    CHECK(text_at(yaml, {"schemaStore", "enable"}) == "false");
    CHECK(text_at(yaml, {"kubernetesCRDStore", "enable"}) == "false");
    REQUIRE(at(yaml, {"schemas"}) != nullptr);
    CHECK(at(yaml, {"schemas"})->fields.empty());
    // http(s) şemaları vscode/content ile istemciye sorulur; istemci -32601 döner, sunucu ağa çıkmaz.
    CHECK(event_named(events, "yaml/registerContentRequest") != nullptr);
}

TEST_CASE("vscode-json-language-server handles only file schemas and keeps validation on") {
    const auto events = run_recorded("json", "vscode-json-language-server", "a.json");
    const core::Json* init = at(event_named(events, "initialize"), {"initializationOptions"});
    REQUIRE(at(init, {"handledSchemaProtocols"}) != nullptr);
    CHECK(*at(init, {"handledSchemaProtocols"}) == json_of(R"(["file"])"));
    CHECK(text_at(event_named(events, "didChangeConfiguration"),
                  {"settings", "json", "validate", "enable"}) == "true");
}

TEST_CASE("taplo gets schemas, catalogs and config file discovery off") {
    const auto events = run_recorded("toml", "taplo", "a.toml");
    const core::Json* section = at(event_named(events, "configuration"), {"answers", "evenBetterToml"});
    CHECK(text_at(section, {"schema", "enabled"}) == "false");
    REQUIRE(at(section, {"schema", "catalogs"}) != nullptr);
    CHECK(at(section, {"schema", "catalogs"})->items.empty());
    CHECK(text_at(section, {"taplo", "configFile", "enabled"}) == "false");
    // taplo didChangeConfiguration'da bölümün kendisini bekler (handlers/configuration.rs).
    CHECK(*at(event_named(events, "didChangeConfiguration"), {"settings"}) == *section);
}

TEST_CASE("a user [lsp.settings.<language>] table merges over the hardening, key by key") {
    std::map<std::string, core::Json> settings;
    settings.emplace("rust",
                     json_of(R"({"rust-analyzer":{"cargo":{"buildScripts":{"enable":true}}},"extra":1})"));
    const auto events = run_recorded("rust", "rust-analyzer", "src/main.rs", std::move(settings));
    const core::Json* init = at(event_named(events, "initialize"), {"initializationOptions"});
    CHECK(text_at(init, {"cargo", "buildScripts", "enable"}) == "true");  // kullanıcı açtı
    CHECK(text_at(init, {"cargo", "noDeps"}) == "true");                  // kardeş anahtar kaldı
    CHECK(text_at(init, {"procMacro", "enable"}) == "false");
    CHECK(text_at(event_named(events, "didChangeConfiguration"), {"settings", "extra"}) == "1");
}

TEST_CASE("an unknown server gets no settings and null configuration answers") {
    const auto events = run_recorded("python", "some-other-server", "a.py");
    CHECK(at(event_named(events, "initialize"), {"initializationOptions"}) == nullptr);
    CHECK(event_named(events, "didChangeConfiguration") == nullptr);
    const core::Json* answer = at(event_named(events, "configuration"), {"answers", "rust-analyzer"});
    REQUIRE(answer != nullptr);
    CHECK(answer->kind == core::Json::Kind::Null);
}

TEST_CASE("every default server that could run project code or fetch from the network is hardened") {
    // T59c: proje kodu çalıştırmayan, varsayılan ayarda ağa çıkmayan sunucular.
    const std::set<std::string> safe = {"clangd",
                                        "bash-language-server",
                                        "marksman",
                                        "lua-language-server",
                                        "vscode-html-language-server",
                                        "vscode-css-language-server",
                                        "docker-langserver",
                                        "pyright-langserver",
                                        "sql-language-server",
                                        "htmx-lsp"};
    for (const auto& [language, commands] : core::parse_config("").config.lsp.servers)
        for (const Argv& command : commands) {
            const std::string program(tui::server_program(command.front()));
            CHECK_MESSAGE((safe.contains(program) || tui::has_hardening(program)), program);
        }
    // Gömülü tabloda olmayan riskli bir sunucu (ör. Gradle'ı çalıştıran kotlin-language-server) varsayılanda
    // yoktur.
    CHECK_FALSE(tui::has_hardening("kotlin-language-server"));
    CHECK_FALSE(core::parse_config("").config.lsp.servers.contains("kotlin"));
}

TEST_CASE("two servers for html both get the document; diagnostics and completions merge, tagged") {
    const test::TempDir bin;
    core::LspConfig config;
    config.enabled = true;
    config.servers.emplace(
        "html", std::vector<Argv>{{fake_named(bin.path(), "html-a").string(), "normal", "from-a"},
                                  {fake_named(bin.path(), "html-b").string(), "normal", "from-b"}});
    Editor editor(config);
    editor.open("page.html", "<p>\n");
    REQUIRE(editor.pump_until([&] { return editor.session.diagnostics().size() == 2; }));
    CHECK(editor.servers->started_clients() == 2);
    std::vector<std::string> messages;
    for (const tui::DiagnosticMark& mark : editor.session.diagnostics())
        messages.push_back(mark.message);
    std::ranges::sort(messages);
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].starts_with("html-a: count=1 text=<p>"));
    CHECK(messages[1].starts_with("html-b: count=1 text=<p>"));

    editor.session.handle(Key::End);
    editor.session.handle(Key::CtrlSpace);
    // İkinci cevap ilkinin önerilerini silmemeli.
    REQUIRE(editor.pump_until(
        [&] { return has_item(editor.session, "from-a") && has_item(editor.session, "from-b"); }));
    CHECK(has_item(editor.session, "alpha"));  // iki sunucudan gelen aynı etiket bir kez
    CHECK(std::ranges::count(editor.session.completion_items(), std::string("alpha")) == 1);
}

TEST_CASE("one of two html servers crashing is reported for that server; the other keeps working") {
    const test::TempDir bin;
    core::LspConfig config;
    config.enabled = true;
    config.servers.emplace("html", std::vector<Argv>{{fake_named(bin.path(), "html-a").string(), "normal"},
                                                     {fake_named(bin.path(), "html-b").string(), "crash"}});
    Editor editor(config);
    editor.open("page.html", "<p>\n");
    std::string status;
    const auto deadline = Clock::now() + kArrival;
    while ((status.empty() || editor.session.diagnostics().empty()) && Clock::now() < deadline) {
        if (std::string next = editor.update().status; !next.empty())
            status = std::move(next);
        std::this_thread::sleep_for(10ms);
    }
    CHECK(status.find("html-b") != std::string::npos);
    CHECK(status.find("stopped") != std::string::npos);
    editor.session.handle(Key::Char, "x");
    REQUIRE(editor.pump_until([&] {
        const auto marks = editor.session.diagnostics();
        return marks.size() == 1 && marks.front().message.find("count=2") != std::string::npos;
    }));
    CHECK(editor.session.diagnostics().front().message.starts_with("html-a: "));
}

TEST_CASE("a missing second server is reported once while the first one runs") {
    const test::TempDir bin;
    core::LspConfig config;
    config.enabled = true;
    config.servers.emplace("html", std::vector<Argv>{{fake_named(bin.path(), "html-a").string(), "normal"},
                                                     {"noteapp-no-such-server"}});
    Editor editor(config);
    editor.open("page.html", "<p>\n");
    std::vector<std::string> statuses;
    const auto deadline = Clock::now() + kArrival;
    while (editor.session.diagnostics().empty() && Clock::now() < deadline) {
        if (std::string status = editor.update().status; !status.empty())
            statuses.push_back(std::move(status));
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE_FALSE(editor.session.diagnostics().empty());
    CHECK(statuses == std::vector<std::string>{"noteapp-no-such-server not found, html runs without it"});
    // Tek sunucu kalınca tanı etiketsizdir: hangi sunucudan geldiği belli.
    CHECK(editor.session.diagnostics().front().message.starts_with("count=1"));
}

// Sunucu bizim çalışma dizinimizi devralırsa göreli yol aramaları (taplo yapılandırması, jdtls.py, Windows'ta
// exe arama) kullanıcının o an bulunduğu klasöre, ör. klonlanmış bir depoya bakar.
TEST_CASE("a language server runs in the lsp state directory, not in our working directory") {
    bool in_lsp_dir = false;
    std::string recorded_cwd;
    const auto events = run_recorded(
        "rust", "rust-analyzer", "src/main.rs", {},
        [&](const fs::path& lsp_dir) {
            std::error_code ec;
            in_lsp_dir = fs::equivalent(recorded_cwd, lsp_dir, ec);
        },
        &recorded_cwd);
    CAPTURE(recorded_cwd);
    CHECK(in_lsp_dir);
#ifdef _WIN32
    CHECK(text_at(at(event_named(events, "initialize"), {"env"}), {"NoDefaultCurrentDirectoryInExePath"}) ==
          "1");
#endif
}

// Yeniden açılan notun tamponu yenidir, edit_count yine 0'dan başlar: önceki temiz taramanın (yol, 0) sonucu
// senkronla değişmiş metne uygulanırsa uzak başvuru didOpen'la giderdi.
TEST_CASE("a note changed on disk while closed is scanned again when it is reopened") {
    Editor editor(fake_servers("normal", {"json"}), true);
    editor.open("a.json", R"({"a": 1})");
    REQUIRE(editor.pump_until([&] { return !editor.session.diagnostics().empty(); }));
    editor.session.handle(Key::CtrlQ);  // temiz not: editör kapanır
    REQUIRE_FALSE(editor.session.is_open());
    editor.update();

    test::upsert_note(editor.dir.path() / "data", "a.json", R"({"$schema": "file://server/share/s.json"})");
    editor.session.open(*core::NotePath::parse("a.json"));
    REQUIRE(editor.session.is_open());
    REQUIRE(editor.session.buffer()->text().find("file://server") != std::string::npos);
    std::vector<std::string> statuses;
    for (int i = 0; i < 50; ++i) {
        if (std::string status = editor.update().status; !status.empty())
            statuses.push_back(std::move(status));
        std::this_thread::sleep_for(10ms);
    }
    CHECK(statuses == std::vector<std::string>{std::string(kNetworkPathStatus)});
    CHECK(editor.session.diagnostics().empty());  // didOpen gitseydi sahte sunucu tanı yayınlardı
}

TEST_CASE("with network paths blocked a note referencing one reaches no server; a local one does") {
    Editor editor(fake_servers("normal", {"json"}), true);
    std::vector<std::string> statuses;
    const auto collect = [&] {
        for (int i = 0; i < 30; ++i) {
            if (std::string status = editor.update().status; !status.empty())
                statuses.push_back(std::move(status));
            std::this_thread::sleep_for(10ms);
        }
    };
    editor.open("remote.json", R"({"$schema": "//server/share/s.json"})");
    collect();
    CHECK(statuses == std::vector<std::string>{std::string(kNetworkPathStatus)});
    CHECK(editor.servers->started_clients() == 0);
    CHECK(editor.session.diagnostics().empty());

    editor.open("local.json", R"({"$schema": "file:///abs/s.json"})");
    REQUIRE(editor.pump_until([&] { return !editor.session.diagnostics().empty(); }));

    // Açık nota uzak başvuru yazılınca belge kapanır, tanılar gider.
    statuses.clear();
    editor.session.handle(Key::End);
    for (const char c : std::string_view(R"( "file://evil/x")"))
        editor.session.handle(Key::Char, std::string(1, c));
    const auto deadline = Clock::now() + kArrival;
    while (statuses.empty() && Clock::now() < deadline) {
        if (std::string status = editor.update().status; !status.empty())
            statuses.push_back(std::move(status));
        std::this_thread::sleep_for(10ms);
    }
    CHECK(statuses == std::vector<std::string>{std::string(kNetworkPathStatus)});
    CHECK(editor.session.diagnostics().empty());
}

// Linux'un kuralı: //host yerel yoldur, not sunucuya gider.
TEST_CASE("with network paths not blocked a //host reference still reaches the server") {
    Editor editor(fake_servers("normal", {"json"}), false);
    editor.open("remote.json", R"({"$schema": "//server/share/s.json"})");
    CHECK(editor.pump_until([&] { return !editor.session.diagnostics().empty(); }));
}

// Yükseltmede eski yansıtmanın artık engellenen dosyaları (rust-analyzer.toml) diskte durur; sunucu ilk
// eşitleme onları silmeden başlarsa okuyabilir.
TEST_CASE("no server starts before the first mirror sync has removed stale files") {
    const test::TempDir bin;
    const fs::path out = bin.path() / "record.jsonl";
    Editor editor(core::LspConfig{.enabled = false, .servers = {}, .settings = {}});
    std::vector<core::ImportedNote> notes;
    constexpr std::size_t kNotes = 2000;  // ilk eşitleme saniyeler sürsün: sunucu o arada başlamamalı
    for (std::size_t i = 0; i < kNotes; ++i)
        notes.push_back(
            {*core::NotePath::parse("pkg" + std::to_string(i % 20) + "/m" + std::to_string(i) + ".rs"),
             "fn f" + std::to_string(i) + "() {}\n", std::chrono::system_clock::now()});
    test::bulk_seed(editor.dir.path() / "data", notes);
    fs::create_directories(editor.workspace);
    std::ofstream(editor.workspace / "rust-analyzer.toml") << "[check]\noverrideCommand = [\"evil\"]\n";

    core::LspConfig config;
    config.enabled = true;
    config.servers.emplace("rust", std::vector<Argv>{{fake_named(bin.path(), "rust-analyzer").string(),
                                                      "record", out.string()}});
    editor.servers.reset();
    editor.servers.emplace(
        config, editor.workspace, editor.store, [&editor] { ++editor.redraws; }, 50ms, 0ms);
    editor.open("main.rs", "fn main() {}\n");
    std::vector<core::Json> events;
    REQUIRE(editor.pump_until(
        [&] {
            events = recorded(out);
            return event_named(events, "initialize") != nullptr;
        },
        60s));
    const core::Json* entries = at(event_named(events, "initialize"), {"workspace_entries"});
    REQUIRE(entries != nullptr);
    REQUIRE_FALSE(entries->items.empty());  // kök sunucunun çalışma dizininin altında görüldü
    for (const core::Json& entry : entries->items)
        CHECK(entry.text != "rust-analyzer.toml");
}
