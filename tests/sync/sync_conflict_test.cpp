#include "core/error.hpp"
#include "core/paths.hpp"
#include "sync/sync_service.hpp"
#include "sync_fixture.hpp"

#include <chrono>
#include <ctime>
#include <doctest/doctest.h>
#include <expected>
#include <filesystem>
#include <format>
#include <git2.h>
#include <initializer_list>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;
using noteapp::core::Error;
using noteapp::sync::SyncResult;
using noteapp::sync::SyncService;
using noteapp::sync::SyncTestHooks;
using namespace noteapp::sync_test;
using namespace std::string_literals;
using Clock = std::chrono::system_clock;

namespace {

using Object = std::unique_ptr<git_object, GitFree<git_object_free>>;
using StatusList = std::unique_ptr<git_status_list, GitFree<git_status_list_free>>;

SyncResult sync_ok(const fs::path& notes_dir) {
    auto result = SyncService(SyncFixture::config_for(notes_dir)).sync_now();
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    return *result;
}

std::expected<SyncResult, Error> try_sync(const fs::path& notes_dir) {
    return SyncService(SyncFixture::config_for(notes_dir)).sync_now();
}

fs::path note(std::string_view relative) {
    return fs::path(std::u8string(relative.begin(), relative.end()));
}

// Diskteki (.git hariç) tüm dosya içerikleri.
std::set<std::string> working_contents(const fs::path& dir) {
    std::set<std::string> contents;
    for (auto it = fs::recursive_directory_iterator(dir); it != fs::recursive_directory_iterator(); ++it) {
        if (it->path().filename() == ".git")
            it.disable_recursion_pending();
        else if (it->is_regular_file())
            contents.insert(read_file(it->path()));
    }
    return contents;
}

// HEAD commit'inin ağacındaki tüm blob içerikleri.
std::set<std::string> committed_contents(const fs::path& dir) {
    const Repository repo = open_repository(dir);
    git_object* raw = nullptr;
    REQUIRE_GIT(git_revparse_single(&raw, repo.get(), "HEAD^{tree}"));
    const Object tree(raw);
    struct Walk {
        git_repository* repo;
        std::set<std::string> contents;
    } walk{repo.get(), {}};
    const auto visit = [](const char*, const git_tree_entry* entry, void* payload) -> int {
        if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB)
            return 0;
        auto& state = *static_cast<Walk*>(payload);
        git_blob* blob = nullptr;
        if (git_blob_lookup(&blob, state.repo, git_tree_entry_id(entry)) < 0)
            return -1;
        state.contents.emplace(static_cast<const char*>(git_blob_rawcontent(blob)),
                               static_cast<std::size_t>(git_blob_rawsize(blob)));
        git_blob_free(blob);
        return 0;
    };
    REQUIRE_GIT(git_tree_walk(reinterpret_cast<git_tree*>(tree.get()), GIT_TREEWALK_PRE, visit, &walk));
    return walk.contents;
}

int repository_state(const fs::path& dir) {
    return git_repository_state(open_repository(dir).get());
}

// Commit'lenmemiş (izlenmeyenler dahil) değişiklik sayısı.
std::size_t uncommitted_count(const fs::path& dir) {
    const Repository repo = open_repository(dir);
    git_status_options options;
    REQUIRE_GIT(git_status_options_init(&options, GIT_STATUS_OPTIONS_VERSION));
    options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    git_status_list* raw = nullptr;
    REQUIRE_GIT(git_status_list_new(&raw, repo.get(), &options));
    return git_status_list_entrycount(StatusList(raw).get());
}

// İki klon ortak bir başlangıçta: `files` A'da yazılır, A ve B senkronlanır. `base`: o anki içerikler.
struct World {
    fs::path a;
    fs::path b;
    std::set<std::string> base;
};

World start(const SyncFixture& fx, std::initializer_list<std::pair<std::string_view, std::string>> files) {
    World world{fx.make_clone(u8"A notları"), fx.make_clone(u8"B notları"), {}};
    for (const auto& [path, content] : files)
        write_file(world.a / note(path), content);
    sync_ok(world.a);
    sync_ok(world.b);
    world.base = working_contents(world.a);
    return world;
}

