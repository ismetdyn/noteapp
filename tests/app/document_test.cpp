// A5: TUI ve GUI'nin ortak belge kuralı. Salt-okunur nedeni, bayt koruması (BOM, CRLF), kayıt ve çakışma
// seçenekleri, dış değişiklik kararı, yeni notun klasörü ve otomatik kayıt zamanlaması burada; arayüz
// testleri yalnız bağlantıyı sınar.
#include "app/document.hpp"
#include "core/i18n.hpp"
#include "support/note_seed.hpp"
#include "support/read_only_fixtures.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <string>

using noteapp::app::Autosave;
using noteapp::app::DiskChange;
using noteapp::app::Document;
using noteapp::app::DocumentOptions;
using noteapp::app::ReadOnly;
using noteapp::core::NotePath;
using noteapp::core::NoteStore;

namespace {

constexpr std::size_t kLimit = 16 * 1024;
constexpr std::string_view kBom = "\xEF\xBB\xBF";

NotePath path(const std::string& text) {
    return *NotePath::parse(text);
}

struct Scene {
    Scene() { noteapp::core::init_i18n("en"); }
    noteapp::test::TempDir dir;
    NoteStore store{dir.path()};

    void put(const std::string& note, std::string_view bytes) {
        noteapp::test::upsert_note(dir.path(), note, bytes);
    }
    std::string bytes(const std::string& note) const { return noteapp::test::note_body(dir.path(), note); }
    bool exists(const std::string& note) const { return noteapp::test::note_exists(dir.path(), note); }
};

}  // namespace

TEST_CASE("document: read-only reasons are the same rule for every fixture and the limit is inclusive") {
    for (const auto& item : noteapp::test::read_only_cases(kLimit)) {
        const std::string name = item.name;
        CAPTURE(name);
        CHECK(noteapp::app::classify(item.bytes, kLimit) == item.reason);
        CHECK_FALSE(noteapp::app::read_only_reason(item.reason, kLimit).empty());
    }
    CHECK(noteapp::app::classify(std::string("ikili\0dosya", 11), kLimit) == ReadOnly::Binary);
    CHECK(noteapp::app::classify(std::string(kLimit, 'x'), kLimit) == ReadOnly::No);
    CHECK(noteapp::app::read_only_reason(ReadOnly::No, kLimit).empty());
}

TEST_CASE("document: a read-only note shows sanitized text, never writes, and a large one shows its start") {
    Scene s;
    s.put("big.txt", std::string(kLimit + 1, 'x'));
    s.put("bad.txt", "a\xFF");
    Document big(DocumentOptions{kLimit, false});
    CHECK(big.open(s.store, path("big.txt")).value().size() == kLimit);
    CHECK(big.read_only() == ReadOnly::TooLarge);
    CHECK_FALSE(big.autosaves());
    Document bad(DocumentOptions{kLimit, false});
    CHECK(bad.open(s.store, path("bad.txt")).value() == "a\xEF\xBF\xBD");
    CHECK_FALSE(bad.writable());
}

TEST_CASE("document: bytes survive open and save in both editor kinds") {
    Scene s;
    const std::string crlf = std::string(kBom) + "a\r\nb\r\n";
    const std::string mixed = "a\r\nb\nc\rd";
    s.put("crlf.txt", crlf);
    s.put("mixed.txt", mixed);
    for (const bool lf_editor : {false, true}) {
        CAPTURE(lf_editor);
        Document doc(DocumentOptions{kLimit, lf_editor});
        const std::string text = doc.open(s.store, path("crlf.txt")).value();
        CHECK(text == (lf_editor ? "a\nb\n" : "a\r\nb\r\n"));  // BOM tamponda görünmez
        CHECK(doc.has_bom());
        CHECK(doc.encode(text) == crlf);
        Document other(DocumentOptions{kLimit, lf_editor});
        const std::string shown = other.open(s.store, path("mixed.txt")).value();
        CHECK(shown == mixed);  // karışık satır sonu LF'ye indirilmez: geri açmak baytları bozardı
        CHECK(other.encode(shown) == mixed);
    }
}

TEST_CASE("document: save writes once, a conflict and a vanished note write nothing") {
    Scene s;
    s.put("a.md", "one");
    Document doc;
    REQUIRE(doc.open(s.store, path("a.md")));
    CHECK(doc.save(s.store, "two") == Document::Saved::Written);
    CHECK(doc.save(s.store, "three") == Document::Saved::Written);  // kendi kaydıyla çakışmaz
    CHECK(s.bytes("a.md") == "three");
    s.put("a.md", "outside");
    CHECK(doc.save(s.store, "mine") == Document::Saved::Conflict);
    CHECK(s.bytes("a.md") == "outside");
    noteapp::test::erase_note(s.dir.path(), "a.md");
    CHECK(doc.save(s.store, "mine") == Document::Saved::Gone);
    CHECK_FALSE(s.exists("a.md"));
}

