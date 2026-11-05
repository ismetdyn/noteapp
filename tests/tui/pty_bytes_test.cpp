// Gerçek `notes` sahte terminalde: tuş başına yazılan bayt üst sınırı (T75). Süre ölçülmez, yalnız bayt
// sayılır; bir sonraki tuş önceki karenin bitişini (senkron güncelleme kapanışı) bekler, sonuç makine
// hızından bağımsızdır.
#include "core/migration.hpp"
#include "support/note_seed.hpp"
#include "support/run_process.hpp"
#include "support/test_support.hpp"

#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <poll.h>
#include <pty.h>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kColumns = 160;
constexpr int kRows = 40;
constexpr std::size_t kMaxBytesPerKey = 400;  // Yazi 167; FTXUI'nin tam karesi 3318 idi
constexpr std::string_view kFrameEnd = "\x1B[?2026l";
constexpr int kReadTimeoutMs = 10000;  // yalnız asılmaya karşı; başarı bunu beklemez

class PtyApp {
public:
    explicit PtyApp(const std::string& program, std::vector<std::string> args = {}) {
        args.insert(args.begin(), program);
        std::vector<char*> argv;
        for (std::string& arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        winsize size{};
        size.ws_row = kRows;
        size.ws_col = kColumns;
        pid_ = forkpty(&fd_, nullptr, nullptr, &size);
        REQUIRE(pid_ >= 0);
        if (pid_ == 0) {
            setenv("TERM", "xterm-256color", 1);
            execv(program.c_str(), argv.data());
            _exit(127);
        }
    }

    ~PtyApp() {
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
        close(fd_);
    }

    PtyApp(const PtyApp&) = delete;
    PtyApp& operator=(const PtyApp&) = delete;

    void send(std::string_view keys) const {
        REQUIRE(write(fd_, keys.data(), keys.size()) == static_cast<ssize_t>(keys.size()));
    }

    // Bir kare bitene dek okur, okunan bayt sayısını döner; terminal sorularını gerçek terminal gibi
    // yanıtlar.
    std::size_t read_frame() const { return read_frame_text().size(); }

    std::string read_frame_text() const {
        std::string seen;
        while (seen.find(kFrameEnd) == std::string::npos) {
            pollfd ready{fd_, POLLIN, 0};
            REQUIRE(poll(&ready, 1, kReadTimeoutMs) > 0);
            std::array<char, 1 << 16> buffer{};
            const ssize_t count = read(fd_, buffer.data(), buffer.size());
            REQUIRE(count > 0);
            const std::string_view chunk(buffer.data(), static_cast<std::size_t>(count));
            answer_queries(chunk);
            seen += chunk;
        }
        return seen;
    }

    // `duration` boyunca okur (terminal sorularını yanıtlar), okunan bayt sayısını döner.
    std::size_t read_for(std::chrono::milliseconds duration) const {
        const auto until = std::chrono::steady_clock::now() + duration;
        std::size_t total = 0;
        for (auto now = std::chrono::steady_clock::now(); now < until;
             now = std::chrono::steady_clock::now()) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - now);
            pollfd ready{fd_, POLLIN, 0};
            if (poll(&ready, 1, static_cast<int>(left.count()) + 1) <= 0)
                continue;
            std::array<char, 1 << 16> buffer{};
            const ssize_t count = read(fd_, buffer.data(), buffer.size());
            REQUIRE(count > 0);
            const std::string_view chunk(buffer.data(), static_cast<std::size_t>(count));
            answer_queries(chunk);
            total += chunk.size();
        }
        return total;
    }

    void signal(int number) const { REQUIRE(kill(pid_, number) == 0); }

    // Süreç bitene dek (terminal kapanınca read EIO döner) kalan tüm çıktı.
    std::string read_until_exit() const {
        std::string seen;
        while (true) {
            pollfd ready{fd_, POLLIN, 0};
            if (poll(&ready, 1, kReadTimeoutMs) <= 0)
                break;
            std::array<char, 1 << 16> buffer{};
            const ssize_t count = read(fd_, buffer.data(), buffer.size());
            if (count <= 0)
                break;
            seen.append(buffer.data(), static_cast<std::size_t>(count));
        }
        return seen;
    }

private:
    // Terminal sorularını gerçek terminal gibi yanıtlar (imleç konumu, aygıt nitelikleri).
    void answer_queries(std::string_view chunk) const {
        if (chunk.find("\x1B[6n") != std::string_view::npos)
            send("\x1B[1;1R");
        if (chunk.find("\x1B[c") != std::string_view::npos)
            send("\x1B[?62;c");
    }

    pid_t pid_ = -1;
    int fd_ = -1;
};

}  // namespace

