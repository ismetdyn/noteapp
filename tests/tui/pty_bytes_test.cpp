// Gerçek `notes` sahte terminalde: tuş başına yazılan bayt üst sınırı (T75). Süre ölçülmez, yalnız bayt
// sayılır; bir sonraki tuş önceki karenin bitişini (senkron güncelleme kapanışı) bekler, sonuç makine
// hızından bağımsızdır.
#include "core/migration.hpp"
#include "support/note_seed.hpp"
#include "support/run_process.hpp"
#include "support/test_support.hpp"

#include <algorithm>
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
#include <utility>
#include <vector>

namespace {

constexpr int kColumns = 160;
constexpr int kRows = 40;
constexpr std::size_t kMaxBytesPerKey = 400;  // Yazi 167; FTXUI'nin tam karesi 3318 idi
constexpr std::string_view kFrameEnd = "\x1B[?2026l";
constexpr int kReadTimeoutMs = 10000;  // yalnız asılmaya karşı; başarı bunu beklemez

class PtyApp {
public:
    explicit PtyApp(const std::string& program, std::vector<std::string> args = {}, int columns = kColumns,
                    int rows = kRows) {
        args.insert(args.begin(), program);
        std::vector<char*> argv;
        for (std::string& arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        winsize size{};
        size.ws_row = static_cast<unsigned short>(rows);
        size.ws_col = static_cast<unsigned short>(columns);
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

    // `duration` boyunca okur (terminal sorularını yanıtlar), okunan bayt sayısını döner; `seen` doluysa
    // okunanı ona ekler.
    std::size_t read_for(std::chrono::milliseconds duration, std::string* seen = nullptr) const {
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
            if (seen)
                seen->append(chunk);
        }
        return total;
    }

    void signal(int number) const { REQUIRE(kill(pid_, number) == 0); }

    // Sürecin şimdiye dek en yüksek yerleşik belleği (VmHWM, KiB); okunamazsa 0.
    std::size_t peak_rss_kib() const {
        std::ifstream status("/proc/" + std::to_string(pid_) + "/status");
        for (std::string line; std::getline(status, line);)
            if (line.starts_with("VmHWM:"))
                return static_cast<std::size_t>(std::stoull(line.substr(line.find_first_of("0123456789"))));
        return 0;
    }

    // Süreç kendiliğinden bitti mi (read_until_exit zaman aşımında da döner). Yıkıcının waitpid'i sonra boşa döner.
    bool exited() const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kReadTimeoutMs);
        while (waitpid(pid_, nullptr, WNOHANG) != pid_) {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            poll(nullptr, 0, 10);
        }
        return true;
    }

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
    // `limits_tui`: config'in [limits.tui] bölümüne eklenen satırlar.
    explicit DrawCountFixture(int autosave_ms = kDefaultAutosaveMs, std::string_view limits_tui = {}) {
        std::filesystem::create_directories(home.path() / "config");
        std::ofstream(home.path() / "config" / "config.toml")
            << "language = \"en\"\n[lsp]\nenabled = false\n[tuning]\nautosave_ms = " << autosave_ms
            << "\n[limits.tui]\n"
            << limits_tui << '\n';
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

namespace {

// Çıktının yorumlandığı ızgara: frame_diff'in kullandığı alt küme (CUP, CHA, CUF, \r\n, SGR). Genişliği kesin
// olmayan glifin ardından frame_diff mutlak konum yazar; bu yüzden her kod noktası bir hücre sayılır.
struct GridCell {
    std::string text = " ";
    bool bold = false;
    bool inverse = false;
    int foreground = 39;
    int background = 49;
};

class Grid {
public:
    Grid(int columns, int rows)
        : columns_(columns),
          cells_(static_cast<std::size_t>(rows), std::vector<GridCell>(static_cast<std::size_t>(columns))) {}

    void feed(std::string_view bytes) {
        for (std::size_t at = 0; at < bytes.size();) {
            if (bytes.substr(at, 2) == "\x1B[") {
                const std::size_t end = bytes.find_first_not_of("0123456789;? ", at + 2);
                if (end == std::string_view::npos)
                    return;
                control(bytes.substr(at + 2, end - at - 2), bytes[end]);
                at = end + 1;
                continue;
            }
            const auto lead = static_cast<unsigned char>(bytes[at]);
            const std::size_t length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if (lead == '\r') {
                x_ = 0;
            } else if (lead == '\n') {
                ++y_;
            } else if (lead >= 0x20 && y_ < static_cast<int>(cells_.size()) && x_ < columns_) {
                GridCell& cell = cells_[static_cast<std::size_t>(y_)][static_cast<std::size_t>(x_)];
                cell = style_;
                cell.text = std::string(bytes.substr(at, length));
                ++x_;
            }
            at += length;
        }
    }

    // Sağdaki boşluklar kırpılmış satır.
    std::string row(int y) const {
        std::string out;
        for (const GridCell& cell : cells_[static_cast<std::size_t>(y)])
            out += cell.text;
        while (out.ends_with(' '))
            out.pop_back();
        return out;
    }

    const GridCell& at(int x, int y) const {
        return cells_[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
    }

private:
    void control(std::string_view params, char final_byte) {
        if (params.starts_with('?') || params.ends_with(' '))
            return;  // kip ve imleç biçimi
        std::vector<int> values;
        for (std::size_t begin = 0; begin <= params.size();) {
            const std::size_t cut = std::min(params.find(';', begin), params.size());
            values.push_back(cut == begin ? 0 : std::stoi(std::string(params.substr(begin, cut - begin))));
            begin = cut + 1;
        }
        const int first = values.front();
        if (final_byte == 'H') {
            y_ = std::max(first, 1) - 1;
            x_ = values.size() > 1 ? std::max(values[1], 1) - 1 : 0;
        } else if (final_byte == 'G') {
            x_ = std::max(first, 1) - 1;
        } else if (final_byte == 'C') {
            x_ += std::max(first, 1);
        } else if (final_byte == 'J' && first == 2) {
            for (auto& line : cells_)
                std::ranges::fill(line, GridCell{});
        } else if (final_byte == 'm') {
            sgr(values);
        }
    }

    void sgr(const std::vector<int>& values) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            const int code = values[i];
            if (code == 0)
                style_ = GridCell{};
            else if (code == 1)
                style_.bold = true;
            else if (code == 22)
                style_.bold = false;
            else if (code == 7)
                style_.inverse = true;
            else if (code == 27)
                style_.inverse = false;
            else if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97) || code == 39)
                style_.foreground = code;
            else if ((code >= 40 && code <= 47) || (code >= 100 && code <= 107) || code == 49)
                style_.background = code;
            else if ((code == 38 || code == 48) && i + 1 < values.size())
                i += values[i + 1] == 5 ? 2 : 4;  // 256 ve truecolor: bu testler 16 rengi sınar
        }
    }

