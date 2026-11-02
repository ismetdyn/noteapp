#include "core/config.hpp"
#include "core/i18n.hpp"
#include "core/text.hpp"
#include "detect/detect.hpp"
#include "gui/gui_state.hpp"
#include "gui/save_dialog.hpp"
#include "window_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <doctest/doctest.h>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

using gui_test::WindowFixture;
using noteapp::gui::CloseChoice;
using noteapp::gui::ConflictChoice;
using noteapp::gui::GuiState;
using noteapp::gui::SaveDetails;
using noteapp::gui::SaveDialogRequest;
using noteapp::gui::SaveForm;
namespace fs = std::filesystem;

namespace {

constexpr int kAutosaveMs = 60;
constexpr const char kJavaCode[] = "public class Greeter {\n    public static void main(String[] args) {\n"
                                   "        System.out.println(\"hi\");\n    }\n}\n";
constexpr const char kBom[] = "\xEF\xBB\xBF";

// Saat enjekte edilir: autosave gerçek zaman beklemeden sınanır.
auto after_ms(int ms) {
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}

bool header_has(const GuiState& state, const char* key) {
    return state.header().find(noteapp::core::tr(key)) != std::string::npos;
}

std::string current(const GuiState& state) {
    const auto path = state.current_path();
    return path ? path->string() : std::string{};
}

// Her test kendi kararlarını enjekte eder; çağrı sayıları da denetlenir.
struct Calls {
    int dialog = 0;
    int conflict = 0;
    int close_prompt = 0;
    SaveDialogRequest last_request;
    std::string conflict_path;
};

struct SaveFixture : WindowFixture {
    Calls calls;
    SaveFixture() { config.tuning.autosave_ms = kAutosaveMs; }

    void wire(GuiState& state, std::optional<SaveDetails> dialog_result, ConflictChoice conflict,
              CloseChoice close_choice = CloseChoice::Cancel) {
        state.deciders.save_dialog = [this, dialog_result](const SaveDialogRequest& request) {
            ++calls.dialog;
            calls.last_request = request;
            return dialog_result;
        };
        state.deciders.conflict = [this, conflict](const std::string& path) {
            ++calls.conflict;
            calls.conflict_path = path;
            return conflict;
        };
        state.deciders.close_unsaved = [this, close_choice] {
            ++calls.close_prompt;
            return close_choice;
        };
    }
};

}  // namespace

TEST_CASE_FIXTURE(SaveFixture, "editing an existing note is autosaved once after autosave_ms") {
    add_note("alpha.md", "alpha body", 10);
    auto state = make_state();
    REQUIRE(current(*state) == "alpha.md");
    CHECK(state->editable());
    CHECK(header_has(*state, "gui.state_saved"));

    for (const char* text :
         {"1alpha body", "12alpha body", "123alpha body", "1234alpha body", "12345alpha body"})
        state->edit_text(text);  // art arda tuşlar
    CHECK(header_has(*state, "gui.state_modified"));
    CHECK(state->write_count() == 0);
    CHECK(read_note("alpha.md") == "alpha body");

    state->tick(after_ms(kAutosaveMs * 4));
    CHECK(state->write_count() == 1);  // tüm tuşlar tek yazmaya birleşti
    CHECK(read_note("alpha.md") == "12345alpha body");
    CHECK(header_has(*state, "gui.state_saved"));

    state->tick(after_ms(kAutosaveMs * 8));
    CHECK(state->write_count() == 1);  // değişmeyen not yeniden yazılmaz
}

TEST_CASE_FIXTURE(SaveFixture, "each keystroke restarts the autosave deadline") {
    add_note("alpha.md", "a", 10);
    auto state = make_state();
    state->edit_text("ab");
    const auto first = state->next_deadline();
    REQUIRE(first.has_value());
    state->tick(*first - std::chrono::milliseconds(1));  // süre dolmadan yazılmaz
    CHECK(state->write_count() == 0);
    state->edit_text("abc");
    REQUIRE(state->next_deadline().has_value());
    CHECK(*state->next_deadline() >= *first);
    state->save_now();
    CHECK_FALSE(state->next_deadline().has_value());  // kayıt bekleyen süreyi iptal eder
}