TEST_CASE("document: conflict choices keep both versions, reload or overwrite on purpose") {
    Scene s;
    s.put("work/a.tar.gz", std::string(kBom) + "x\r\n");
    Document doc(DocumentOptions{kLimit, true});
    REQUIRE(doc.open(s.store, path("work/a.tar.gz")));
    s.put("work/a.tar.gz", "outside");
    REQUIRE(doc.save(s.store, "mine\n") == Document::Saved::Conflict);

    Document copy = doc;
    const auto made = copy.create_beside(s.store, "mine\n", noteapp::app::kCopySuffix);
    REQUIRE(made);
    CHECK(made->string() == "work/a.tar (copy).gz");
    CHECK(s.bytes("work/a.tar (copy).gz") == std::string(kBom) + "mine\r\n");  // baytlar aynen
    CHECK(copy.path() == made.value());
    CHECK(s.bytes("work/a.tar.gz") == "outside");

    Document reload = doc;
    CHECK(reload.reload(s.store).value() == "outside");
    CHECK_FALSE(reload.has_bom());

    REQUIRE(doc.overwrite(s.store, "mine\n"));
    CHECK(s.bytes("work/a.tar.gz") == std::string(kBom) + "mine\r\n");
    CHECK(noteapp::app::kDefaultConflictChoice == noteapp::app::ConflictChoice::SaveCopy);
}

TEST_CASE("document: a draft is created with its exact bytes and becomes the note") {
    Scene s;
    Document doc;
    CHECK(doc.is_draft());
    CHECK_FALSE(doc.autosaves());  // ilk kayıt ad ister
    const auto made = doc.create(s.store, noteapp::core::NewNote{"f", "n", "txt", {}}, "a\r\nb");
    REQUIRE(made);
    CHECK(s.bytes("f/n.txt") == "a\r\nb");
    CHECK(doc.path() == path("f/n.txt"));
    CHECK(doc.autosaves());
    CHECK(doc.save(s.store, "c") == Document::Saved::Written);
}

TEST_CASE("document: disk changes reload a clean buffer and never touch a dirty one") {
    const noteapp::core::Revision mine{};
    noteapp::core::Revision other{};
    other.seq = 1;
    CHECK(noteapp::app::disk_change(mine, mine, true) == DiskChange::None);
    CHECK(noteapp::app::disk_change(other, mine, false) == DiskChange::Reload);
    CHECK(noteapp::app::disk_change(other, mine, true) == DiskChange::Changed);
    CHECK(noteapp::app::disk_change(std::nullopt, mine, true) == DiskChange::Deleted);
    CHECK(noteapp::app::disk_change(std::nullopt, mine, false) == DiskChange::Close);

    Scene s;
    s.put("a.md", "x");
    Document doc;
    REQUIRE(doc.open(s.store, path("a.md")));
    doc.note_disk_change(DiskChange::Changed);
    CHECK(doc.changed_on_disk());
    CHECK_FALSE(doc.autosaves());  // otomatik kayıt sormaz: açık kayda bırakır
    doc.note_disk_change(DiskChange::Deleted);
    CHECK(doc.deleted_on_disk());
    CHECK_FALSE(doc.changed_on_disk());
    doc.note_disk_change(DiskChange::None);
    CHECK_FALSE(doc.deleted_on_disk());
}

TEST_CASE(
    "document: the new note folder prefers the folder row, then the scope, then the note, then config") {
    const std::string row = "row";
    const NotePath note = path("notes/a.md");
    const NotePath at_root = path("a.md");
    using noteapp::app::FolderContext;
    using noteapp::app::new_note_folder;
    CHECK(new_note_folder(FolderContext{&row, "scope", &note, "inbox"}) == "row");
    CHECK(new_note_folder(FolderContext{nullptr, "scope", &note, "inbox"}) == "scope");
    CHECK(new_note_folder(FolderContext{nullptr, "", &note, "inbox"}) == "notes");
    CHECK(new_note_folder(FolderContext{nullptr, "", &at_root, "inbox"}) == "inbox");
    CHECK(new_note_folder(FolderContext{nullptr, "", nullptr, "inbox"}) == "inbox");
}

TEST_CASE("document: autosave restarts on every edit and fires only after the delay") {
    Autosave timer(std::chrono::milliseconds(60));
    const auto start = Autosave::Clock::now();
    CHECK_FALSE(timer.due(start));
    timer.edited(start);
    CHECK_FALSE(timer.due(start + std::chrono::milliseconds(59)));
    timer.edited(start + std::chrono::milliseconds(50));
    CHECK_FALSE(timer.due(start + std::chrono::milliseconds(100)));
    CHECK(timer.due(start + std::chrono::milliseconds(110)));
    timer.cancel();
    CHECK_FALSE(timer.deadline().has_value());
}