    int columns_;
    std::vector<std::vector<GridCell>> cells_;
    int x_ = 0;
    int y_ = 0;
    GridCell style_;
};

// Açılış kareleri (işçi sonuçları dahil) susana dek: ekranın tamamı.
Grid settled_screen(const PtyApp& app, int columns, int rows) {
    std::string bytes = app.read_frame_text();
    const auto until = std::chrono::steady_clock::now() + kSettleLimit;
    while (std::chrono::steady_clock::now() < until && app.read_for(kQuiet, &bytes) > 0) {
    }
    Grid grid(columns, rows);
    grid.feed(bytes);
    return grid;
}

}  // namespace

// U2 T3: ipucu çubuğu gerçek terminalde. En küçük pencerede de yardım görünür, 120 sütunda ilk altı ipucu
// tamdır.
TEST_CASE("the real TUI keeps F1 help at the right of the hint bar at 60x15 and 120x40") {
    for (const auto& [columns, rows] : {std::pair{60, 15}, std::pair{120, 40}}) {
        CAPTURE(columns);
        const DrawCountFixture f;
        const PtyApp app(NOTEAPP_NOTES_EXE, {}, columns, rows);
        const Grid screen = settled_screen(app, columns, rows);
        const std::string bar = screen.row(rows - 1);
        CAPTURE(bar);
        CHECK(bar.ends_with("F1 help"));
        CHECK(screen.at(columns - 1, rows - 1).text == "p");  // sağ kenara yaslı
        if (columns == 120)
            for (const char* hint : {": cmd", "t trash", "Space open", "o sort", "V col", "s search"})
                CHECK(bar.find(hint) != std::string::npos);
    }
}