TEST_CASE_FIXTURE(SaveFixture, "rescan reloads a clean open note silently when it changed on disk") {
    add_note("alpha.md", "old", 10);
    auto state = make_state();
    REQUIRE(state->buffer() == "old");

    add_note("alpha.md", "from outside", 0);
    state->rescan();
    CHECK(state->buffer() == "from outside");
    CHECK(state->write_count() == 0);
}

TEST_CASE_FIXTURE(SaveFixture, "rescan keeps a dirty buffer when the note changed on disk") {
    add_note("alpha.md", "old", 10);
    auto state = make_state();
    wire(*state, std::nullopt, ConflictChoice::Cancel);
    state->edit_text("mine");

    add_note("alpha.md", "from outside", 0);
    state->rescan();
    CHECK(state->buffer() == "mine");
    CHECK(read_note("alpha.md") == "from outside");
}

TEST_CASE_FIXTURE(SaveFixture, "conflict: save mine as copy keeps both versions and shows the copy") {
    add_note("work/alpha.md", "original", 10);
    auto state = make_state();
    wire(*state, std::nullopt, ConflictChoice::SaveCopy);
    REQUIRE(current(*state) == "work/alpha.md");

    state->edit_text("mine");
    add_note("work/alpha.md", "external", 0);  // okuma ile kayıt arasında dışarıdan değişiklik
    state->save_now();

    CHECK(calls.conflict == 1);
    CHECK(calls.conflict_path == "work/alpha.md");
    CHECK(read_note("work/alpha.md") == "external");
    CHECK(read_note("work/alpha (copy).md") == "mine");
    CHECK(current(*state) == "work/alpha (copy).md");
    CHECK(state->buffer() == "mine");
    CHECK(state->header().find("work/alpha (copy).md") != std::string::npos);
    CHECK(header_has(*state, "gui.state_saved"));
}

TEST_CASE_FIXTURE(SaveFixture, "conflict: a second copy gets the collision suffix") {
    add_note("alpha.md", "original", 10);
    add_note("alpha (copy).md", "older copy", 20);
    auto state = make_state();
    wire(*state, std::nullopt, ConflictChoice::SaveCopy);
    state->rescan();
    state->select_row(state->model().index_of(*noteapp::core::NotePath::parse("alpha.md")));
    REQUIRE(current(*state) == "alpha.md");

    state->edit_text("mine");
    add_note("alpha.md", "external", 0);
    state->save_now();
    CHECK(read_note("alpha (copy).md") == "older copy");  // ezilmedi
    CHECK(read_note("alpha (copy)-2.md") == "mine");
}

TEST_CASE_FIXTURE(SaveFixture, "conflict: overwrite writes the buffer over the external change") {
    add_note("alpha.md", "original", 10);
    auto state = make_state();
    wire(*state, std::nullopt, ConflictChoice::Overwrite);

    state->edit_text("mine");
    add_note("alpha.md", "external", 0);
    state->save_now();
    CHECK(calls.conflict == 1);
    CHECK(read_note("alpha.md") == "mine");
    CHECK_FALSE(note_exists("alpha (copy).md"));
    CHECK(header_has(*state, "gui.state_saved"));

    state->edit_text("+mine");  // yeni mtime alındı: sonraki kayıt çakışmaz
    state->save_now();
    CHECK(calls.conflict == 1);
    CHECK(read_note("alpha.md") == "+mine");
}

TEST_CASE_FIXTURE(SaveFixture, "conflict: reload drops the buffer and shows the disk version") {
    add_note("alpha.md", "original", 10);
    auto state = make_state();
    wire(*state, std::nullopt, ConflictChoice::Reload);

    state->edit_text("mine");
    add_note("alpha.md", "external", 0);
    state->save_now();
    CHECK(state->buffer() == "external");
    CHECK(read_note("alpha.md") == "external");
    CHECK(header_has(*state, "gui.state_saved"));
}

TEST_CASE_FIXTURE(SaveFixture, "conflict: cancel keeps the buffer dirty and the disk untouched") {
    add_note("alpha.md", "original", 10);
    auto state = make_state();
    wire(*state, std::nullopt, ConflictChoice::Cancel);

    state->edit_text("mine");
    add_note("alpha.md", "external", 0);
    state->save_now();
    CHECK(state->buffer() == "mine");
    CHECK(read_note("alpha.md") == "external");
    CHECK(header_has(*state, "gui.state_modified"));
}

