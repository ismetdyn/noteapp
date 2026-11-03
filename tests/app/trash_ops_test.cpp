// A3: TUI ve GUI'nin ortak çöp kuralı. Geri alma yığını, klasörün çöpe atılması (kalıcı değil), onay
// politikası (varsayılan Vazgeç) ve toplu işlem raporu burada; arayüz testleri yalnız bağlantıyı sınar.
#include "app/listing.hpp"
#include "app/trash_ops.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <vector>

using noteapp::app::Confirm;
using noteapp::app::ConfirmKind;
using noteapp::app::Prompts;
using noteapp::app::TrashOps;
using noteapp::app::TrashTarget;
using noteapp::core::Limit;
using noteapp::core::NotePath;
using noteapp::core::NoteStore;

namespace {

NotePath path(const std::string& text) {
    return *NotePath::parse(text);
}

struct Scene {
    noteapp::test::TempDir dir;
    NoteStore store{dir.path()};
    std::vector<Confirm> asked;
    bool answer = false;
    Prompts prompts{[this](const Confirm& request) {
        asked.push_back(request);
        return answer;
    }};

    void put(const std::string& note) { REQUIRE(noteapp::test::seed_note(store, note, note)); }
    bool live(const std::string& note) const { return store.read(path(note)).has_value(); }
    bool trashed(const std::string& note) const { return store.read_trash(path(note)).has_value(); }
    bool folder(const std::string& name) const { return std::ranges::contains(store.folders(), name); }
    // Arayüzün yeniden taraması: klasör işlemleri bu listeye bakar.
    const noteapp::app::Listing& listing() {
        list.set_notes(store.list());
        list.set_folders(store.folders());
        return list;
    }
    // Yeniden taramasız: son listing() ne gösterdiyse (senkron arada not getirmiş olabilir).
    const noteapp::app::Listing& shown() const { return list; }

    noteapp::app::Listing list;
};

}  // namespace

TEST_CASE("undo is a stack: two trashed notes come back newest first, then nothing is left") {
    Scene s;
    s.put("a.md");
    s.put("b.md");
    TrashOps ops;
    REQUIRE(ops.trash_note(s.store, path("a.md")));
    REQUIRE(ops.trash_note(s.store, path("b.md")));
    CHECK(ops.undo_depth() == 2);

    auto first = ops.undo(s.store);
    REQUIRE(first);
    CHECK(first->single);
    REQUIRE(first->first);
    CHECK(first->first->string() == "b.md");
    CHECK(s.live("b.md"));
    CHECK(s.trashed("a.md"));

    auto second = ops.undo(s.store);
    REQUIRE(second);
    CHECK(s.live("a.md"));
    CHECK_FALSE(ops.can_undo());
    CHECK_FALSE(ops.undo(s.store));
}

TEST_CASE("the undo stack keeps the newest steps within limits.app.trash_undo_steps") {
    Scene s;
    for (const char* note : {"a.md", "b.md", "c.md"})
        s.put(note);
    TrashOps ops(Limit{2, "2"});
    for (const char* note : {"a.md", "b.md", "c.md"})
        REQUIRE(ops.trash_note(s.store, path(note)));
    CHECK(ops.undo_depth() == 2);
    REQUIRE(ops.undo(s.store));
    REQUIRE(ops.undo(s.store));
    CHECK_FALSE(ops.undo(s.store));
    CHECK(s.live("c.md"));
    CHECK(s.live("b.md"));
    CHECK(s.trashed("a.md"));  // en eski adım düştü; not çöpte duruyor, kaybolmadı

    TrashOps unlimited(Limit{0, "0"});
    for (const char* note : {"a.md", "b.md", "c.md"}) {
        if (s.trashed(note))
            REQUIRE(unlimited.restore(s.store, path(note)));
        REQUIRE(unlimited.trash_note(s.store, path(note)));
    }
    CHECK(unlimited.undo_depth() == 3);
}

