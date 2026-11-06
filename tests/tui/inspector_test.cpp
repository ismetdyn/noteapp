// F4: TUI'nin özellik denetçisi (SPEC-tui "Denetçi"). Model GUI ile ortak (app::Inspector); burada `p` ile
// açılış (REVIEW, bağlantı bölümü, harita), `a` `e` `d` ve Enter, tür önekli değer satırı, onaylı silme,
// etiket, salt okunur anahtar ve güvenilmez metnin çizimi gerçek depo ve main'in yoluyla (ListActions).
#include "core/i18n.hpp"
#include "core/note_props.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "tui/list_actions.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

using namespace noteapp;
using tui::Focus;
using tui::Intent;
using tui::Key;

namespace {

constexpr int kWidth = 110;
constexpr int kHeight = 32;

core::NotePath np(const std::string& path) {
    return core::NotePath::parse(path).value();
}

struct Scene {
    test::TempDir dir;
    test::TempDir state_home;
    test::ScopedEnv home{"NOTEAPP_HOME", test::utf8(state_home.path())};
    std::optional<core::NoteStore> store{std::in_place, dir.path()};
    std::mutex mutex;
    std::string internal;
    tui::SystemClipboard system{[](std::string_view) {}};
    tui::TuiState state;
    tui::PreviewCache preview;
    tui::StatusLine status;
    int changes = 0;
    std::optional<tui::ConfirmState> confirm;
    std::optional<tui::RenameState> rename;
    tui::ListActions actions{tui::ListHost{store, mutex, internal, system, state, preview, status,
                                           [this] { refresh(); }, [this] { ++changes; }}};

    Scene() {
        core::init_i18n("en");
        REQUIRE(test::seed_note(*store, "b.md", "b\n"));
        REQUIRE(test::seed_note(*store, "a.md", "a\n"));
        state.set_window(kWidth, kHeight);
        refresh();
        REQUIRE(state.select_note(np("a.md")));
    }
    void refresh() {
        preview.invalidate();
        state.set_notes(store->list());
        state.set_folders(store->folders());
    }
    // main'in yolu: açık girdi ya da onay tuşu alır, yoksa durum; Intent ListActions'a.
    void key(Key key, std::string_view text = {}) {
        if (confirm) {
            if (confirm->handle(key, text) == tui::ConfirmState::Outcome::Confirmed)
                actions.finish_delete(confirm);
            else if (key == Key::Enter || key == Key::Escape)
                confirm.reset();
            return;
        }
        if (rename) {
            actions.handle_rename_key(rename, key, text);
            return;
        }
        const Intent intent = state.handle(key, text);
        if (intent != Intent::None)
            actions.run(intent, confirm, rename);
    }
    void type(std::string_view text) {
        for (const char c : text)
            key(Key::Char, std::string(1, c));
    }
    void open_review_inspector() {
        key(Key::Char, "l");  // REVIEW
        REQUIRE(state.focus() == Focus::Preview);
        key(Key::Char, "p");
        REQUIRE(state.focus() == Focus::Inspector);
    }
    const app::InspectorRow* row(std::string_view key_wanted) const {
        for (const app::InspectorRow& item : state.inspector().rows())
            if (item.key == key_wanted)
                return &item;
        return nullptr;
    }
    void select(std::string_view key_wanted) {
        for (std::size_t i = 0; i < state.inspector().rows().size(); ++i)
            if (state.inspector().rows()[i].key == key_wanted) {
                state.inspector().select(i);
                return;
            }
        FAIL("no row " << key_wanted);
    }
    graph::Value stored(const std::string& path, std::string_view key_wanted) const {
        const auto props = core::note_properties(*store, np(path));
        REQUIRE(props.has_value());
        for (const core::Property& property : *props)
            if (property.key == key_wanted)
                return property.value;
        return graph::Value{};
    }
    std::string screen() {
        auto screen =
            ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
        ftxui::Render(screen, tui::render(state, {}, status, {kWidth, kHeight}, nullptr, nullptr, nullptr,
                                          nullptr, nullptr, nullptr, nullptr, rename ? &*rename : nullptr));
        std::string out;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x)
                out += screen.PixelAt(x, y).character;
            out += '\n';
        }
        return out;
    }
};

}  // namespace

