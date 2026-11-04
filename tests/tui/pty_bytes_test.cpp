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
            if (chunk.find("\x1B[6n") != std::string_view::npos)
                send("\x1B[1;1R");
            if (chunk.find("\x1B[c") != std::string_view::npos)
                send("\x1B[?62;c");
            seen += chunk;
        }
        return seen;
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