// Değişmez (her çakışma testinde): iki tarafın başlangıçtan sonra yazdığı her içerik birleşmiş commit'te ve
// diskte bir yerde durur; depo temiz (yarım merge yok, commit'lenmemiş dosya yok).
void check_nothing_lost(const fs::path& merged, const std::set<std::string>& base,
                        const std::set<std::string>& ours, const std::set<std::string>& theirs) {
    const std::set<std::string> committed = committed_contents(merged);
    const std::set<std::string> on_disk = working_contents(merged);
    for (const std::set<std::string>* side : {&ours, &theirs})
        for (const std::string& content : *side) {
            if (base.contains(content))
                continue;  // değişmemiş içerik: diğer tarafın düzenlemesi onun yerini alabilir
            CHECK_MESSAGE(committed.contains(content), content);
            CHECK_MESSAGE(on_disk.contains(content), content);
        }
    CHECK(repository_state(merged) == GIT_REPOSITORY_STATE_NONE);
    CHECK(uncommitted_count(merged) == 0);
}

// Çakışma kopyası adındaki yerel saat damgası: "YYYY-MM-DD HHMM".
std::string stamp(Clock::time_point when) {
    const std::time_t time = Clock::to_time_t(when);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    return std::format("{:04}-{:02}-{:02} {:02}{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                       local.tm_hour, local.tm_min);
}

std::string copy_name(std::string_view stem, std::string_view ext, Clock::time_point when,
                      std::string_view number = "") {
    return std::format("{} (conflict {}){}{}", stem, stamp(when), number, ext);
}

// Diskteki (.git hariç) "(conflict …)" kopyalarının adları.
std::set<std::string> copies_on_disk(const fs::path& dir) {
    std::set<std::string> names;
    for (auto it = fs::recursive_directory_iterator(dir); it != fs::recursive_directory_iterator(); ++it) {
        if (it->path().filename() == ".git")
            it.disable_recursion_pending();
        else if (utf8(it->path().filename()).find(" (conflict ") != std::string::npos)
            names.insert(utf8(it->path().filename()));
    }
    return names;
}

Error injected_failure() {
    return Error{Error::Code::Io, "error.io", {{"path", "enjekte"}, {"detail", "disk dolu"}}};
}

// Senkron sırasında dakika dönebilir: damga başlangıç ya da bitiş dakikasından biri.
bool is_copy(const std::string& actual, std::string_view stem, std::string_view ext, Clock::time_point t0,
             Clock::time_point t1, std::string_view number = "") {
    return actual == copy_name(stem, ext, t0, number) || actual == copy_name(stem, ext, t1, number);
}

}  // namespace

TEST_CASE("acceptance 2: changes to different notes merge both ways without conflict copies") {
    const SyncFixture fx;
    const World w = start(
        fx,
        {{"a.md", "a ortak\n"}, {"b.md", "b ortak\n"}, {"silinecek.md", "eski\n"}, {"ayni.md", "ortak\n"}});
    write_file(w.b / "b.md", "B değiştirdi\n");
    write_file(w.b / "yeni-b.md", "B ekledi\n");
    write_file(w.b / "ayni.md", "iki taraf da aynısını yazdı\n");
    sync_ok(w.b);

    write_file(w.a / "a.md", "A değiştirdi\n");
    write_file(w.a / "ayni.md", "iki taraf da aynısını yazdı\n");  // aynı içerik çakışma değildir
    fs::remove(w.a / "silinecek.md");
    const auto ours = working_contents(w.a);
    const auto theirs = working_contents(w.b);
    const SyncResult merged = sync_ok(w.a);
    CHECK(merged.conflict_copies.empty());
    CHECK(merged.pushed);
    CHECK(merged.pulled > 0);
    CHECK(head_commit(w.a).parents == 2);
    CHECK(head_commit(w.a).message.find('{') == std::string::npos);
    CHECK(ref_id(fx.remote(), "refs/heads/main") == ref_id(w.a, "HEAD"));
    check_nothing_lost(w.a, w.base, ours, theirs);

    const SyncResult pulled = sync_ok(w.b);
    CHECK(pulled.conflict_copies.empty());
    for (const fs::path& side : {w.a, w.b}) {
        CAPTURE(utf8(side));
        CHECK(read_file(side / "a.md") == "A değiştirdi\n");
        CHECK(read_file(side / "b.md") == "B değiştirdi\n");
        CHECK(read_file(side / "yeni-b.md") == "B ekledi\n");
        CHECK(read_file(side / "ayni.md") == "iki taraf da aynısını yazdı\n");
        CHECK_FALSE(fs::exists(side / "silinecek.md"));
    }
    CHECK(ref_id(w.a, "HEAD") == ref_id(w.b, "HEAD"));
}

