// Dosyayı not olarak içe alma (CLI `note import`, TUI ve GUI aynı kural): baytlar aynen, ad dosya adından,
// dolu adda deponun `-2` adı, klasör alt ağacıyla, bağ ve gizli girdi atlanır.
#include "app/document.hpp"
#include "app/import_files.hpp"
#include "core/file_stat.hpp"
#include "core/i18n.hpp"
#include "detect/detect.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using noteapp::app::ImportOptions;
using noteapp::app::ImportReport;
using noteapp::core::NotePath;
using noteapp::core::NoteStore;

namespace {

const noteapp::detect::Options kDetect{"txt", 0.5, 65536};

struct Scene {
    noteapp::test::TempDir files;  // içe alınacak dosyalar
    noteapp::test::TempDir data;
    NoteStore store{data.path()};

    fs::path write(const std::string& relative, const std::string& bytes) const {
        const fs::path file = files.path() / fs::path(std::u8string(relative.begin(), relative.end()));
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
        return file;
    }
    ImportReport run(std::vector<fs::path> sources, ImportOptions options = {}) {
        options.detect = kDetect;
        return noteapp::app::import_paths(store, sources, options);
    }
    std::string body(const std::string& path) const {
        const auto content = store.read(*NotePath::parse(path));
        return content ? content->text : "<missing>";
    }
    std::vector<std::string> paths() const {
        std::vector<std::string> out;
        for (const auto& info : store.list())
            out.push_back(info.path.string());
        std::ranges::sort(out);
        return out;
    }
};

std::vector<std::string> imported_notes(const ImportReport& report) {
    std::vector<std::string> out;
    for (const auto& item : report.imported)
        out.push_back(item.note.string());
    return out;
}

}  // namespace

TEST_CASE("a text file becomes a note named after the file, its type from the extension") {
    Scene scene;
    const fs::path file = scene.write("plan.md", "# Plan\n");
    const ImportReport report = scene.run({file});
    CHECK(report.complete());
    CHECK(report.wrote);
    REQUIRE(report.imported.size() == 1);
    CHECK(report.imported[0].note.string() == "plan.md");
    CHECK(report.imported[0].source == noteapp::test::utf8(file));
    CHECK_FALSE(report.imported[0].renamed);
    CHECK(scene.body("plan.md") == "# Plan\n");
    CHECK(noteapp::test::slurp(file) == "# Plan\n");  // özgün dosya yerinde, aynı
}

TEST_CASE("CRLF, BOM and binary bytes are kept exactly; a binary note is read-only") {
    Scene scene;
    const std::string crlf_bom = "\xEF\xBB\xBFline one\r\nline two\r\n";
    std::string binary("PK\x03\x04", 4);
    binary += std::string("\0\x01\xFF\xFE", 4);
    const ImportReport report =
        scene.run({scene.write("win.txt", crlf_bom), scene.write("pack.zip", binary)});
    CHECK(report.complete());
    CHECK(scene.body("win.txt") == crlf_bom);
    CHECK(scene.body("pack.zip") == binary);
    CHECK(noteapp::app::classify(scene.body("pack.zip"), std::numeric_limits<std::uintmax_t>::max()) ==
          noteapp::app::ReadOnly::Binary);
}

TEST_CASE("a file without an extension gets the detected type, like a new note") {
    Scene scene;
    const std::string bytes = "def main():\n    print('hi')\n";
    const ImportReport report = scene.run({scene.write("script", bytes)});
    REQUIRE(report.imported.size() == 1);
    CHECK(report.imported[0].note.string() == "script." + noteapp::detect::suggest(bytes, kDetect).type);
}