TEST_CASE("tui inspector: p in REVIEW opens the shown note's properties; Esc and p go back") {
    Scene scene;
    scene.open_review_inspector();
    REQUIRE(scene.state.inspector().target().has_value());
    CHECK(scene.state.inspector().target()->note == np("a.md"));
    REQUIRE(scene.row("type") != nullptr);
    CHECK(scene.row("type")->read_only);
    const std::string text = scene.screen();
    CHECK(text.find("type = md") != std::string::npos);
    CHECK(text.find(core::tr("inspector.hints").substr(0, 5)) != std::string::npos);
    scene.key(Key::Escape);
    CHECK(scene.state.focus() == Focus::Preview);
    scene.key(Key::Char, "p");
    scene.key(Key::Char, "p");
    CHECK(scene.state.focus() == Focus::Preview);
}

TEST_CASE("tui inspector: a asks a key then a value (auto type or prefix); e edits; d deletes after Yes") {
    Scene scene;
    scene.open_review_inspector();
    scene.key(Key::Char, "a");
    REQUIRE(scene.rename.has_value());
    REQUIRE(scene.rename->is_property());
    scene.type("priority");
    scene.key(Key::Enter);
    REQUIRE(scene.rename.has_value());
    CHECK(scene.rename->property_key() == "priority");
    CHECK(scene.screen().find(core::tr("inspector.value_prompt", {{"key", "priority"}})) !=
          std::string::npos);
    scene.type("3");
    scene.key(Key::Enter);
    CHECK_FALSE(scene.rename.has_value());
    CHECK(scene.stored("a.md", "priority") == graph::Value{std::int64_t{3}});
    CHECK(scene.status.notices.items().back().kind == app::NoticeKind::Success);

    scene.key(Key::Char, "a");
    scene.type("code");
    scene.key(Key::Enter);
    scene.type("text:42");
    scene.key(Key::Enter);
    CHECK(scene.stored("a.md", "code") == graph::Value{std::string("42")});

    // `e` (ve Enter) değer satırını düzenleme metniyle açar: önek türü korur.
    scene.select("code");
    scene.key(Key::Char, "e");
    REQUIRE(scene.rename.has_value());
    CHECK(scene.rename->text() == "text:42");
    scene.type("7");
    scene.key(Key::Enter);
    CHECK(scene.stored("a.md", "code") == graph::Value{std::string("427")});

    // `d`: varsayılan Hayır; Evet'e geçip Enter siler.
    scene.select("priority");
    scene.key(Key::Char, "d");
    REQUIRE(scene.confirm.has_value());
    scene.key(Key::Enter);
    CHECK(scene.stored("a.md", "priority") == graph::Value{std::int64_t{3}});
    scene.key(Key::Char, "d");
    scene.key(Key::Left);
    scene.key(Key::Enter);
    CHECK(std::holds_alternative<std::monostate>(scene.stored("a.md", "priority")));
    CHECK(scene.row("priority") == nullptr);
}

TEST_CASE("tui inspector: reserved keys stay read-only; a refused write keeps the line with the reason") {
    Scene scene;
    scene.open_review_inspector();
    scene.select("type");
    scene.key(Key::Char, "e");
    CHECK_FALSE(scene.rename.has_value());
    CHECK(scene.status.message == core::tr("inspector.read_only"));
    scene.key(Key::Char, "d");
    CHECK_FALSE(scene.confirm.has_value());

    scene.key(Key::Char, "a");
    scene.type("name");
    scene.key(Key::Enter);
    scene.type("z");
    scene.key(Key::Enter);
    REQUIRE(scene.rename.has_value());  // satır açık kalır
    CHECK(scene.rename->error().find("'name' is managed by the app") != std::string::npos);
    scene.key(Key::Escape);
    CHECK_FALSE(scene.rename.has_value());
    CHECK(scene.stored("a.md", "name") == graph::Value{std::string("a")});
}

TEST_CASE("tui inspector: #key adds a label to the note's text; d on the label removes it") {
    Scene scene;
    scene.open_review_inspector();
    scene.key(Key::Char, "a");
    scene.type("#plan");
    scene.key(Key::Enter);
    CHECK_FALSE(scene.rename.has_value());
    CHECK(scene.store->read(np("a.md"))->text == "a\n#plan\n");
    REQUIRE(scene.row("plan") != nullptr);
    scene.select("plan");
    scene.key(Key::Char, "d");
    scene.key(Key::Left);
    scene.key(Key::Enter);
    CHECK(scene.store->read(np("a.md"))->text == "a\n");
}