TEST_CASE("acceptance 3: the same note changed on both sides keeps ours in place and theirs in a copy") {
    const SyncFixture fx;
    const World w = start(fx, {{"not.md", "ortak\n"}});
    write_file(w.b / "not.md", "B sürümü\n");
    sync_ok(w.b);

    write_file(w.a / "not.md", "A sürümü\n");
    const auto ours = working_contents(w.a);
    const auto theirs = working_contents(w.b);
    const auto t0 = Clock::now();
    const SyncResult result = sync_ok(w.a);
    const auto t1 = Clock::now();

    REQUIRE(result.conflict_copies.size() == 1);
    const std::string copy = result.conflict_copies[0].string();
    CHECK_MESSAGE(is_copy(copy, "not", ".md", t0, t1), copy);
    CHECK(read_file(w.a / "not.md") == "A sürümü\n");
    CHECK(read_file(w.a / note(copy)) == "B sürümü\n");
    CHECK(result.pushed);
    CHECK(head_commit(w.a).parents == 2);
    CHECK(ref_id(fx.remote(), "refs/heads/main") == ref_id(w.a, "HEAD"));
    CHECK(read_file(noteapp::core::state_dir() / "log.txt").find(copy) != std::string::npos);
    check_nothing_lost(w.a, w.base, ours, theirs);

    // B iki dosyayı da çeker: kendi sürümü kaybolmaz, kopyada durur.
    const SyncResult pulled = sync_ok(w.b);
    CHECK(pulled.conflict_copies.empty());
    CHECK(read_file(w.b / "not.md") == "A sürümü\n");
    CHECK(read_file(w.b / note(copy)) == "B sürümü\n");
    CHECK(ref_id(w.a, "HEAD") == ref_id(w.b, "HEAD"));
}

TEST_CASE("acceptance 4: delete/modify in both directions keeps the modified version, deletes nothing") {
    const SyncFixture fx;
    const World w = start(fx, {{"x.md", "x ortak\n"}, {"y.md", "y ortak\n"}});
    write_file(w.b / "x.md", "B x'i değiştirdi\n");
    fs::remove(w.b / "y.md");
    sync_ok(w.b);

    fs::remove(w.a / "x.md");                         // biz sildik, onlar değiştirdi
    write_file(w.a / "y.md", "A y'yi değiştirdi\n");  // biz değiştirdik, onlar sildi
    const auto ours = working_contents(w.a);
    const auto theirs = working_contents(w.b);
    const SyncResult result = sync_ok(w.a);
    CHECK(result.conflict_copies.empty());
    CHECK(result.pushed);
    CHECK(head_commit(w.a).parents == 2);
    CHECK(read_file(w.a / "x.md") == "B x'i değiştirdi\n");
    CHECK(read_file(w.a / "y.md") == "A y'yi değiştirdi\n");
    check_nothing_lost(w.a, w.base, ours, theirs);

    sync_ok(w.b);
    CHECK(read_file(w.b / "x.md") == "B x'i değiştirdi\n");
    CHECK(read_file(w.b / "y.md") == "A y'yi değiştirdi\n");
}

TEST_CASE("add/add with different content: ours at the path, theirs in a conflict copy") {
    const SyncFixture fx;
    const World w = start(fx, {{"ortak.md", "ortak\n"}});
    write_file(w.b / "yeni.md", "B'nin yeni notu\n");
    sync_ok(w.b);
    write_file(w.a / "yeni.md", "A'nın yeni notu\n");
    const auto ours = working_contents(w.a);
    const auto theirs = working_contents(w.b);
    const auto t0 = Clock::now();
    const SyncResult result = sync_ok(w.a);
    const auto t1 = Clock::now();
    REQUIRE(result.conflict_copies.size() == 1);
    const std::string copy = result.conflict_copies[0].string();
    CHECK_MESSAGE(is_copy(copy, "yeni", ".md", t0, t1), copy);
    CHECK(read_file(w.a / "yeni.md") == "A'nın yeni notu\n");
    CHECK(read_file(w.a / note(copy)) == "B'nin yeni notu\n");
    check_nothing_lost(w.a, w.base, ours, theirs);
}