// NO_COLOR'da hap zemin rengini kaybeder: ters ve kalın kalır ki kip yine bir bakışta okunsun.
TEST_CASE("the mode pill is black on cyan, and inverted and bold under NO_COLOR") {
    constexpr int kPillColumns = 120;
    constexpr int kPillRows = 40;
    constexpr int kPillCells = 6;  // " LIST "
    for (const bool no_color : {false, true}) {
        CAPTURE(no_color);
        const DrawCountFixture f;
        const noteapp::test::ScopedEnv color("NO_COLOR", no_color ? std::string("1") : std::string());
        const PtyApp app(NOTEAPP_NOTES_EXE, {}, kPillColumns, kPillRows);
        const Grid screen = settled_screen(app, kPillColumns, kPillRows);
        const int status_row = kPillRows - 2;
        CHECK(screen.row(status_row).starts_with(" LIST  "));
        for (int x = 0; x < kPillCells; ++x) {
            CAPTURE(x);
            const GridCell& cell = screen.at(x, status_row);
            CHECK(cell.bold);
            CHECK(cell.inverse == no_color);
            CHECK(cell.background == (no_color ? 49 : 46));
            CHECK(cell.foreground == (no_color ? 39 : 30));
        }
        CHECK_FALSE(screen.at(kPillCells, status_row).inverse);
    }
}

// NX-B: Ayarlar kirliyken Ctrl+C hemen çıkmaz; GUI pencere kapanışı gibi Kaydet / At / Vazgeç sorulur.
TEST_CASE("Ctrl+C with unsaved Settings asks first; Cancel stays, Discard quits without writing") {
    noteapp::test::TempDir home;
    noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    english_config(home.path());
    const std::filesystem::path config = home.path() / "config" / "config.toml";
    const std::string before = noteapp::test::slurp(config);
    PtyApp app(NOTEAPP_NOTES_EXE);
    app.read_frame();
    app.send(",");  // Ayarlar
    app.read_frame();
    app.send("\x1B[B");  // notes_dir satırı
    app.read_frame();
    app.send("x");
    app.read_frame();
    app.send("\x03");
    CHECK(app.read_frame_text().find("Save them?") != std::string::npos);
    app.send("\r");  // varsayılan Vazgeç: uygulama açık kalır
    app.read_frame();
    app.send("\x03");
    app.read_frame();
    app.send("\x1B[D");  // At
    app.read_frame();
    app.send("\r");
    app.read_until_exit();
    CHECK(noteapp::test::slurp(config) == before);
}

namespace {

constexpr int kEditorRow = 2;        // config_entries sırası: language, notes_dir, editor…
constexpr int kIdleSecondsRow = 10;  // sync.idle_seconds

// Ayarlar açık, `down` satır aşağıdaki alana "x" yazılmış.
void dirty_settings(const PtyApp& app, int down) {
    app.read_frame();
    app.send(",");
    app.read_frame();
    for (int row = 0; row < down; ++row) {
        app.send("\x1B[B");
        app.read_frame();
    }
    app.send("x");
    app.read_frame();
}

}  // namespace

// NX-D: soru açıkken ikinci Ctrl+C soruyu kapatmaz, çıkmaz da: ok yine düğmeler arasında gezer.
TEST_CASE("a second Ctrl+C keeps the unsaved Settings prompt open") {
    noteapp::test::TempDir home;
    noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    english_config(home.path());
    const std::filesystem::path config = home.path() / "config" / "config.toml";
    const std::string before = noteapp::test::slurp(config);
    PtyApp app(NOTEAPP_NOTES_EXE);
    dirty_settings(app, kEditorRow);
    app.send("\x03");
    CHECK(app.read_frame_text().find("Save them?") != std::string::npos);
    app.send("\x03");
    app.read_for(std::chrono::milliseconds(200));
    app.send("\x1B[D");  // soru açıksa At düğmesine geçer
    CHECK(app.read_frame_text().find("Discard") != std::string::npos);
    app.send("\r");
    app.read_until_exit();
    CHECK(app.exited());
    CHECK(noteapp::test::slurp(config) == before);
}