TEST_CASE("moving through the list writes under the byte bound per key at 160x40") {
    noteapp::test::TempDir home;
    noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    constexpr int kNotes = 44;
    {
        noteapp::core::NoteStore store(home.path() / "data");
        for (int i = 0; i < kNotes; ++i)
            REQUIRE(noteapp::test::seed_note(store, "n" + std::to_string(i) + ".md", "x\n"));
    }

    PtyApp app(NOTEAPP_NOTES_EXE);
    CHECK(app.read_frame() > 0);
    constexpr int kSteps = 20;
    std::size_t total = 0;
    for (const char key : {'j', 'k'}) {
        for (int step = 0; step < kSteps; ++step) {
            app.send(std::string_view(&key, 1));
            total += app.read_frame();
        }
    }
    const std::size_t per_key = total / (2 * kSteps);
    MESSAGE("bytes per key: ", per_key);
    CHECK(per_key < kMaxBytesPerKey);

    // Ctrl+L aynı ekranı baştan çizer: yine kare biter, süreç ayaktadır.
    app.send("\x0C");
    CHECK(app.read_frame() > kMaxBytesPerKey);
}

TEST_CASE("a file dropped after migration shows one notice at open, gone after the first key") {
    noteapp::test::TempDir home;
    noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    const std::filesystem::path notes = home.path() / "notes";
    std::filesystem::create_directories(notes);
    std::ofstream(notes / "plan.md") << "plan\n";
    REQUIRE(noteapp::core::migrate_legacy_notes(notes, home.path() / "data").has_value());
    std::ofstream(notes / "denee.txt") << "yeni\n";

    constexpr std::string_view kNotice = "note graph status";
    constexpr int kMaxOpeningFrames = 20;
    PtyApp app(NOTEAPP_NOTES_EXE);
    bool shown = false;
    for (int frame = 0; frame < kMaxOpeningFrames && !shown; ++frame)  // işçi sonucu sonraki karede gelir
        shown = app.read_frame_text().find(kNotice) != std::string::npos;
    REQUIRE(shown);

    app.send("j");
    CHECK(app.read_frame_text().find(kNotice) == std::string::npos);
    app.send("\x0C");  // tam kare: ekranın tamamı
    CHECK(app.read_frame_text().find(kNotice) == std::string::npos);
}

// FTXUI terminali bıraktıktan sonra stderr'e giden çıkış notu da güvenilmez yolu taşır: sağdan sola geçersiz
// kılma ve UTF-8 C1 CSI ham gitmemeli.
TEST_CASE("the exit note after the full screen never sends a raw bidi or C1 character from the path") {
    noteapp::test::TempDir home;
    noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    const std::string rlo = "\xE2\x80\xAE";
    const std::string csi = "\xC2\x9B";
    {
        noteapp::core::NoteStore store(home.path() / "data");
        REQUIRE(noteapp::test::seed_note(store, "gpj" + rlo + "x" + csi + "31m.md", "metin\n"));
    }
    PtyApp app(NOTEAPP_NOTES_EXE);
    CHECK(app.read_frame() > 0);
    app.send("\r");  // Enter: editör
    app.read_frame();
    app.send("y");  // kirli metin: çıkışta kaydedilir ve not yazılır
    app.read_frame();
    app.signal(SIGTERM);
    const std::string output = app.read_until_exit();
    constexpr std::string_view kLeaveAltScreen = "\x1B[?1049l";
    const std::size_t left = output.rfind(kLeaveAltScreen);
    REQUIRE(left != std::string::npos);
    const std::string after = output.substr(left + kLeaveAltScreen.size());
    CAPTURE(after);
    CHECK(after.find("gpj") != std::string::npos);  // not gerçekten yazıldı
    CHECK(after.find(rlo) == std::string::npos);
    CHECK(after.find(csi) == std::string::npos);
}

namespace {

// `timeout`: argümanı tanımayan eski ikili TUI açıp beklerdi; test asılmasın.
noteapp::test::RunResult run_notes(const std::vector<std::string>& args) {
    std::vector<std::string> command{"20", NOTEAPP_NOTES_EXE};
    command.insert(command.end(), args.begin(), args.end());
    return noteapp::test::run_process("/usr/bin/timeout", command);
}

void english_config(const std::filesystem::path& home) {
    std::filesystem::create_directories(home / "config");
    std::ofstream(home / "config" / "config.toml") << "language = \"en\"\n";
}

}  // namespace

TEST_CASE("notes --help prints the short usage and exits 0; an unknown argument exits 2") {
    noteapp::test::TempDir home;
    noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    english_config(home.path());
    const auto help = run_notes({"--help"});
    CHECK(help.exit_code == 0);
    CHECK(help.out.find("notes import") != std::string::npos);
    const auto bad = run_notes({"--bogus"});
    CHECK(bad.exit_code == 2);
    CHECK(bad.err.find("--bogus") != std::string::npos);
}