TEST_CASE("binary conflict: both versions byte-exact") {
    const SyncFixture fx;
    const std::string base = "\x89PNG\r\n\x1a\n"s + '\0' + "taban" + '\0';
    const std::string from_a = "\x89PNG\r\n\x1a\n"s + '\0' + "\xff\xfe" + "A" + '\0' + "\r\n";
    const std::string from_b = "\x89PNG\r\n\x1a\n"s + '\0' + "\x01\x02" + "B" + '\0' + "\n";
    const World w = start(fx, {{"resim.png", base}});
    write_file(w.b / "resim.png", from_b);
    sync_ok(w.b);
    write_file(w.a / "resim.png", from_a);
    const auto ours = working_contents(w.a);
    const auto theirs = working_contents(w.b);
    const auto t0 = Clock::now();
    const SyncResult result = sync_ok(w.a);
    const auto t1 = Clock::now();
    REQUIRE(result.conflict_copies.size() == 1);
    const std::string copy = result.conflict_copies[0].string();
    CHECK_MESSAGE(is_copy(copy, "resim", ".png", t0, t1), copy);
    CHECK(read_file(w.a / "resim.png") == from_a);
    CHECK(read_file(w.a / note(copy)) == from_b);
    check_nothing_lost(w.a, w.base, ours, theirs);

    sync_ok(w.b);
    CHECK(read_file(w.b / "resim.png") == from_a);
    CHECK(read_file(w.b / note(copy)) == from_b);
}

TEST_CASE("several conflicts in one sync each get a copy; non-conflicting changes merge alongside") {
    const SyncFixture fx;
    const World w = start(fx, {{"bir.md", "1 ortak\n"}, {"iki.txt", "2 ortak\n"}, {"uc.md", "3 ortak\n"}});
    write_file(w.b / "bir.md", "1 B\n");
    write_file(w.b / "iki.txt", "2 B\n");
    write_file(w.b / "uc.md", "3 B\n");
    sync_ok(w.b);
    write_file(w.a / "bir.md", "1 A\n");
    write_file(w.a / "iki.txt", "2 A\n");
    write_file(w.a / "dort.md", "4 A\n");
    const auto ours = working_contents(w.a);
    const auto theirs = working_contents(w.b);
    const auto t0 = Clock::now();
    const SyncResult result = sync_ok(w.a);
    const auto t1 = Clock::now();

    REQUIRE(result.conflict_copies.size() == 2);
    const std::string first = result.conflict_copies[0].string();
    const std::string second = result.conflict_copies[1].string();
    CHECK_MESSAGE(is_copy(first, "bir", ".md", t0, t1), first);
    CHECK_MESSAGE(is_copy(second, "iki", ".txt", t0, t1), second);
    CHECK(read_file(w.a / "bir.md") == "1 A\n");
    CHECK(read_file(w.a / note(first)) == "1 B\n");
    CHECK(read_file(w.a / "iki.txt") == "2 A\n");
    CHECK(read_file(w.a / note(second)) == "2 B\n");
    CHECK(read_file(w.a / "uc.md") == "3 B\n");
    CHECK(read_file(w.a / "dort.md") == "4 A\n");
    check_nothing_lost(w.a, w.base, ours, theirs);
}

TEST_CASE("a conflict copy never overwrites an existing file: the name gets a number") {
    const SyncFixture fx;
    const World w = start(fx, {{"not.md", "ortak\n"}});
    write_file(w.b / "not.md", "B\n");
    sync_ok(w.b);

    // Senkron bu dakikada ya da bir sonrakinde koşar: iki dakikanın adı da önceden dolu.
    const auto now = Clock::now();
    const std::string taken_now = copy_name("not", ".md", now);
    const std::string taken_next = copy_name("not", ".md", now + std::chrono::minutes(1));
    write_file(w.a / note(taken_now), "eski kopya 1\n");
    write_file(w.a / note(taken_next), "eski kopya 2\n");
    write_file(w.a / "not.md", "A\n");
    const auto ours = working_contents(w.a);
    const auto theirs = working_contents(w.b);
    const auto t0 = Clock::now();
    const SyncResult result = sync_ok(w.a);
    const auto t1 = Clock::now();

    REQUIRE(result.conflict_copies.size() == 1);
    const std::string copy = result.conflict_copies[0].string();
    CHECK_MESSAGE(is_copy(copy, "not", ".md", t0, t1, " 2"), copy);
    CHECK(read_file(w.a / note(copy)) == "B\n");
    CHECK(read_file(w.a / note(taken_now)) == "eski kopya 1\n");
    CHECK(read_file(w.a / note(taken_next)) == "eski kopya 2\n");
    CHECK(read_file(w.a / "not.md") == "A\n");
    check_nothing_lost(w.a, w.base, ours, theirs);
}