// Ctrl+C sorusunda Kaydet kaydeder ve çıkar (TuiSession: SubmitAndQuit -> Exit).
TEST_CASE("Save at the Ctrl+C prompt writes the settings and quits") {
    noteapp::test::TempDir home;
    noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    english_config(home.path());
    const std::filesystem::path config = home.path() / "config" / "config.toml";
    PtyApp app(NOTEAPP_NOTES_EXE);
    dirty_settings(app, kEditorRow);
    app.send("\x03");
    CHECK(app.read_frame_text().find("Save them?") != std::string::npos);
    app.send("\x1B[D");
    app.read_frame();
    app.send("\x1B[D");  // Kaydet
    app.read_frame();
    app.send("\r");
    app.read_until_exit();
    CHECK(app.exited());
    const std::string written = noteapp::test::slurp(config);
    CAPTURE(written);
    CHECK(written.find("editor = 'x'") != std::string::npos);
}

// Kaydet doğrulamadan geçmezse uygulama açık kalır ve hiçbir şey yazılmaz; Ctrl+C soruyu yeniden açar.
TEST_CASE("a Save that fails validation at the Ctrl+C prompt keeps the app open") {
    noteapp::test::TempDir home;
    noteapp::test::ScopedEnv env("NOTEAPP_HOME", noteapp::test::utf8(home.path()));
    english_config(home.path());
    const std::filesystem::path config = home.path() / "config" / "config.toml";
    const std::string before = noteapp::test::slurp(config);
    PtyApp app(NOTEAPP_NOTES_EXE);
    dirty_settings(app, kIdleSecondsRow);  // "…x" sayı değil
    app.send("\x03");
    CHECK(app.read_frame_text().find("Save them?") != std::string::npos);
    app.send("\x1B[D");
    app.read_frame();
    app.send("\x1B[D");
    app.read_frame();
    app.send("\r");
    app.read_frame();
    app.send("\x03");
    CHECK(app.read_frame_text().find("Save them?") != std::string::npos);
    CHECK(noteapp::test::slurp(config) == before);
    app.send("\x1B[D");
    app.read_frame();
    app.send("\r");
    app.read_until_exit();
    CHECK(app.exited());
}

namespace {

// NY-T yapıştırma: terminal Ctrl+Shift+V'yi ya bracketed paste (ESC[200~ … ESC[201~) ya da tuş olayları olarak
// gönderir. Eskiden CRLF iki Enter'dı (satır arası boş satır, sonda iki katı boş satır); metin notun satır
// sonuyla bayt bayt girmeli.
constexpr std::string_view kPasteStart = "\x1B[200~";
constexpr std::string_view kPasteEnd = "\x1B[201~";
constexpr int kPasteAutosaveMs = 200;

std::string bracketed(std::string_view text) {
    return std::string(kPasteStart) + std::string(text) + std::string(kPasteEnd);
}

// Kayıt bitene dek bekler (en çok kSettleLimit); son okunan gövdeyi döner.
std::string wait_body(const PtyApp& app, const std::filesystem::path& data, std::string_view path,
                      std::string_view expected) {
    const auto until = std::chrono::steady_clock::now() + kSettleLimit;
    std::string body = noteapp::test::note_body(data, path);
    while (body != expected && std::chrono::steady_clock::now() < until) {
        app.read_for(std::chrono::milliseconds(50));
        body = noteapp::test::note_body(data, path);
    }
    return body;
}

// Kayıtlı not yolları, sıralı; `count` kadar not olunca (en çok kSettleLimit) döner.
std::vector<std::string> wait_notes(const PtyApp& app, const std::filesystem::path& data, std::size_t count) {
    std::vector<std::string> names;
    const auto until = std::chrono::steady_clock::now() + kSettleLimit;
    while (names.size() < count && std::chrono::steady_clock::now() < until) {
        app.read_for(std::chrono::milliseconds(50));
        names.clear();
        for (const noteapp::core::NoteInfo& note : noteapp::core::NoteStore(data).list())
            names.push_back(note.path.string());
    }
    std::ranges::sort(names);
    return names;
}

struct PasteCase {
    std::string_view name;
    std::string sent;
    std::string_view note;      // yapıştırmadan önceki not (imleç başta)
    std::string_view expected;  // otomatik kayıttan sonra
};

}  // namespace