TEST_CASE("a folder is imported with its own name, nested folders and empty folders") {
    Scene scene;
    scene.write("docs/a.md", "a");
    scene.write("docs/sub/b.txt", "b");
    scene.write("docs/sub/deeper/c.md", "c");
    fs::create_directories(scene.files.path() / "docs" / "empty");
    const ImportReport report = scene.run({scene.files.path() / "docs"}, {.folder = "inbox"});
    CHECK(report.complete());
    CHECK(scene.paths() ==
          std::vector<std::string>{"inbox/docs/a.md", "inbox/docs/sub/b.txt", "inbox/docs/sub/deeper/c.md"});
    CHECK(scene.body("inbox/docs/sub/deeper/c.md") == "c");
    CHECK(noteapp::test::folder_exists(scene.data.path(), "inbox/docs/empty"));
}

TEST_CASE("a taken name gets the store's -2, -3 numbering; nothing is overwritten") {
    Scene scene;
    REQUIRE(noteapp::test::seed_note(scene.store, "plan.md", "old"));
    const fs::path file = scene.write("plan.md", "new");
    const ImportReport first = scene.run({file});
    const ImportReport second = scene.run({file});
    CHECK(imported_notes(first) == std::vector<std::string>{"plan-2.md"});
    CHECK(imported_notes(second) == std::vector<std::string>{"plan-3.md"});
    CHECK(first.imported[0].renamed);
    CHECK(scene.body("plan.md") == "old");
    CHECK(scene.body("plan-3.md") == "new");
}

TEST_CASE("overwrite replaces the bytes of the note at the same path") {
    Scene scene;
    REQUIRE(noteapp::test::seed_note(scene.store, "plan.md", "old"));
    const ImportReport report = scene.run({scene.write("plan.md", "new")}, {.overwrite = true});
    CHECK(report.complete());
    REQUIRE(report.imported.size() == 1);
    CHECK(report.imported[0].overwritten);
    CHECK(scene.body("plan.md") == "new");
    CHECK(scene.paths() == std::vector<std::string>{"plan.md"});
}

TEST_CASE("symbolic links are not followed and are reported as skipped") {
    Scene scene;
    const fs::path target = scene.write("real.md", "real");
    scene.write("docs/inside.md", "inside");
    std::error_code ec;
    fs::create_symlink(target, scene.files.path() / "docs" / "link.md", ec);
    if (ec) {
        MESSAGE("symbolic links are not available here: " << ec.message());
        return;
    }
    REQUIRE(noteapp::test::make_dir_link(scene.files.path(), scene.files.path() / "docs" / "loop"));
    const ImportReport report =
        scene.run({scene.files.path() / "docs", scene.files.path() / "docs" / "link.md"});
    CHECK(scene.paths() == std::vector<std::string>{"docs/inside.md"});
    REQUIRE(report.skipped.size() == 3);
    for (const auto& skipped : report.skipped)
        CHECK(skipped.reason == "import_link");
    CHECK_FALSE(report.complete());
}

TEST_CASE("hidden entries are skipped inside folders but a hidden path given by name is tried") {
    Scene scene;
    scene.write("docs/seen.md", "seen");
    scene.write("docs/.secret.md", "secret");
    scene.write("docs/.git/HEAD", "ref");
    const ImportReport walked = scene.run({scene.files.path() / "docs"});
    CHECK(scene.paths() == std::vector<std::string>{"docs/seen.md"});
    CHECK(walked.hidden == 2);
    CHECK(walked.complete());  // gizli girdi bilerek dışarıda: eksik sayılmaz

    // Açıkça verilen gizli dosya denenir; '.' ile başlayan not adı geçersizdir, ad verilince alınır.
    const fs::path env = scene.write(".env", "KEY=1\n");
    const ImportReport named = scene.run({env});
    REQUIRE(named.failed.size() == 1);
    CHECK(named.failed[0].error.code == noteapp::core::Error::Code::InvalidPath);
    const ImportReport renamed = scene.run({env}, {.name = "env.txt"});
    CHECK(imported_notes(renamed) == std::vector<std::string>{"env.txt"});
    CHECK(scene.body("env.txt") == "KEY=1\n");
}