TEST_CASE("CRLF content survives conflicts byte-exact despite a global core.autocrlf=true") {
    const SyncFixture fx;  // global config'te autocrlf=true
    const World w = start(fx, {{"satir.txt", "satır 1\r\northak\r\n"}, {"uzantisiz", "ortak\r\n"}});
    write_file(w.b / "satir.txt", "satır 1\r\nB\r\n");
    write_file(w.b / "uzantisiz", "B\r\n");
    sync_ok(w.b);
    write_file(w.a / "satir.txt", "satır 1\r\nA\r\n");
    write_file(w.a / "uzantisiz", "A\r\n");
    const auto ours = working_contents(w.a);
    const auto theirs = working_contents(w.b);
    const auto t0 = Clock::now();
    const SyncResult result = sync_ok(w.a);
    const auto t1 = Clock::now();

    REQUIRE(result.conflict_copies.size() == 2);
    const std::string satir = result.conflict_copies[0].string();
    const std::string uzantisiz = result.conflict_copies[1].string();
    CHECK_MESSAGE(is_copy(satir, "satir", ".txt", t0, t1), satir);
    CHECK_MESSAGE(is_copy(uzantisiz, "uzantisiz", "", t0, t1), uzantisiz);  // uzantısız: nokta yok
    CHECK(read_file(w.a / "satir.txt") == "satır 1\r\nA\r\n");
    CHECK(read_file(w.a / note(satir)) == "satır 1\r\nB\r\n");
    CHECK(read_file(w.a / "uzantisiz") == "A\r\n");
    CHECK(read_file(w.a / note(uzantisiz)) == "B\r\n");
    check_nothing_lost(w.a, w.base, ours, theirs);

    sync_ok(w.b);
    CHECK(read_file(w.b / "satir.txt") == "satır 1\r\nA\r\n");
    CHECK(read_file(w.b / note(satir)) == "satır 1\r\nB\r\n");
}

// Çakışma değil: aynı notta örtüşmeyen düzenlemeler tek dosyada birleşir (iki taraf da tek içerikte durur,
// bu yüzden bütün-dosya değişmezi burada uygulanmaz); satır sonlarına dokunulmaz.
TEST_CASE("non-overlapping edits of the same note merge into one file, CRLF kept") {
    const SyncFixture fx;  // global config'te autocrlf=true
    const World w = start(fx, {{"birlesen.txt", "1\r\n2\r\n3\r\n4\r\n5\r\n"}});
    write_file(w.b / "birlesen.txt", "1\r\n2\r\n3\r\n4\r\n5 B\r\n");
    sync_ok(w.b);
    write_file(w.a / "birlesen.txt", "1 A\r\n2\r\n3\r\n4\r\n5\r\n");
    const SyncResult result = sync_ok(w.a);
    CHECK(result.conflict_copies.empty());
    CHECK(head_commit(w.a).parents == 2);
    CHECK(read_file(w.a / "birlesen.txt") == "1 A\r\n2\r\n3\r\n4\r\n5 B\r\n");
    CHECK(uncommitted_count(w.a) == 0);

    sync_ok(w.b);
    CHECK(read_file(w.b / "birlesen.txt") == "1 A\r\n2\r\n3\r\n4\r\n5 B\r\n");
}