TEST_CASE("a terminal paste lands byte for byte in the note: CRLF, CR, LF, mixed, bracketed or not") {
    const std::vector<PasteCase> cases{
        {"bracketed CRLF", bracketed("one\r\ntwo\r\n\r\nthree\r\n"), "x\n", "one\ntwo\n\nthree\nx\n"},
        {"bracketed CR (Konsole, VTE, xterm)", bracketed("one\rtwo\r\rthree\r"), "x\n", "one\ntwo\n\nthree\nx\n"},
        {"bracketed LF", bracketed("one\ntwo\n\nthree\n"), "x\n", "one\ntwo\n\nthree\nx\n"},
        {"bracketed mixed", bracketed("one\r\ntwo\rthree\nfour"), "x\n", "one\ntwo\nthree\nfourx\n"},
        {"bracketed trailing newlines", bracketed("end\r\n\r\n\r\n"), "x\n", "end\n\n\nx\n"},
        {"bracketed list, indent and tab", bracketed("- a\r  - b\r\tc\r"), "x\n", "- a\n  - b\n\tc\nx\n"},
        {"bracketed into a CRLF note", bracketed("one\ntwo\n"), "x\r\n", "one\r\ntwo\r\nx\r\n"},
        {"unbracketed CR keys", "one\rtwo\r\rthree\r", "x\n", "one\ntwo\n\nthree\nx\n"},
        {"unbracketed CRLF", "one\r\ntwo\r\n\r\nthree\r\n", "x\n", "one\ntwo\n\nthree\nx\n"},
    };
    for (const PasteCase& paste : cases) {
        CAPTURE(paste.name);
        const DrawCountFixture f(kPasteAutosaveMs);
        noteapp::test::upsert_note(f.home.path() / "data", "a.py", paste.note);
        const PtyApp app(NOTEAPP_NOTES_EXE);
        app.read_frame();
        wait_quiet(app, kQuiet, kSettleLimit);
        app.send("\r");  // Enter: tek not editörde, imleç başta
        app.read_frame();
        wait_quiet(app, kQuiet, kSettleLimit);
        app.send(paste.sent);
        CHECK(wait_body(app, f.home.path() / "data", "a.py", paste.expected) == paste.expected);
    }
}

TEST_CASE("a bracketed paste is one edit drawn in one frame, and the mode is left at exit") {
    const DrawCountFixture f;
    const PtyApp app(NOTEAPP_NOTES_EXE);
    CHECK(app.read_frame_text().find("\x1B[?2004h") != std::string::npos);  // terminalden istenir
    wait_quiet(app, kQuiet, kSettleLimit);
    app.send("\r");
    app.read_frame();
    wait_quiet(app, kQuiet, kSettleLimit);
    const std::size_t before = draws(f.log);
    app.send(bracketed("one\r\ntwo\r\nthree\r\n"));
    app.read_frame();
    wait_quiet(app, kQuiet, kSettleLimit);
    CHECK(draws(f.log) == before + 1);
    app.send("\x1A");  // Ctrl+Z: yapıştırma tek geri alma adımı
    app.read_frame();
    app.send("y");
    app.read_frame();
    app.send("\x13");  // Ctrl+S
    CHECK(wait_body(app, f.home.path() / "data", "a.py", "yx = 1\n") == "yx = 1\n");
    app.signal(SIGTERM);
    CHECK(app.read_until_exit().find("\x1B[?2004l") != std::string::npos);
}

TEST_CASE("a paste into a new note draft keeps its lines; the name comes from the first line") {
    const DrawCountFixture f;
    const PtyApp app(NOTEAPP_NOTES_EXE);
    app.read_frame();
    wait_quiet(app, kQuiet, kSettleLimit);
    app.send("a");  // ad sormadan boş taslak
    app.read_frame();
    wait_quiet(app, kQuiet, kSettleLimit);
    app.send(bracketed("Shopping\r\n\r\n- milk\r\n- eggs\r\n"));
    app.read_frame();
    app.send("\x13");  // ilk Ctrl+S ad girdisini açar (ilk satırın slug'ı)
    app.read_frame();
    wait_quiet(app, kQuiet, kSettleLimit);
    app.send("\r");
    const std::filesystem::path data = f.home.path() / "data";
    const std::vector<std::string> names = wait_notes(app, data, 2);
    REQUIRE(names.size() == 2);
    CHECK(names[1].starts_with("shopping."));
    CHECK(noteapp::test::note_body(data, names[1]) == "Shopping\n\n- milk\n- eggs\n");
}