TEST_CASE("a folder with notes goes to the trash, never permanently, and undo brings all of it back") {
    Scene s;
    s.put("work/a.md");
    s.put("work/sub/b.md");
    s.put("keep.md");
    REQUIRE(s.store.create_folder("work/empty"));
    TrashOps ops;

    // Varsayılan istem Vazgeç: hiçbir şey değişmez.
    CHECK_FALSE(ops.trash_folder(s.store, s.listing(), "work", Prompts{}));
    CHECK(s.live("work/a.md"));

    s.answer = true;
    const auto trashed = ops.trash_folder(s.store, s.listing(), "work", s.prompts);
    REQUIRE(trashed);
    CHECK_FALSE(trashed->failure);
    CHECK(trashed->notes == 2);
    REQUIRE(s.asked.size() == 1);
    CHECK(s.asked[0].kind == ConfirmKind::TrashFolder);
    CHECK(s.asked[0].path == "work");
    CHECK(s.asked[0].notes == 2);
    CHECK(s.asked[0].folders == 2);
    CHECK(s.trashed("work/a.md"));
    CHECK(s.trashed("work/sub/b.md"));
    CHECK_FALSE(s.folder("work"));
    CHECK(s.live("keep.md"));

    const auto undone = ops.undo(s.store);
    REQUIRE(undone);
    CHECK(undone->folder == "work");
    CHECK(undone->report.done == 2);
    CHECK(undone->report.lines.empty());
    CHECK(s.live("work/a.md"));
    CHECK(s.live("work/sub/b.md"));
    CHECK(s.folder("work/empty"));
    CHECK_FALSE(ops.can_undo());
}

TEST_CASE("a folder without notes is trashed without asking and undo recreates it") {
    Scene s;
    REQUIRE(s.store.create_folder("empty/inner"));
    TrashOps ops;
    const auto trashed = ops.trash_folder(s.store, s.listing(), "empty", s.prompts);
    REQUIRE(trashed);
    CHECK(s.asked.empty());
    CHECK_FALSE(s.folder("empty"));
    REQUIRE(ops.undo(s.store));
    CHECK(s.folder("empty/inner"));
}

TEST_CASE("an unknown folder reports the error and leaves no undo step") {
    Scene s;
    TrashOps ops;
    const auto trashed = ops.trash_folder(s.store, s.listing(), "ghost", s.prompts);
    REQUIRE(trashed);
    CHECK(trashed->failure);
    CHECK_FALSE(ops.can_undo());
}

TEST_CASE("permanent delete, purge and empty always ask and default to Cancel") {
    Scene s;
    s.put("a.md");
    s.put("b.md");
    s.put("work/c.md");
    TrashOps ops;
    REQUIRE(ops.trash_note(s.store, path("b.md")));

    CHECK(ops.remove(s.store, path("a.md"), Prompts{}) == false);
    CHECK(ops.purge(s.store, path("b.md"), Prompts{}) == false);
    CHECK(ops.remove_folder(s.store, s.listing(), "work", Prompts{}) == false);
    CHECK(ops.empty_trash(s.store, Prompts{}) == false);
    CHECK_FALSE(
        ops.delete_items(s.store, s.listing(), std::vector<TrashTarget>{{path("a.md")}}, false, Prompts{}));
    CHECK(s.live("a.md"));
    CHECK(s.trashed("b.md"));
    CHECK(s.live("work/c.md"));

    s.answer = true;
    CHECK(ops.remove_folder(s.store, s.listing(), "work", s.prompts) == true);
    REQUIRE(s.asked.size() == 1);
    CHECK(s.asked[0].kind == ConfirmKind::DeleteFolder);
    CHECK(s.asked[0].notes == 1);
    CHECK_FALSE(s.live("work/c.md"));
    CHECK_FALSE(s.trashed("work/c.md"));
    CHECK(ops.purge(s.store, path("b.md"), s.prompts) == true);
    CHECK(s.asked.back().kind == ConfirmKind::Purge);
    CHECK_FALSE(ops.can_undo());  // kalıcı silinen öğe geri alma yığınından düştü
}

TEST_CASE("emptying the trash clears the undo stack") {
    Scene s;
    s.put("a.md");
    TrashOps ops;
    REQUIRE(ops.trash_note(s.store, path("a.md")));
    s.answer = true;
    CHECK(ops.empty_trash(s.store, s.prompts) == true);
    CHECK(s.asked.back().kind == ConfirmKind::EmptyTrash);
    CHECK(s.asked.back().items == 1);
    CHECK_FALSE(ops.can_undo());
    CHECK_FALSE(s.trashed("a.md"));
}