TEST_CASE("conflicts on Turkish file names in a Turkish folder") {
    const SyncFixture fx;
    const World w = start(fx, {{"klasör/çalışma notu.md", "ortak\n"}});
    write_file(w.b / note("klasör/çalışma notu.md"), "B: ğüşiöç\n");
    sync_ok(w.b);
    write_file(w.a / note("klasör/çalışma notu.md"), "A: ĞÜŞİÖÇ\n");
    const auto ours = working_contents(w.a);
    const auto theirs = working_contents(w.b);
    const auto t0 = Clock::now();
    const SyncResult result = sync_ok(w.a);
    const auto t1 = Clock::now();

    REQUIRE(result.conflict_copies.size() == 1);
    const std::string copy = result.conflict_copies[0].string();
    CHECK_MESSAGE(is_copy(copy, "klasör/çalışma notu", ".md", t0, t1), copy);
    CHECK(read_file(w.a / note("klasör/çalışma notu.md")) == "A: ĞÜŞİÖÇ\n");
    CHECK(read_file(w.a / note(copy)) == "B: ğüşiöç\n");
    check_nothing_lost(w.a, w.base, ours, theirs);

    sync_ok(w.b);
    CHECK(read_file(w.b / note(copy)) == "B: ğüşiöç\n");
}

TEST_CASE("acceptance 5: the remote advancing between fetch and push is merged and pushed on the retry") {
    const SyncFixture fx;
    const World w = start(fx, {{"ortak.md", "ortak\n"}});
    write_file(w.a / "a.md", "A\n");
    int calls = 0;
    SyncTestHooks hooks;
    hooks.before_push = [&] {
        if (++calls > 1)
            return;
        // A fetch'ledi ama henüz push'lamadı: B tam şimdi kendi commit'ini gönderir. Ayrı NOTEAPP_HOME:
        // B, A'nın tuttuğu senkron kilidine takılmasın (gerçekte ayrı makine).
        const EnvGuard other_machine("NOTEAPP_HOME", fx.root() / "app-b");
        write_file(w.b / "b.md", "B\n");
        sync_ok(w.b);
    };
    const auto result = SyncService(SyncFixture::config_for(w.a)).sync_now(hooks);
    REQUIRE_MESSAGE(result.has_value(), describe(result));
    CHECK(calls == 2);  // ilk push reddedildi, ikinci tur birleştirip gönderdi
    CHECK(result->pushed);
    CHECK(result->pulled == 1);
    CHECK(head_commit(w.a).parents == 2);
    CHECK(read_file(w.a / "b.md") == "B\n");
    CHECK(ref_id(fx.remote(), "refs/heads/main") == ref_id(w.a, "HEAD"));

    sync_ok(w.b);
    CHECK(read_file(w.b / "a.md") == "A\n");
}

TEST_CASE("an unfinished manual merge/rebase is sync.repo_busy_state and nothing is touched") {
    const SyncFixture fx;
    const World w = start(fx, {{"not.md", "ortak\n"}});
    const auto head = ref_id(w.a, "HEAD");
    const auto remote_head = ref_id(fx.remote(), "refs/heads/main");
    write_file(w.a / ".git" / "MERGE_HEAD", *head + "\n");  // elle başlatılmış, bitmemiş merge
    write_file(w.a / "not.md", "yarım iş\n");

    const auto result = try_sync(w.a);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.repo_busy_state");
    CHECK(ref_id(w.a, "HEAD") == head);
    CHECK(ref_id(fx.remote(), "refs/heads/main") == remote_head);
    CHECK(read_file(w.a / "not.md") == "yarım iş\n");
    CHECK(fs::exists(w.a / ".git" / "MERGE_HEAD"));

    fs::remove(w.a / ".git" / "MERGE_HEAD");  // kullanıcı işini bitirdi
    CHECK(sync_ok(w.a).pushed);
}

TEST_CASE("unrelated histories are sync.unrelated_histories and nothing is merged") {
    const SyncFixture fx;
    const fs::path a = fx.make_clone(u8"A");
    write_file(a / "a.md", "A\n");
    sync_ok(a);
    const auto remote_head = ref_id(fx.remote(), "refs/heads/main");

    const fs::path b = fx.make_clone(u8"B");  // kendi kökünden başlayan geçmiş
    write_file(b / "b.md", "B\n");
    const auto result = try_sync(b);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message_key == "sync.unrelated_histories");
    CHECK(read_file(b / "b.md") == "B\n");
    CHECK_FALSE(fs::exists(b / "a.md"));
    CHECK(commit_count(b) == 1);
    CHECK(repository_state(b) == GIT_REPOSITORY_STATE_NONE);
    CHECK(ref_id(fx.remote(), "refs/heads/main") == remote_head);
}