TEST_CASE("folder, name and type options place and name the note") {
    Scene scene;
    const fs::path file = scene.write("plan.txt", "x");
    CHECK(imported_notes(scene.run({file}, {.folder = "work/inbox"})) ==
          std::vector<std::string>{"work/inbox/plan.txt"});
    CHECK(imported_notes(scene.run({file}, {.name = "fresh"})) == std::vector<std::string>{"fresh.txt"});
    CHECK(imported_notes(scene.run({file}, {.name = "fresh.md"})) == std::vector<std::string>{"fresh.md"});
    CHECK(imported_notes(scene.run({file}, {.type = "md"})) == std::vector<std::string>{"plan.md"});
    scene.write("tree/a.txt", "a");
    CHECK(imported_notes(scene.run({scene.files.path() / "tree"}, {.name = "archive"})) ==
          std::vector<std::string>{"archive/a.txt"});
}

TEST_CASE("a missing path fails with not found; the other paths still import") {
    Scene scene;
    const fs::path missing = scene.files.path() / "nope.md";
    const ImportReport report = scene.run({missing, scene.write("ok.md", "ok")});
    CHECK(imported_notes(report) == std::vector<std::string>{"ok.md"});
    REQUIRE(report.failed.size() == 1);
    CHECK(report.failed[0].source == noteapp::test::utf8(missing));
    CHECK(report.failed[0].error.code == noteapp::core::Error::Code::NotFound);
    CHECK_FALSE(report.complete());
}

TEST_CASE("created_at and updated_at come from the file, not from the import time") {
    Scene scene;
    const fs::path file = scene.write("old.md", "old");
    const auto then = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2} + std::chrono::hours(3);
    fs::last_write_time(file, std::chrono::clock_cast<std::chrono::file_clock>(then));
    REQUIRE(scene.run({file}).complete());
    const auto metas = scene.store.list_meta();
    REQUIRE(metas.size() == 1);
    using std::chrono::milliseconds;
    using std::chrono::time_point_cast;
    CHECK(time_point_cast<milliseconds>(noteapp::core::to_system_time(metas[0].updated_at)) ==
          time_point_cast<milliseconds>(then));
    REQUIRE(metas[0].created_at.has_value());
    // Doğum zamanı mtime'dan yeniyse (kopya) mtime kazanır: test dosyası az önce yazıldı.
    CHECK(time_point_cast<milliseconds>(noteapp::core::to_system_time(*metas[0].created_at)) ==
          time_point_cast<milliseconds>(then));
}

// Dosya belleğe sığmazsa okuma bad_alloc (ya da string'in sınırında length_error) atar; dev bellek ayırmadan
// okuyucu benzetir. Süreç düşmez: o dosya alınamayanlardadır, sonrakiler alınır.
TEST_CASE("a file too large for memory fails as an item; the rest of the import goes on") {
    Scene scene;
    const fs::path huge = scene.write("huge.bin", "x");
    const fs::path longest = scene.write("longest.bin", "x");
    const fs::path ok = scene.write("ok.md", "ok");
    const noteapp::app::ImportControl control{
        .read = [&](const fs::path& file) -> std::expected<std::string, std::string> {
            if (file == huge)
                throw std::bad_alloc();
            if (file == longest)
                throw std::length_error("basic_string::_M_create");
            std::ifstream in(file, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), {});
        }};
    ImportOptions options;
    options.detect = kDetect;
    const ImportReport report =
        noteapp::app::import_paths(scene.store, std::vector{huge, longest, ok}, options, control);
    CHECK(imported_notes(report) == std::vector<std::string>{"ok.md"});
    REQUIRE(report.failed.size() == 2);
    CHECK(report.failed[0].source == noteapp::test::utf8(huge));
    CHECK(report.failed[0].error.message_key == "error.out_of_memory");
    CHECK(report.failed[1].source == noteapp::test::utf8(longest));
    CHECK(report.failed[1].error.message_key == "error.out_of_memory");
    CHECK(scene.paths() == std::vector<std::string>{"ok.md"});
    noteapp::core::init_i18n("en");
    CHECK(noteapp::core::format_error(report.failed[0].error) ==
          "Not enough memory to read '" + noteapp::test::utf8(huge) + "'; it was not imported");
}