TEST_CASE("notes import prints the summary, then opens the TUI on the imported note") {
    noteapp::test::TempDir home;
    noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    english_config(home.path());
    const noteapp::test::TempDir files;
    const std::filesystem::path file = files.path() / "rapor.txt";
    std::ofstream(file, std::ios::binary) << "kuzey yildizi\r\n";

    const PtyApp app(NOTEAPP_NOTES_EXE, {"import", noteapp::test::utf8(file), "--folder", "inbox"});
    constexpr int kMaxOpeningFrames = 20;
    std::string seen;
    for (int frame = 0; frame < kMaxOpeningFrames && seen.find("yildizi") == std::string::npos; ++frame)
        seen += app.read_frame_text();
    CHECK(seen.find("Imported: 1") != std::string::npos);
    // Kare boşlukları imleç hareketiyle yazar: tek sözcük aranır. Önizleme notu gösteriyor.
    CHECK(seen.find("yildizi") != std::string::npos);
    CHECK(noteapp::test::note_body(home.path() / "data", "inbox/rapor.txt") == "kuzey yildizi\r\n");
}

TEST_CASE("notes import with nothing imported exits 1 without opening the TUI") {
    noteapp::test::TempDir home;
    noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    english_config(home.path());
    const auto missing = run_notes({"import", noteapp::test::utf8(home.path() / "yok.md")});
    CHECK(missing.exit_code == 1);
    CHECK(missing.err.find("yok.md") != std::string::npos);
    CHECK(run_notes({"import"}).exit_code == 2);
    CHECK(run_notes({"import", "a", "--folder", "../dis"}).exit_code == 2);
}

namespace {

// Çizim sayacı: `notes` NOTEAPP_TUI_DRAW_LOG dosyasına her çizimde bir satır yazar (boş fark da sayılır).
std::size_t draws(const std::filesystem::path& log) {
    std::ifstream in(log);
    std::size_t lines = 0;
    for (std::string line; std::getline(in, line);)
        ++lines;
    return lines;
}

// Çıktı `quiet` boyunca susana dek okur (en çok `limit`): açılıştaki işçi sonuçlarının kareleri biter.
void wait_quiet(const PtyApp& app, std::chrono::milliseconds quiet, std::chrono::milliseconds limit) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until && app.read_for(quiet) > 0) {
    }
}

struct DrawCountFixture {
    noteapp::test::TempDir home;
    noteapp::test::ScopedEnv home_env{"NOTEAPP_HOME", noteapp::test::utf8(home.path())};
    std::filesystem::path log = home.path() / "draws.log";
    noteapp::test::ScopedEnv log_env{"NOTEAPP_TUI_DRAW_LOG", noteapp::test::utf8(log)};

    // Dil sunucusu ve yazım denetimi kapalı (.py, lsp.enabled = false): işçi sonucu kare saymayı bozmasın.
    explicit DrawCountFixture(int autosave_ms = kDefaultAutosaveMs) {
        std::filesystem::create_directories(home.path() / "config");
        std::ofstream(home.path() / "config" / "config.toml")
            << "language = \"en\"\n[lsp]\nenabled = false\n[tuning]\nautosave_ms = " << autosave_ms << '\n';
        add_note("a.py", "x = 1\n");
    }

    void add_note(std::string_view path, std::string_view text) const {
        noteapp::core::NoteStore store(home.path() / "data");
        REQUIRE(noteapp::test::seed_note(store, path, text));
    }

    static constexpr int kDefaultAutosaveMs = 3000;
};

constexpr auto kQuiet = std::chrono::milliseconds(300);
constexpr auto kSettleLimit = std::chrono::milliseconds(5000);

}  // namespace

TEST_CASE("an idle TUI draws nothing and writes no bytes") {
    const DrawCountFixture f;
    const PtyApp app(NOTEAPP_NOTES_EXE);
    CHECK(app.read_frame() > 0);
    wait_quiet(app, kQuiet, kSettleLimit);
    const std::size_t before = draws(f.log);
    CHECK(before > 0);
    CHECK(app.read_for(std::chrono::milliseconds(1500)) == 0);
    CHECK(draws(f.log) == before);
}

TEST_CASE("one key in the list draws exactly one frame") {
    const DrawCountFixture f;
    f.add_note("b.py", "y = 2\n");
    const PtyApp app(NOTEAPP_NOTES_EXE);
    CHECK(app.read_frame() > 0);
    wait_quiet(app, kQuiet, kSettleLimit);
    const std::size_t before = draws(f.log);
    app.send("j");
    CHECK(app.read_frame() > 0);
    wait_quiet(app, kQuiet, kSettleLimit);
    CHECK(draws(f.log) == before + 1);
}