TEST_CASE_FIXTURE(SaveFixture, "the default conflict button is 'save mine as copy', Escape cancels") {
    const auto box = noteapp::gui::make_conflict_box("alpha.md");
    const auto& default_button = box.buttons.at(box.default_button);
    CHECK(default_button.first == noteapp::core::tr("gui.conflict_copy"));
    CHECK(default_button.second == ConflictChoice::SaveCopy);
    const auto& escape_button = box.buttons.at(box.escape_button);
    CHECK(escape_button.first == noteapp::core::tr("gui.cancel"));
    CHECK(escape_button.second == ConflictChoice::Cancel);
}

TEST_CASE_FIXTURE(SaveFixture, "a vanished note is saved through the new-note dialog, nothing is lost") {
    add_note("alpha.md", "original", 10);
    auto state = make_state();
    wire(*state, SaveDetails{"restored", "", "md"}, ConflictChoice::Cancel);

    remove_note("alpha.md");
    state->edit_text("mine");
    state->save_now();
    CHECK(calls.dialog == 1);
    CHECK(read_note("restored.md") == "mine");
}

TEST_CASE_FIXTURE(SaveFixture,
                  "new note: Ctrl+N opens an empty untitled buffer, first save asks the dialog") {
    add_note("work/alpha.md", "alpha", 10);
    auto state = make_state();
    wire(*state, SaveDetails{"greeter", "work", "txt"}, ConflictChoice::Cancel);

    state->new_note();
    CHECK(state->buffer().empty());
    CHECK(state->editable());
    CHECK(header_has(*state, "gui.untitled"));

    state->edit_text(kJavaCode);
    state->tick(after_ms(kAutosaveMs * 3));
    CHECK(calls.dialog == 0);  // yeni nota autosave yok: ilk kayıt kullanıcı kararı
    CHECK(state->write_count() == 0);

    state->save_now();
    CHECK(calls.dialog == 1);
    CHECK(read_note("work/greeter.txt") == kJavaCode);
    CHECK(current(*state) == "work/greeter.txt");  // artık mevcut not, listede seçili
    CHECK(header_has(*state, "gui.state_saved"));
}

TEST_CASE_FIXTURE(SaveFixture, "the suggested note name never carries a value from the content") {
    struct Case {
        std::string content;
        std::string name;
    };
    for (const Case& sample : {Case{"AWS_SECRET=sk-live\n", "aws-secret"}, Case{"KEY=value\n", "key"},
                               Case{"export TOKEN=x\n", "token"}, Case{"API_KEY: abc\n", "api-key"},
                               Case{"# prod\nAPI_TOKEN=s3cr3t\nB=1\n", "api-token"},
                               Case{"Toplantı: bütçe\n", "toplanti-butce"}}) {
        CAPTURE(sample.content);
        auto state = make_state();
        wire(*state, std::nullopt, ConflictChoice::Cancel);
        state->new_note();
        state->edit_text(sample.content);
        state->save_now();
        CHECK(calls.last_request.initial.name == sample.name);
    }
}

TEST_CASE_FIXTURE(SaveFixture, "save dialog is prefilled with the detect suggestion and the tree folder") {
    add_note("work/alpha.md", "alpha", 10);
    auto state = make_state();
    wire(*state, std::nullopt, ConflictChoice::Cancel);

    state->new_note();
    state->edit_text(kJavaCode);
    state->save_now();
    REQUIRE(calls.dialog == 1);

    const noteapp::detect::Options options{config.default_type, config.tuning.min_confidence,
                                           config.tuning.max_scan_bytes};
    const auto expected = noteapp::detect::suggest(kJavaCode, options);
    REQUIRE(expected.type == "java");
    CHECK(calls.last_request.initial.type == "java");
    CHECK(calls.last_request.initial.name == noteapp::core::slugify("public class Greeter {",
                                                                    std::chrono::system_clock::now(),
                                                                    config.tuning.slug_max_length));
    CHECK(calls.last_request.initial.folder.empty());  // "Tümü" seçili
    CHECK(calls.last_request.hint.find("java") != std::string::npos);
    CHECK(calls.last_request.hint.find(std::to_string(std::lround(expected.confidence * 100))) !=
          std::string::npos);
    CHECK(calls.last_request.types == config.types);
    CHECK(calls.last_request.folders == std::vector<std::string>{"work"});
}