TEST_CASE("progress names each entry with its number and the total; hidden entries do not count") {
    Scene scene;
    const fs::path a = scene.write("a.md", "a");
    scene.write("dir/b.md", "b");
    scene.write("dir/c.md", "c");
    scene.write("dir/.hidden", "h");
    std::vector<noteapp::app::ImportProgress> seen;
    ImportOptions options;
    options.detect = kDetect;
    const noteapp::app::ImportControl control{
        .progress = [&](const noteapp::app::ImportProgress& progress) { seen.push_back(progress); }};
    const fs::path dir = scene.files.path() / "dir";
    const ImportReport report =
        noteapp::app::import_paths(scene.store, std::vector{a, dir}, options, control);
    CHECK(report.complete());
    REQUIRE(seen.size() == 4);
    const std::vector<fs::path> order{a, dir, dir / "b.md", dir / "c.md"};
    for (std::size_t i = 0; i < seen.size(); ++i) {
        CAPTURE(i);
        CHECK(seen[i].done == i + 1);
        CHECK(seen[i].total == 4);
        CHECK(seen[i].current == noteapp::test::utf8(order[i]));
    }
    noteapp::core::init_i18n("en");
    CHECK(noteapp::app::import_progress_text(seen[1]) == "Importing 2/4: " + noteapp::test::utf8(dir));
}

// Durdurma girdi sınırında: şimdiki girdi biter, kalanlar alınmaz ve sayılır. Durdurulunca klasörün içine
// inilmez (büyük ağaç durdurmayı bekletmesin): alınmayan klasör bir girdidir.
TEST_CASE("a stop keeps what was imported and counts the entries left out, a folder once") {
    Scene scene;
    const fs::path a = scene.write("a.md", "a");
    scene.write("dir/b.md", "b");
    scene.write("dir/sub/c.md", "c");
    const fs::path last = scene.write("z.md", "z");
    std::stop_source stop;
    ImportOptions options;
    options.detect = kDetect;
    const noteapp::app::ImportControl control{.stop = stop.get_token(),
                                              .progress = [&](const noteapp::app::ImportProgress& progress) {
                                                  if (progress.done == 2)  // klasöre başlarken
                                                      stop.request_stop();
                                              }};
    const fs::path dir = scene.files.path() / "dir";
    const ImportReport report =
        noteapp::app::import_paths(scene.store, std::vector{a, dir, last}, options, control);
    CHECK(imported_notes(report) == std::vector<std::string>{"a.md"});
    CHECK(report.cancelled == 3);  // dir/b.md, dir/sub (içi sayılmaz), z.md
    CHECK_FALSE(report.complete());
    CHECK(scene.paths() == std::vector<std::string>{"a.md"});
    noteapp::core::init_i18n("en");
    CHECK(noteapp::app::import_status(report) ==
          "Imported: 1, skipped: 0, not imported: 0 Stopped: 3 entries were not imported (a folder counts "
          "once)");
}

// Açılışta istenen durdurma ön sayımı da kısa keser: hiçbir klasörün içi gezilmez.
TEST_CASE("a stop before the start imports nothing and does not walk the folders") {
    Scene scene;
    scene.write("dir/b.md", "b");
    scene.write("dir/sub/c.md", "c");
    const fs::path last = scene.write("z.md", "z");
    std::stop_source stop;
    stop.request_stop();
    ImportOptions options;
    options.detect = kDetect;
    int progress_calls = 0;
    const noteapp::app::ImportControl control{
        .stop = stop.get_token(), .progress = [&](const noteapp::app::ImportProgress&) { ++progress_calls; }};
    const ImportReport report = noteapp::app::import_paths(
        scene.store, std::vector{scene.files.path() / "dir", last}, options, control);
    CHECK(report.imported.empty());
    CHECK(report.cancelled == 2);
    CHECK(progress_calls == 0);
    CHECK_FALSE(report.wrote);
}