// Dosya adı sınırı 255 bayt (Linux); "ç" 2 bayttır ama Windows'ta tek UTF-16 birimi, bu yüzden yol MAX_PATH'i
// aşmaz. Kesme karakter ortasına denk gelsin: damgadan sonra kök için 225 (tek sayı) bayt kalır.
TEST_CASE("a conflict copy's name fits 255 bytes: the stem is cut on a UTF-8 character boundary") {
    const SyncFixture fx;
    const auto stem_of = [](int characters) {
        std::string text;
        for (int i = 0; i < characters; ++i)
            text += "ç";
        return text;
    };
    const std::string name = stem_of(123) + "a.md";  // 250 bayt

    SUBCASE("the free name") {
        const World w = start(fx, {{name, "ortak\n"}});
        write_file(w.b / note(name), "B\n");
        sync_ok(w.b);
        write_file(w.a / note(name), "A\n");
        const auto t0 = Clock::now();
        const SyncResult result = sync_ok(w.a);
        const auto t1 = Clock::now();
        REQUIRE(result.conflict_copies.size() == 1);
        const std::string copy = result.conflict_copies[0].string();
        CHECK(copy.size() <= 255);
        CHECK_MESSAGE(is_copy(copy, stem_of(112), ".md", t0, t1), copy);
        CHECK(read_file(w.a / note(copy)) == "B\n");
    }
    SUBCASE("the number also fits: the stem gets shorter") {
        const World w = start(fx, {{name, "ortak\n"}});
        write_file(w.b / note(name), "B\n");
        sync_ok(w.b);
        const auto now = Clock::now();
        write_file(w.a / note(copy_name(stem_of(112), ".md", now)), "eski 1\n");
        write_file(w.a / note(copy_name(stem_of(112), ".md", now + std::chrono::minutes(1))), "eski 2\n");
        write_file(w.a / note(name), "A\n");
        const auto t0 = Clock::now();
        const SyncResult result = sync_ok(w.a);
        const auto t1 = Clock::now();
        REQUIRE(result.conflict_copies.size() == 1);
        const std::string copy = result.conflict_copies[0].string();
        CHECK(copy.size() <= 255);
        CHECK_MESSAGE(is_copy(copy, stem_of(111), ".md", t0, t1, " 2"), copy);
        CHECK(read_file(w.a / note(copy)) == "B\n");
    }
    SUBCASE("a huge extension is treated as part of the name") {
        std::string huge = "x.";
        huge += stem_of(125);  // 252 bayt; uzantı tek başına sınırı doldururdu
        const World w = start(fx, {{huge, "ortak\n"}});
        write_file(w.b / note(huge), "B\n");
        sync_ok(w.b);
        write_file(w.a / note(huge), "A\n");
        const SyncResult result = sync_ok(w.a);
        REQUIRE(result.conflict_copies.size() == 1);
        const std::string copy = result.conflict_copies[0].string();
        CHECK(copy.size() <= 255);
        CHECK(read_file(w.a / note(copy)) == "B\n");
    }
}