TEST_CASE("tui inspector: p on a related row of the links section inspects that relation") {
    Scene scene;
    REQUIRE(scene.store->add_relation(np("a.md"), np("b.md"), {"uses", true}).has_value());
    scene.refresh();
    REQUIRE(scene.state.select_note(np("a.md")));
    scene.key(Key::Char, "l");
    const auto links = scene.store->links(np("a.md"));
    REQUIRE(links.has_value());
    scene.state.set_links(*links, 1, np("a.md"));
    scene.key(Key::Char, "b");
    REQUIRE(scene.state.link_panel().selected_line() != nullptr);
    REQUIRE(scene.state.link_panel().selected_line()->group == core::LinkGroup::Related);
    scene.key(Key::Char, "p");
    REQUIRE(scene.state.focus() == Focus::Inspector);
    const auto& target = scene.state.inspector().target();
    REQUIRE(target.has_value());
    CHECK(target->kind == app::InspectTarget::Kind::Relation);
    CHECK(scene.state.inspector().title() == "a.md → b.md 'uses'");
    REQUIRE(scene.row("name") != nullptr);
    CHECK(scene.row("name")->read_only);
    scene.key(Key::Char, "a");
    scene.type("weight");
    scene.key(Key::Enter);
    scene.type("2");
    scene.key(Key::Enter);
    const auto props = core::relation_properties(*scene.store, links->related.front().id);
    REQUIRE(props.has_value());
    CHECK(std::ranges::any_of(*props, [](const core::Property& property) {
        return property.key == "weight" && property.value == graph::Value{std::int64_t{2}};
    }));
}

TEST_CASE("tui inspector: p on the map inspects the Tab-selected note; Esc returns to the map") {
    Scene scene;
    for (const tui::Command& command : tui::TuiState::commands())
        if (command.id == "view.map" && (command.contexts & tui::kInList) != 0)
            command.run(scene.state);
    REQUIRE(scene.state.map_view());
    scene.state.update_map([&] { return scene.store->edges(); }, np("a.md"));
    scene.key(Key::Char, "w");
    scene.state.update_map([&] { return scene.store->edges(); }, np("a.md"));
    scene.key(Key::Char, "p");
    REQUIRE(scene.state.focus() == Focus::Inspector);
    REQUIRE(scene.state.inspector().target().has_value());
    CHECK(scene.state.inspector().target()->note == np("a.md"));
    scene.key(Key::Escape);
    CHECK(scene.state.focus() == Focus::List);
    CHECK(scene.state.map_view());
}

// İnceleme: senkron ya da başka süreç yazınca açık denetçi eski satırları gösteriyordu. main yeniden taramada
// ve not listesini tazeleyince refresh_inspector'ı çağırır.
TEST_CASE("tui inspector: an external write shows up after the rescan refresh while the inspector is open") {
    Scene scene;
    scene.open_review_inspector();
    REQUIRE(scene.row("status") == nullptr);
    {
        core::NoteStore other(scene.dir.path());  // başka süreç gibi ayrı bağlantı
        REQUIRE(core::set_note_property(other, np("a.md"), "status", graph::Value{std::string("synced")})
                    .has_value());
    }
    scene.actions.refresh_inspector();
    REQUIRE(scene.row("status") != nullptr);
    CHECK(scene.row("status")->value_text == "synced");

    // Odak denetçide değilse depo okunmaz: satırlar dokunulmadan kalır.
    scene.key(Key::Escape);
    {
        core::NoteStore other(scene.dir.path());
        REQUIRE(core::set_note_property(other, np("a.md"), "status", graph::Value{}).has_value());
    }
    scene.actions.refresh_inspector();
    CHECK(scene.row("status") != nullptr);
}

TEST_CASE("tui inspector: untrusted keys and values are drawn without terminal control") {
    Scene scene;
    REQUIRE(core::set_note_property(*scene.store, np("a.md"), "k\x1b]0;x\x07",
                                    graph::Value{std::string("v\x1b[2J\xE2\x80\xAE")})
                .has_value());
    scene.open_review_inspector();
    const std::string text = scene.screen();
    CHECK(text.find('\x1b') == std::string::npos);
    CHECK(text.find('\x07') == std::string::npos);
    CHECK(text.find("\xE2\x80\xAE") == std::string::npos);  // RLO
}