TEST_CASE_FIXTURE(SaveFixture, "the save dialog draws the detect suggestion") {
    auto state = make_state();
    auto view = gui_test::make_view(*state);
    view->install_deciders();
    std::string hint;
    const auto real_dialog = state->deciders.save_dialog;
    state->deciders.save_dialog = [&](const SaveDialogRequest& request) {
        hint = request.hint;
        return real_dialog(request);
    };
    view->on_modal_frame = [frame = 0]() mutable {
        constexpr int kCancelFrame = 5;
        if (++frame == kCancelFrame)
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        if (frame == kCancelFrame + 1)
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    };
    state->new_note();
    state->edit_text(kJavaCode);

    const gui_test::RenderedText rendered;
    state->save_now();

    REQUIRE(hint.find("java") != std::string::npos);
    CHECK(rendered.contains(hint));
}

TEST_CASE_FIXTURE(SaveFixture, "chosen type becomes the extension") {
    auto state = make_state();
    wire(*state, SaveDetails{"greeter", "", "java"}, ConflictChoice::Cancel);
    state->new_note();
    state->edit_text("plain words");
    state->save_now();
    CHECK(note_exists("greeter.java"));
}

TEST_CASE_FIXTURE(SaveFixture, "cancelling the save dialog keeps the new note and its buffer") {
    auto state = make_state();
    wire(*state, std::nullopt, ConflictChoice::Cancel);
    state->new_note();
    state->edit_text("keep me");
    state->save_now();
    CHECK(calls.dialog == 1);
    CHECK(state->buffer() == "keep me");
    CHECK(header_has(*state, "gui.state_unsaved"));
    CHECK_FALSE(note_exists("keep-me.md"));
}

TEST_CASE_FIXTURE(SaveFixture, "a name collision in the dialog creates a suffixed note, never overwrites") {
    add_note("same.md", "existing", 10);
    auto state = make_state();
    wire(*state, SaveDetails{"same", "", "md"}, ConflictChoice::Cancel);
    state->new_note();
    state->edit_text("fresh");
    state->save_now();
    CHECK(read_note("same.md") == "existing");
    CHECK(read_note("same-2.md") == "fresh");
}

TEST_CASE_FIXTURE(SaveFixture, "an invalid dialog result keeps the buffer and reports the error") {
    auto state = make_state();
    wire(*state, SaveDetails{"bad/name", "", "md"}, ConflictChoice::Cancel);
    state->new_note();
    state->edit_text("keep me");
    state->save_now();
    CHECK(state->buffer() == "keep me");
    CHECK(header_has(*state, "gui.state_unsaved"));
    CHECK_FALSE(state->status().empty());
}

TEST_CASE_FIXTURE(SaveFixture, "SaveDialog shows prefilled fields and rejects invalid input inline") {
    SaveDialogRequest request;
    request.mode = SaveDialogRequest::Mode::Create;
    request.initial = {"my-note", "work", "java"};
    request.folders = {"work", "home"};
    request.types = {"md", "java"};
    request.hint = "suggested: java 82%";
    SaveForm form(request);

    CHECK(form.details().name == "my-note");
    CHECK(form.details().folder == "work");
    CHECK(form.details().type == "java");

    // Yazılan uzantı Tip alanını geçer, üstüne ekleme olmaz.
    for (const auto& [typed, stem, type] :
         {std::tuple{"deneme.cpp", "deneme", "cpp"}, std::tuple{"deneme.xyz", "deneme", "xyz"},
          std::tuple{"a.tar.gz", "a.tar", "gz"}, std::tuple{"notlar.v2", "notlar", "v2"}}) {
        CAPTURE(typed);
        form.fields.name = typed;
        CHECK(form.try_accept());
        CHECK(form.details().name == stem);
        CHECK(form.details().type == type);
    }

    form.fields.name = "a/b";
    CHECK_FALSE(form.try_accept());
    CHECK_FALSE(form.error.empty());

    form.fields.name = "ok";
    form.fields.folder = "../escape";
    CHECK_FALSE(form.try_accept());
    CHECK_FALSE(form.error.empty());

    form.fields.folder = "home";
    form.fields.type = ".txt";  // elle yazılan uzantı, baştaki nokta atılır
    CHECK(form.try_accept());
    CHECK(form.error.empty());
    CHECK(form.details().type == "txt");
    CHECK(form.details().folder == "home");
}