TEST_CASE("a pending autosave draws nothing until its deadline, then saves with one frame") {
    constexpr int kAutosaveMs = 1500;
    const DrawCountFixture f(kAutosaveMs);
    const PtyApp app(NOTEAPP_NOTES_EXE);
    CHECK(app.read_frame() > 0);
    wait_quiet(app, kQuiet, kSettleLimit);
    app.send("\r");  // Enter: editör
    app.read_frame();
    wait_quiet(app, kQuiet, kSettleLimit);
    const auto typed = std::chrono::steady_clock::now();
    app.send("z");  // kirli metin: süre dolunca otomatik kayıt
    app.read_frame();
    const std::size_t after_key = draws(f.log);
    const std::filesystem::path data = f.home.path() / "data";
    // Süre dolmadan: ne kayıt ne çizim (eskiden her 15 ms'de bir boş kare kurulurdu).
    app.read_for(std::chrono::milliseconds(kAutosaveMs / 2));
    CHECK(draws(f.log) == after_key);
    CHECK(noteapp::test::note_body(data, "a.py") == "x = 1\n");
    // Süre dolunca: kayıt ve onu gösteren tek çizim.
    const auto until = typed + std::chrono::milliseconds(kAutosaveMs) + kSettleLimit;
    while (noteapp::test::note_body(data, "a.py") != "zx = 1\n" && std::chrono::steady_clock::now() < until)
        app.read_for(std::chrono::milliseconds(50));
    CHECK(noteapp::test::note_body(data, "a.py") == "zx = 1\n");
    CHECK(std::chrono::steady_clock::now() - typed >= std::chrono::milliseconds(kAutosaveMs));
    wait_quiet(app, kQuiet, kSettleLimit);
    CHECK(draws(f.log) == after_key + 1);
}

namespace {

// Çıktıdaki SGR parametreleri. Renk: 30–37, 40–47, 90–97, 100–107 ve 38/48 (256 ve truecolor); 39/49
// renk değil, varsayılana dönüştür.
struct SgrSeen {
    bool color = false;
    bool bold = false;
    bool dim = false;
    bool inverse = false;
};

SgrSeen scan_sgr(std::string_view bytes) {
    SgrSeen seen;
    for (std::size_t at = bytes.find("\x1B["); at != std::string_view::npos;
         at = bytes.find("\x1B[", at + 2)) {
        const std::size_t end = bytes.find_first_not_of("0123456789;", at + 2);
        if (end == std::string_view::npos || bytes[end] != 'm')
            continue;
        std::string_view params = bytes.substr(at + 2, end - at - 2);
        while (!params.empty()) {
            const std::size_t cut = params.find(';');
            const std::string_view param = params.substr(0, cut);
            const int code = param.empty() ? 0 : std::stoi(std::string(param));
            seen.color |= (code >= 30 && code <= 37) || (code >= 40 && code <= 47) ||
                          (code >= 90 && code <= 97) || (code >= 100 && code <= 107) || code == 38 ||
                          code == 48;
            seen.bold |= code == 1;
            seen.dim |= code == 2;
            seen.inverse |= code == 7;
            params = cut == std::string_view::npos ? std::string_view{} : params.substr(cut + 1);
        }
    }
    return seen;
}

}  // namespace

// no-color.org: boş olmayan NO_COLOR rengi kapatır (FTXUI Palette1); odak ve seçim kalın, soluk ve ters ile
// okunur kalır. Boş NO_COLOR rengi kapatmaz.
TEST_CASE("NO_COLOR draws no color but keeps bold, dim and inverse; without it the TUI is colored") {
    for (const bool no_color : {false, true}) {
        CAPTURE(no_color);
        noteapp::test::TempDir home;
        noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
        noteapp::test::ScopedEnv color("NO_COLOR", no_color ? std::string("1") : std::string());
        {
            noteapp::core::NoteStore store(home.path() / "data");
            REQUIRE(noteapp::test::seed_note(store, "a.java", "class A { // yorum\n}\n"));
        }
        PtyApp app(NOTEAPP_NOTES_EXE);
        std::string shown = app.read_frame_text();
        app.send(":");  // palet: modal çerçevesi ve soluk ipucu satırı
        shown += app.read_frame_text();
        const SgrSeen seen = scan_sgr(shown);
        CHECK(seen.color == !no_color);
        CHECK(seen.bold);
        CHECK(seen.dim);
        CHECK(seen.inverse);
    }
}