TEST_CASE("a multi-line paste into the new-note name row is one name, not an early Enter") {
    const DrawCountFixture f;
    const PtyApp app(NOTEAPP_NOTES_EXE);
    app.read_frame();
    wait_quiet(app, kQuiet, kSettleLimit);
    app.send("n");  // listede ad satırı
    app.read_frame();
    wait_quiet(app, kQuiet, kSettleLimit);
    app.send(bracketed("weekly\r\nplan\r\n"));
    app.read_frame();
    app.send("\r");
    CHECK(wait_notes(app, f.home.path() / "data", 2) == std::vector<std::string>{"a.py", "weekly-plan.md"});
}

namespace {

// Bir yapıştırma birden çok okumada gelir: parçalar arasında terminal susar.
void send_in_reads(const PtyApp& app, const std::vector<std::string>& parts) {
    for (const std::string& part : parts) {
        app.send(part);
        app.read_for(std::chrono::milliseconds(120));
    }
}

// Editörde a.py açık, imleç başta.
void open_editor(const PtyApp& app) {
    app.read_frame();
    wait_quiet(app, kQuiet, kSettleLimit);
    app.send("\r");
    app.read_frame();
    wait_quiet(app, kQuiet, kSettleLimit);
}

}  // namespace

TEST_CASE("a paste split across reads keeps its bytes: the end mark, UTF-8 and escape sequences inside") {
    struct SplitCase {
        std::string_view name;
        std::vector<std::string> parts;
        std::string_view expected;
    };
    const std::vector<SplitCase> cases{
        {"end mark split", {std::string(kPasteStart) + "ab\x1B[20", "1~"}, "abx = 1\n"},
        {"UTF-8 split", {std::string(kPasteStart) + "\xC5", "\x9F" + std::string(kPasteEnd)}, "\xC5\x9Fx = 1\n"},
        {"escape sequences inside", {bracketed("a\x1B[Ab\x1B[1;5Cc")}, "a\x1B[Ab\x1B[1;5Ccx = 1\n"},
    };
    for (const SplitCase& paste : cases) {
        CAPTURE(paste.name);
        const DrawCountFixture f(kPasteAutosaveMs);
        const PtyApp app(NOTEAPP_NOTES_EXE);
        open_editor(app);
        send_in_reads(app, paste.parts);
        CHECK(wait_body(app, f.home.path() / "data", "a.py", paste.expected) == paste.expected);
    }
}

// Hiç bitmeyen yapıştırma tuşları sonsuza dek yutmasın: limits.tui.paste_timeout sessizlikten sonra toplananı
// yapıştırır, sonraki tuş yine çalışır.
TEST_CASE("a paste that never ends is delivered after limits.tui.paste_timeout and keys work again") {
    const DrawCountFixture f(kPasteAutosaveMs, "paste_timeout = \"300ms\"");
    const PtyApp app(NOTEAPP_NOTES_EXE);
    open_editor(app);
    app.send(std::string(kPasteStart) + "lost");
    app.read_for(std::chrono::milliseconds(900));
    app.send("z");
    CHECK(wait_body(app, f.home.path() / "data", "a.py", "lostzx = 1\n") == "lostzx = 1\n");
}

// Ctrl+V ile aynı sınır (limits.tui.paste_bytes): aşan terminal yapıştırması metne girmez, neden söylenir.
TEST_CASE("a terminal paste over limits.tui.paste_bytes changes nothing and says why") {
    const DrawCountFixture f(kPasteAutosaveMs, "paste_bytes = 16");
    const PtyApp app(NOTEAPP_NOTES_EXE);
    open_editor(app);
    app.send(bracketed("0123456789abcdefg"));  // 17 bayt
    std::string seen;
    app.read_for(std::chrono::milliseconds(500), &seen);
    CHECK(seen.find("Not pasted") != std::string::npos);
    app.send(bracketed("0123456789abcdef"));  // 16 bayt: sınırda girer
    CHECK(wait_body(app, f.home.path() / "data", "a.py", "0123456789abcdefx = 1\n") == "0123456789abcdefx = 1\n");
}

namespace {

constexpr std::string_view kShortPasteTimeout = "paste_timeout = \"300ms\"";
constexpr auto kPastTimeout = std::chrono::milliseconds(450);  // 300 ms sessizliğin ötesi, ikincisinden önce

}  // namespace