TEST_CASE_FIXTURE(SaveFixture, "switching notes saves the dirty note first") {
    add_note("alpha.md", "alpha", 20);
    add_note("beta.md", "beta", 10);
    auto state = make_state();
    REQUIRE(current(*state) == "beta.md");

    state->edit_text("edited beta");
    state->move_selection(1);  // alpha'ya geç
    CHECK(read_note("beta.md") == "edited beta");
    CHECK(current(*state) == "alpha.md");
    CHECK(state->buffer() == "alpha");
    CHECK(state->write_count() == 1);
}

TEST_CASE_FIXTURE(SaveFixture, "switching away from a new note asks the dialog; cancel stays on the note") {
    add_note("alpha.md", "alpha", 10);
    auto state = make_state();
    wire(*state, std::nullopt, ConflictChoice::Cancel);
    state->new_note();
    state->edit_text("draft text");

    state->move_selection(1);
    CHECK(calls.dialog == 1);
    CHECK(state->buffer() == "draft text");  // vazgeçildi: taslak yerinde
    CHECK(header_has(*state, "gui.state_unsaved"));
    CHECK_FALSE(state->current_path().has_value());

    wire(*state, SaveDetails{"draft", "", "md"}, ConflictChoice::Cancel);
    state->move_selection(1);
    CHECK(read_note("draft.md") == "draft text");
    CHECK(state->buffer() == "alpha");
}

TEST_CASE_FIXTURE(SaveFixture, "a failed save while switching keeps the buffer and the selection") {
    add_note("alpha.md", "alpha", 20);
    add_note("beta.md", "beta", 10);
    auto state = make_state();
    wire(*state, std::nullopt, ConflictChoice::Cancel);
    state->edit_text("mine");
    add_note("beta.md", "external", 0);

    state->move_selection(1);  // çakışma → Cancel → geçiş iptal
    CHECK(state->buffer() == "mine");
    CHECK(current(*state) == "beta.md");
}

TEST_CASE_FIXTURE(SaveFixture, "close with an unsaved new note: Cancel keeps the window and the buffer") {
    auto state = make_state();
    wire(*state, SaveDetails{"x", "", "md"}, ConflictChoice::Cancel, CloseChoice::Cancel);
    state->new_note();
    state->edit_text("unsaved");

    CHECK_FALSE(state->request_close());
    CHECK(calls.close_prompt == 1);
    CHECK(state->buffer() == "unsaved");
    CHECK_FALSE(note_exists("x.md"));
}

TEST_CASE_FIXTURE(SaveFixture, "close with an unsaved new note: Discard closes, Save creates the note") {
    {
        auto state = make_state();
        wire(*state, SaveDetails{"x", "", "md"}, ConflictChoice::Cancel, CloseChoice::Discard);
        state->new_note();
        state->edit_text("throwaway");
        CHECK(state->request_close());
        CHECK_FALSE(note_exists("x.md"));
    }
    auto state = make_state();
    wire(*state, SaveDetails{"x", "", "md"}, ConflictChoice::Cancel, CloseChoice::Save);
    state->new_note();
    state->edit_text("kept");
    CHECK(state->request_close());
    CHECK(read_note("x.md") == "kept");
}

TEST_CASE_FIXTURE(SaveFixture, "close with an empty new note does not ask") {
    auto state = make_state();
    wire(*state, std::nullopt, ConflictChoice::Cancel, CloseChoice::Cancel);
    state->new_note();
    CHECK(state->request_close());
    CHECK(calls.close_prompt == 0);
}