// Merge başladıktan sonraki bir hata merge'ü geri alır (SPEC-sync karar 9): merge'den önce tüm yerel
// değişiklikler commit'lendi, HEAD ve uzak dal her baytı tutar. Depo MERGE durumunda kalsaydı her senkron
// sync.repo_busy_state ile dururdu.
TEST_CASE("a failure while writing conflict copies undoes the merge; the next sync makes the copies") {
    const SyncFixture fx;
    const World w = start(fx, {{"bir.md", "1 ortak\n"}, {"iki.md", "2 ortak\n"}, {"uc.md", "3 ortak\n"}});
    write_file(w.b / "bir.md", "1 B\n");
    write_file(w.b / "iki.md", "2 B\n");
    write_file(w.b / "uc.md", "3 B\n");  // çakışmasız: merge yazar, geri alma HEAD'e döndürür
    sync_ok(w.b);
    write_file(w.a / "bir.md", "1 A\n");
    write_file(w.a / "iki.md", "2 A\n");
    const auto ours = working_contents(w.a);
    const auto theirs = working_contents(w.b);

    int written = 0;
    SyncTestHooks hooks;
    hooks.after_conflict_copy = [&](const std::string&) -> std::expected<void, Error> {
        if (++written < 2)
            return {};
        return std::unexpected(injected_failure());
    };
    const auto failed = SyncService(SyncFixture::config_for(w.a)).sync_now(hooks);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().message_key == "error.io");
    CHECK(written == 2);
    CHECK(repository_state(w.a) == GIT_REPOSITORY_STATE_NONE);
    CHECK(head_commit(w.a).parents == 1);  // A'nın kendi commit'i; merge commit'i yok
    CHECK(uncommitted_count(w.a) == 0);    // index ve çalışma ağacı HEAD'de
    CHECK(copies_on_disk(w.a).empty());
    CHECK(read_file(w.a / "bir.md") == "1 A\n");
    CHECK(read_file(w.a / "uc.md") == "3 ortak\n");

    const SyncResult retried = sync_ok(w.a);
    CHECK(retried.conflict_copies.size() == 2);
    CHECK(read_file(w.a / "uc.md") == "3 B\n");
    CHECK(head_commit(w.a).parents == 2);
    check_nothing_lost(w.a, w.base, ours, theirs);
}

TEST_CASE("a note the user edits while a merge fails is left as edited when the merge is undone") {
    const SyncFixture fx;
    const World w = start(fx, {{"not.md", "ortak\n"}, {"b.md", "b ortak\n"}});
    write_file(w.b / "not.md", "B\n");
    write_file(w.b / "b.md", "b B\n");
    sync_ok(w.b);
    write_file(w.a / "not.md", "A\n");
    const auto theirs = working_contents(w.b);

    SyncTestHooks hooks;
    hooks.after_conflict_copy = [&](const std::string&) -> std::expected<void, Error> {
        // Kullanıcı tam o an iki notu düzenler: merge'ün B sürümünü yazdığı b.md'yi ve çakışan not.md'yi.
        write_file(w.a / "b.md", "kullanıcı arada yazdı\n");
        write_file(w.a / "not.md", "A, arada düzenlendi\n");
        return std::unexpected(injected_failure());
    };
    REQUIRE_FALSE(SyncService(SyncFixture::config_for(w.a)).sync_now(hooks).has_value());
    CHECK(repository_state(w.a) == GIT_REPOSITORY_STATE_NONE);
    CHECK(read_file(w.a / "b.md") == "kullanıcı arada yazdı\n");
    CHECK(read_file(w.a / "not.md") == "A, arada düzenlendi\n");
    CHECK(copies_on_disk(w.a).empty());

    const auto ours = working_contents(w.a);
    sync_ok(w.a);
    check_nothing_lost(w.a, w.base, ours, theirs);
}

TEST_CASE(
    "a sync killed in the middle of a conflicted merge is undone by the next sync, which merges again") {
    const SyncFixture fx;
    const World w = start(fx, {{"not.md", "ortak\n"}, {"b.md", "b ortak\n"}});
    write_file(w.b / "not.md", "B\n");
    write_file(w.b / "b.md", "b B\n");
    sync_ok(w.b);
    write_file(w.a / "not.md", "A\n");
    const auto ours = working_contents(w.a);
    const auto theirs = working_contents(w.b);

    // Süreç ilk kopyayı yazdıktan hemen sonra öldürülür: geri alma çalışmaz, kilit sonra bayat kalır.
    SyncTestHooks hooks;
    hooks.after_conflict_copy = [](const std::string&) -> std::expected<void, Error> {
        throw std::runtime_error("süreç öldürüldü");
    };
    CHECK_THROWS(static_cast<void>(SyncService(SyncFixture::config_for(w.a)).sync_now(hooks)));
    REQUIRE(repository_state(w.a) == GIT_REPOSITORY_STATE_MERGE);
    write_file(noteapp::core::state_dir() / "sync.lock", "999999999\n2026-10-28T00:00:00Z\n00000000\n");

    const SyncResult recovered = sync_ok(w.a);
    CHECK_FALSE(recovered.conflict_copies.empty());
    CHECK(head_commit(w.a).parents == 2);
    check_nothing_lost(w.a, w.base, ours, theirs);
}