// Zaman aşımıyla biten yapıştırmanın geç gelen devamı tuş değildir: metin olarak eklenir, hiçbir komut koşmaz.
TEST_CASE("the late rest of a paste that timed out lands as text in the editor; Ctrl+Q in it is a byte") {
    const DrawCountFixture f(kPasteAutosaveMs, kShortPasteTimeout);
    const PtyApp app(NOTEAPP_NOTES_EXE);
    open_editor(app);
    app.send(std::string(kPasteStart) + "part1");
    app.read_for(kPastTimeout);
    app.send("part2\x11" + std::string(kPasteEnd));
    CHECK(wait_body(app, f.home.path() / "data", "a.py", "part1part2\x11x = 1\n") == "part1part2\x11x = 1\n");
}

TEST_CASE("the late rest of a paste that timed out runs no list command") {
    const DrawCountFixture f(kPasteAutosaveMs, kShortPasteTimeout);
    const PtyApp app(NOTEAPP_NOTES_EXE);
    app.read_frame();
    wait_quiet(app, kQuiet, kSettleLimit);
    app.send(std::string(kPasteStart) + "ab");
    app.read_for(kPastTimeout);
    app.send("dD" + std::string(kPasteEnd));  // tuş olsaydı: çöpe at, kalıcı sil sorusu
    wait_quiet(app, kQuiet, kSettleLimit);
    std::vector<std::string> names;
    for (const noteapp::core::NoteInfo& note : noteapp::core::NoteStore(f.home.path() / "data").list())
        names.push_back(note.path.string());
    CHECK(names == std::vector<std::string>{"a.py"});
}

// Toplamda zaman aşımından uzun ama arası kısa akış bölünmez: tek yapıştırma, tek geri alma adımı.
TEST_CASE("a long paste with short gaps does not end early: one undo step takes all of it back") {
    const DrawCountFixture f(kPasteAutosaveMs, kShortPasteTimeout);
    const PtyApp app(NOTEAPP_NOTES_EXE);
    open_editor(app);
    std::vector<std::string> parts{std::string(kPasteStart) + "a"};
    for (int i = 0; i < 6; ++i)  // 6 x 120 ms > 300 ms
        parts.emplace_back("b");
    parts.emplace_back(kPasteEnd);
    send_in_reads(app, parts);
    CHECK(wait_body(app, f.home.path() / "data", "a.py", "abbbbbbx = 1\n") == "abbbbbbx = 1\n");
    app.send("\x1A");  // Ctrl+Z
    app.read_frame();
    app.send("y");
    CHECK(wait_body(app, f.home.path() / "data", "a.py", "yx = 1\n") == "yx = 1\n");
}

TEST_CASE("paste_timeout = 0 waits for the end mark however long it takes") {
    const DrawCountFixture f(kPasteAutosaveMs, "paste_timeout = 0");
    const PtyApp app(NOTEAPP_NOTES_EXE);
    open_editor(app);
    app.send(std::string(kPasteStart) + "abc");
    app.read_for(std::chrono::milliseconds(1500));
    app.send("\x11" + std::string(kPasteEnd));  // tuş olsaydı Ctrl+Q çıkış sorusu
    CHECK(wait_body(app, f.home.path() / "data", "a.py", "abc\x11x = 1\n") == "abc\x11x = 1\n");
}

// Ayrıştırıcı sınırı aşan baytı saklamaz: dev yapıştırma belleği şişirmez, sonunda neden söylenir.
TEST_CASE("a paste far over limits.tui.paste_bytes is not kept in memory and says why") {
    constexpr std::size_t kPasteMiB = 24;
    const DrawCountFixture f(kPasteAutosaveMs, "paste_bytes = \"4KiB\"");
    const PtyApp app(NOTEAPP_NOTES_EXE);
    open_editor(app);
    const std::size_t before = app.peak_rss_kib();
    REQUIRE(before > 0);
    app.send(bracketed(std::string(kPasteMiB * 1024 * 1024, 'x')));
    std::string seen;
    app.read_for(std::chrono::milliseconds(3000), &seen);
    CHECK(seen.find("Not pasted") != std::string::npos);
    const std::size_t grown = app.peak_rss_kib() - before;
    CAPTURE(grown);
    CHECK(grown < kPasteMiB * 1024 / 4);  // eskiden metnin tamamı (ve kopyası) bellekteydi
    CHECK(noteapp::test::note_body(f.home.path() / "data", "a.py") == "x = 1\n");
}