TEST_CASE_FIXTURE(SaveFixture, "close autosaves a dirty note; a conflict cancel aborts the close") {
    add_note("alpha.md", "alpha", 10);
    {
        auto state = make_state();
        wire(*state, std::nullopt, ConflictChoice::Cancel);
        state->edit_text("xalpha");
        CHECK(state->request_close());
        CHECK(read_note("alpha.md") == "xalpha");
    }
    auto state = make_state();
    wire(*state, std::nullopt, ConflictChoice::Cancel);
    state->edit_text("yxalpha");
    add_note("alpha.md", "external", 0);
    CHECK_FALSE(state->request_close());
    CHECK(state->buffer() == "yxalpha");
    CHECK(read_note("alpha.md") == "external");
}

TEST_CASE_FIXTURE(SaveFixture, "type combobox moves the note to the new extension") {
    add_note("work/alpha.md", "alpha", 10);
    auto state = make_state();
    REQUIRE(state->current_type() == "md");

    state->edit_text("xalpha");  // kirli not önce kaydedilir, sonra taşınır
    state->change_type("txt");
    CHECK(read_note("work/alpha.txt") == "xalpha");
    CHECK_FALSE(note_exists("work/alpha.md"));
    CHECK(current(*state) == "work/alpha.txt");
    CHECK(state->buffer() == "xalpha");

    state->edit_text("xyalpha");  // taşımadan sonra kayıt yeni yola gider
    state->save_now();
    CHECK(read_note("work/alpha.txt") == "xyalpha");
}

TEST_CASE_FIXTURE(SaveFixture, "type combobox refuses to overwrite an existing target") {
    add_note("alpha.md", "alpha", 10);
    add_note("alpha.txt", "other", 20);
    auto state = make_state();
    REQUIRE(current(*state) == "alpha.md");

    state->change_type("txt");
    CHECK(read_note("alpha.md") == "alpha");
    CHECK(read_note("alpha.txt") == "other");
    CHECK(current(*state) == "alpha.md");
    CHECK(state->current_type() == "md");  // kutu geri alınır
    CHECK_FALSE(state->status().empty());
}

TEST_CASE_FIXTURE(SaveFixture, "type choices list the configured types plus the current extension") {
    add_note("alpha.weird", "alpha", 10);
    auto state = make_state();
    const auto choices = state->type_choices();
    CHECK(std::ranges::find(choices, "weird") != choices.end());
    CHECK(std::ranges::find(choices, config.types.front()) != choices.end());
}

TEST_CASE_FIXTURE(SaveFixture, "Ctrl+Shift+S moves with the dialog prefilled from the current note") {
    add_note("alpha.md", "alpha", 10);
    auto state = make_state();
    wire(*state, SaveDetails{"renamed", "sub", "txt"}, ConflictChoice::Cancel);

    state->save_as();
    CHECK(calls.last_request.mode == SaveDialogRequest::Mode::Move);
    CHECK(calls.last_request.initial.name == "alpha");
    CHECK(calls.last_request.initial.type == "md");
    CHECK(read_note("sub/renamed.txt") == "alpha");
    CHECK_FALSE(note_exists("alpha.md"));
    CHECK(current(*state) == "sub/renamed.txt");
}

TEST_CASE_FIXTURE(SaveFixture, "Enter in the search box with no match starts a new note from the query") {
    add_note("alpha.md", "alpha", 10);
    auto state = make_state();
    wire(*state, std::nullopt, ConflictChoice::Cancel);

    state->set_query("zzzqqq");
    REQUIRE(state->model().row_count() == 0);
    state->search_enter();
    CHECK(state->buffer().starts_with("zzzqqq"));
    CHECK(state->editable());
    CHECK(header_has(*state, "gui.untitled"));
    CHECK(state->take_focus_request() == noteapp::gui::FocusRequest::Editor);

    state->save_now();
    CHECK(calls.last_request.initial.name == "zzzqqq");
}

TEST_CASE_FIXTURE(SaveFixture, "binary and non-UTF-8 notes stay read-only and are never rewritten") {
    add_note("blob.bin", std::string("ab\0cd", 5), 10);
    add_note("latin.txt", "caf\xe9", 20);
    auto state = make_state();
    REQUIRE(current(*state) == "blob.bin");
    CHECK_FALSE(state->editable());
    state->move_selection(1);
    REQUIRE(current(*state) == "latin.txt");
    CHECK_FALSE(state->editable());
    state->save_now();
    CHECK(read_note("latin.txt") == "caf\xe9");
    CHECK(state->write_count() == 0);
}