TEST_CASE("a batch continues past a failure, reports it and one undo restores what was trashed") {
    Scene s;
    s.put("a.md");
    s.put("c.md");
    TrashOps ops;
    const std::vector<TrashTarget> items{{path("a.md")}, {path("gone.md")}, {path("c.md")}};
    const auto report = ops.trash_items(s.store, s.listing(), items, s.prompts);
    REQUIRE(report);
    CHECK(s.asked.empty());  // klasörde not yok: sorulmaz
    CHECK(report->done == 2);
    CHECK(report->failed == 1);
    REQUIRE(report->lines.size() == 1);
    CHECK(report->lines[0].path == "gone.md");
    CHECK(report->lines[0].error);
    REQUIRE(ops.undo(s.store));
    CHECK(s.live("a.md"));
    CHECK(s.live("c.md"));
}

TEST_CASE("restoring from the trash view takes the item off the undo stack") {
    Scene s;
    s.put("a.md");
    TrashOps ops;
    REQUIRE(ops.trash_note(s.store, path("a.md")));
    REQUIRE(ops.restore(s.store, path("a.md")));
    CHECK_FALSE(ops.can_undo());
}

// A3b: kalıcı klasör silme yalnız kullanıcıya gösterilen notları siler; taramadan sonra gelen not kalır.
TEST_CASE("permanent folder delete removes only the shown notes; a note synced in later stays") {
    for (const bool batch : {false, true}) {
        CAPTURE(batch);
        Scene s;
        s.put("work/a.md");
        s.put("work/b.md");
        s.listing();            // kullanıcı iki not gördü
        s.put("work/late.md");  // senkron getirdi, liste taranmadı
        s.answer = true;
        TrashOps ops;
        if (batch) {
            const auto report = ops.delete_items(
                s.store, s.shown(), std::vector<TrashTarget>{{path("work"), true}}, false, s.prompts);
            REQUIRE(report);
            CHECK(report->failed == 1);
            REQUIRE(report->lines.size() == 1);
            REQUIRE(report->lines[0].error);
            CHECK(report->lines[0].error->code == noteapp::core::Error::Code::NotEmpty);
        } else {
            const auto removed = ops.remove_folder(s.store, s.shown(), "work", s.prompts);
            REQUIRE_FALSE(removed);
            CHECK(removed.error().code == noteapp::core::Error::Code::NotEmpty);
            CHECK(removed.error().message_key == "error.folder_unseen_notes");
        }
        REQUIRE(s.asked.size() == 1);
        CHECK(s.asked[0].notes == 2);
        CHECK(s.live("work/late.md"));  // gösterilmeyen not kalıcı silinmedi
        CHECK_FALSE(s.live("work/a.md"));
        CHECK_FALSE(s.live("work/b.md"));
        CHECK(s.folder("work"));
    }
}

// A3b: çöpte artık olmayan tek notluk adım yığını kilitlemez; eski adımlar geri alınabilir.
TEST_CASE("undo drops a step whose note is gone from the trash and reaches the older step") {
    Scene s;
    s.put("a.md");
    s.put("b.md");
    TrashOps ops;
    REQUIRE(ops.trash_note(s.store, path("a.md")));
    REQUIRE(ops.trash_note(s.store, path("b.md")));
    REQUIRE(s.store.purge(path("b.md")));  // başka cihaz çöpü temizledi

    const auto gone = ops.undo(s.store);
    REQUIRE(gone);
    CHECK_FALSE(gone->first);
    REQUIRE(gone->report.lines.size() == 1);
    REQUIRE(gone->report.lines[0].error);
    CHECK(gone->report.lines[0].error->code == noteapp::core::Error::Code::NotFound);
    CHECK(ops.undo_depth() == 1);

    REQUIRE(ops.undo(s.store));
    CHECK(s.live("a.md"));
    CHECK_FALSE(ops.can_undo());
}

// A3b: çöpteki yol başka nota geçtiyse (eski not temizlendi, aynı adlı başka not atıldı) geri alma o notu
// getirmez.
TEST_CASE("undo does not restore a different note that took the same trash path") {
    Scene s;
    s.put("a.md");
    TrashOps ops;
    REQUIRE(ops.trash_note(s.store, path("a.md")));
    REQUIRE(s.store.purge(path("a.md")));
    s.put("a.md");
    REQUIRE(s.store.trash(path("a.md")));  // başka not, aynı çöp yolu

    const auto undone = ops.undo(s.store);
    REQUIRE(undone);
    CHECK_FALSE(undone->first);
    CHECK(s.trashed("a.md"));  // başkasının notu çöpte kaldı
    CHECK_FALSE(s.live("a.md"));
    CHECK_FALSE(ops.can_undo());
}