// Beklenmeyen istisna (okuma, depo) süreci düşürmez ve alınanları raporsuz bırakmaz: o girdi alınamayanlarda,
// içe alma orada durur (depo tutarlı kalsın diye sürmez), kalanlar durdurulmuş sayılır.
TEST_CASE("an unexpected exception stops the import and keeps the partial report") {
    Scene scene;
    const fs::path a = scene.write("a.md", "a");
    const fs::path b = scene.write("b.md", "b");
    const fs::path c = scene.write("c.md", "c");
    const noteapp::app::ImportControl control{
        .read = [&](const fs::path& file) -> std::expected<std::string, std::string> {
            if (file == b)
                throw std::runtime_error("disk vanished");
            std::ifstream in(file, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), {});
        }};
    ImportOptions options;
    options.detect = kDetect;
    ImportReport report;
    REQUIRE_NOTHROW(report = noteapp::app::import_paths(scene.store, std::vector{a, b, c}, options, control));
    CHECK(imported_notes(report) == std::vector<std::string>{"a.md"});
    CHECK(report.wrote);
    REQUIRE(report.failed.size() == 1);
    CHECK(report.failed[0].source == noteapp::test::utf8(b));
    CHECK(report.failed[0].error.message_key == "error.io");
    CHECK(report.cancelled == 1);  // c.md
    CHECK(scene.paths() == std::vector<std::string>{"a.md"});
}

// Arayüz post vermezse iş başlamaz (işçi boş std::function çağırıp süreci düşürmesin).
TEST_CASE("an import job without a post function refuses to start") {
    Scene scene;
    noteapp::app::ImportJob job({});
    ImportOptions options;
    options.detect = kDetect;
    CHECK_FALSE(job.start(scene.store, {scene.write("a.md", "a")}, options, {}, {}));
    CHECK_FALSE(job.running());
}

// Arayüzlerin işçisi: rapor ve ilerleme `post` kuyruğundan, çağıran iş parçacığında gelir.
TEST_CASE("an import job runs on a worker, posts progress and the report, and starts once at a time") {
    Scene scene;
    const fs::path a = scene.write("a.md", "a");
    const fs::path b = scene.write("b.md", "b");
    std::mutex mutex;
    std::vector<std::function<void()>> queue;
    noteapp::app::ImportJob job([&](std::function<void()> work) {
        const std::scoped_lock lock(mutex);
        queue.push_back(std::move(work));
    });
    const auto caller = std::this_thread::get_id();
    std::optional<noteapp::app::ImportProgress> last_progress;
    std::optional<ImportReport> report;
    ImportOptions options;
    options.detect = kDetect;
    REQUIRE(job.start(
        scene.store, {a, b}, options,
        [&](const noteapp::app::ImportProgress& progress) {
            CHECK(std::this_thread::get_id() == caller);
            CHECK_FALSE(report.has_value());  // rapor son ilerlemeden sonra
            last_progress = progress;
        },
        [&](ImportReport done) {
            CHECK(std::this_thread::get_id() == caller);
            report = std::move(done);
        }));
    CHECK(job.running());
    CHECK_FALSE(job.start(scene.store, {a}, options, {}, {}));  // biri sürerken ikincisi yok
    constexpr int kMaxWaitSteps = 3000;
    for (int step = 0; step < kMaxWaitSteps && job.running(); ++step) {
        std::vector<std::function<void()>> work;
        {
            const std::scoped_lock lock(mutex);
            work.swap(queue);
        }
        for (const auto& task : work)
            task();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(job.running());
    REQUIRE(report.has_value());
    CHECK(imported_notes(*report) == std::vector<std::string>{"a.md", "b.md"});
    REQUIRE(last_progress.has_value());
    CHECK(last_progress->done == 2);
    CHECK(last_progress->total == 2);
}