// ImGui metni C dizgisi olarak işler: ilk NUL'dan sonrası kayıtta sessizce kesilirdi. looks_binary yalnız
// ilk 8 KB'a baktığından geç gelen NUL burada ayrıca yakalanır.
TEST_CASE_FIXTURE(SaveFixture, "a NUL byte past the binary sniff window keeps the note read-only") {
    const std::string content = std::string(9000, 'a') + std::string("\0tail", 5);
    add_note("late-nul.txt", content, 10);
    auto state = make_state();
    REQUIRE(current(*state) == "late-nul.txt");
    CHECK_FALSE(state->editable());
    state->edit_text("x");  // salt-okunurda düzenleme yok sayılır
    state->save_now();
    state->tick(after_ms(kAutosaveMs * 4));
    CHECK(read_note("late-nul.txt") == content);
    CHECK(state->write_count() == 0);
}

TEST_CASE_FIXTURE(SaveFixture, "CRLF line endings survive an edit") {
    add_note("win.txt", "one\r\ntwo\r\n", 10);
    auto state = make_state();
    CHECK(state->buffer() == "one\ntwo\n");
    state->edit_text("one\ntwo\nthree\n");
    state->save_now();
    CHECK(read_note("win.txt") == "one\r\ntwo\r\nthree\r\n");
}

TEST_CASE_FIXTURE(SaveFixture, "a UTF-8 BOM stays out of the buffer and survives an edit") {
    add_note("bom.txt", std::string(kBom) + "one\r\n", 10);
    auto state = make_state();
    CHECK(state->buffer() == "one\n");
    state->edit_text("one\ntwo\n");
    state->save_now();
    CHECK(read_note("bom.txt") == std::string(kBom) + "one\r\ntwo\r\n");
}

TEST_CASE_FIXTURE(SaveFixture, "a conflict copy of a BOM note keeps the BOM") {
    add_note("bom.txt", std::string(kBom) + "one", 10);
    auto state = make_state();
    wire(*state, std::nullopt, ConflictChoice::SaveCopy);
    state->edit_text("mine");
    add_note("bom.txt", "external", 0);
    state->save_now();
    CHECK(read_note("bom (copy).txt") == std::string(kBom) + "mine");
}

// Karakterizasyon: yükleme CRLF'i doğrusal sürede LF'ye indirir (eskiden satır başına erase, O(n^2)); süre
// değil içerik doğrulanır.
TEST_CASE_FIXTURE(SaveFixture, "a large CRLF note loads with its content intact and keeps CRLF on save") {
    std::string crlf;
    std::string expected;
    for (int line = 0; line < 20000; ++line) {
        crlf += "satır " + std::to_string(line) + "\r\n";
        expected += "satır " + std::to_string(line) + "\n";
    }
    add_note("big.txt", crlf, 10);
    auto state = make_state();
    REQUIRE(current(*state) == "big.txt");
    CHECK(state->buffer() == expected);

    state->edit_text(expected + "son\n");
    state->save_now();
    CHECK(read_note("big.txt") == crlf + "son\r\n");
}

TEST_CASE_FIXTURE(WindowFixture, "save as on a multi-dot note keeps the whole name through the form") {
    add_note("a.tar.gz", "x");
    auto state = make_state();
    state->rescan();
    state->select_row(state->model().index_of(*noteapp::core::NotePath::parse("a.tar.gz")));
    std::optional<std::string> initial;
    state->deciders.save_dialog = [&](const SaveDialogRequest& request) -> std::optional<SaveDetails> {
        SaveForm form(request);
        initial = form.fields.name;
        REQUIRE(form.try_accept());
        return form.details();
    };
    state->save_as();
    CHECK(initial == "a.tar.gz");
    CHECK(note_exists("a.tar.gz"));
    CHECK_FALSE(note_exists("a.tar"));
    CHECK_FALSE(note_exists("a.gz"));
}
