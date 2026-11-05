// Ctrl+E akışı: PDF motorunun kendisi tests/pdf'te sınanır.
#include "gui/gui_state.hpp"
#include "gui/view.hpp"
#include "support/test_support.hpp"
#include "window_fixture.hpp"

#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <optional>
#include <string>

using gui_test::make_view;
using gui_test::press;
using gui_test::WindowFixture;
using noteapp::gui::GuiState;
namespace fs = std::filesystem;

namespace {

struct ExportFixture : WindowFixture {
    noteapp::test::TempDir out;
    int asked = 0;
    std::string suggested;

    void wire(GuiState& state, std::optional<fs::path> answer) {
        state.deciders.export_path = [this,
                                      answer](const std::string& suggestion) -> std::optional<std::string> {
            ++asked;
            suggested = suggestion;
            if (!answer)
                return std::nullopt;
            return noteapp::test::utf8(*answer);
        };
    }
    fs::path out_path() const { return out.path() / fs::path(u8"çıktı.pdf"); }
    bool out_empty() const { return fs::is_empty(out.path()); }
};

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE_FIXTURE(ExportFixture, "export flushes a dirty buffer first, then writes the pdf and reports it") {
    add_note("alpha.md", "old");
    auto state = make_state();
    state->edit_text("# yeni içerik çğış");
    wire(*state, out_path());

    state->export_pdf();

    CHECK(read_note("alpha.md") == "# yeni içerik çğış");  // PDF'in kaynağı diskteki sürüm
    CHECK(noteapp::test::slurp(out_path()).starts_with("%PDF-"));
    CHECK(suggested == "alpha.pdf");
    CHECK(contains(state->status(), noteapp::test::utf8(out_path())));
}

TEST_CASE_FIXTURE(ExportFixture, "a failing flush cancels the export and keeps the buffer") {
    add_note("alpha.md", "original", 10);
    auto state = make_state();
    state->edit_text("mine");
    add_note("alpha.md", "external", 0);  // çakışma; varsayılan karar Vazgeç
    wire(*state, out_path());

    state->export_pdf();

    CHECK(asked == 0);
    CHECK(out_empty());
    CHECK(state->buffer() == "mine");
    CHECK(state->dirty());
}

TEST_CASE_FIXTURE(ExportFixture, "cancelled export path writes no pdf") {
    add_note("alpha.md", "# a");
    auto state = make_state();
    wire(*state, std::nullopt);

    state->export_pdf();

    CHECK(asked == 1);
    CHECK(out_empty());
}

TEST_CASE_FIXTURE(ExportFixture, "export failure is shown in the status bar") {
    add_note("alpha.md", "# a");
    auto state = make_state();
    const fs::path bad = out.path() / "yok-dizin" / "x.pdf";
    wire(*state, bad);

    state->export_pdf();

    CHECK_FALSE(fs::exists(bad));
    CHECK(contains(state->status(), "Operation failed"));
}

TEST_CASE_FIXTURE(ExportFixture, "export does nothing for a binary note") {
    add_note("blob.bin", std::string("ab\0cd", 5));
    auto state = make_state();
    wire(*state, out_path());

    state->export_pdf();

    CHECK(asked == 0);
    CHECK(out_empty());
}

TEST_CASE_FIXTURE(ExportFixture, "export does nothing when no note is shown") {
    auto state = make_state();
    wire(*state, out_path());

    state->export_pdf();

    CHECK(asked == 0);
}

TEST_CASE_FIXTURE(ExportFixture, "Ctrl+E opens the export path decider") {
    add_note("alpha.md", "# a");
    auto state = make_state();
    auto view = make_view(*state);
    wire(*state, out_path());
    press(*view, ImGuiMod_Ctrl | ImGuiKey_E);
    CHECK(asked == 1);
    CHECK(noteapp::test::slurp(out_path()).starts_with("%PDF-"));
}

TEST_CASE_FIXTURE(ExportFixture, "the path dialog accepts a typed path and asks before replacing a file") {
    add_note("alpha.md", "# a");
    auto state = make_state();
    auto view = make_view(*state);
    view->install_deciders();
    const std::string typed = noteapp::test::utf8(out_path());

    // Metin alanı odakta ve seçili: yazılan öneriyi değiştirir, Enter kabul eder. İkinci diyalog (var olan
    // dosya) yalnız ikinci aktarımda çıkar ve varsayılanı Vazgeç'tir.
    const auto script = [&typed, frame = 0]() mutable {
        ++frame;
        if (frame == 5)
            ImGui::GetIO().AddInputCharactersUTF8(typed.c_str());
        if (frame == 8 || frame == 15)
            gui_test::tap_key(ImGuiKey_Enter);
        if (frame == 400)
            gui_test::tap_key(ImGuiKey_Escape);
    };
    view->on_modal_frame = script;
    state->export_pdf();
    const std::string first = noteapp::test::slurp(out_path());
    REQUIRE(first.starts_with("%PDF-"));

    std::ofstream(out_path(), std::ios::binary | std::ios::trunc) << "keep";
    view->on_modal_frame = script;
    state->export_pdf();
    CHECK(noteapp::test::slurp(out_path()) == "keep");
}

// NY-G: işlem bildirimi (toast). Dışa aktarma başarı bildirimi "Klasörü aç" taşır, notice_ms sonra düşer;
// düşme anı uyanmadır (next_deadline), boşta kare üretilmez. Hata kapatılana dek kalır.
TEST_CASE_FIXTURE(ExportFixture, "a pdf export posts a notice that opens its folder and expires after notice_ms") {
    config.tuning.notice_ms = noteapp::core::Limit{50, "50"};
    add_note("alpha.md", "# a");
    auto state = make_state();
    auto view = make_view(*state);
    wire(*state, out_path());
    std::string opened;
    state->deciders.open_folder = [&opened](const std::string& folder) {
        opened = folder;
        return true;
    };
    state->export_pdf();
    REQUIRE(state->notices().items().size() == 1);
    const noteapp::app::Notice notice = state->notices().items().front();
    CHECK(notice.kind == noteapp::app::NoticeKind::Success);
    CHECK(contains(notice.text, noteapp::test::utf8(out_path())));
    REQUIRE(notice.folder.has_value());
    CHECK(fs::path(*notice.folder) == out.path());
    gui_test::settle(*view);
    const std::string box = "notice:" + std::to_string(notice.id);
    REQUIRE(view->item_rects().contains(box));
    gui_test::click_button(*view, "notice_open:" + std::to_string(notice.id));
    CHECK(fs::path(opened) == out.path());

    REQUIRE(state->next_deadline().has_value());
    CHECK(*state->next_deadline() <= notice.posted + std::chrono::milliseconds(50));
    CHECK(state->tick(notice.posted + std::chrono::milliseconds(50)));
    CHECK(state->notices().items().empty());
    gui_test::settle(*view);
    CHECK_FALSE(view->item_rects().contains(box));
}

TEST_CASE_FIXTURE(ExportFixture, "a failed export posts an error notice that stays until it is clicked") {
    config.tuning.notice_ms = noteapp::core::Limit{50, "50"};
    add_note("alpha.md", "# a");
    auto state = make_state();
    auto view = make_view(*state);
    wire(*state, out.path() / "yok-dizin" / "x.pdf");
    state->export_pdf();
    REQUIRE(state->notices().items().size() == 1);
    const auto id = state->notices().items().front().id;
    CHECK(state->notices().items().front().kind == noteapp::app::NoticeKind::Error);
    state->tick(std::chrono::steady_clock::now() + std::chrono::hours(1));
    REQUIRE(state->notices().items().size() == 1);
    gui_test::settle(*view);
    gui_test::click_button(*view, "notice:" + std::to_string(id));
    CHECK(state->notices().items().empty());
}

// İnceleme bulgusu: "Klasörü aç" adresi baytları kodlar; göreli çıktı yolunun klasörü mutlaktır (boş olsaydı
// file:/// kökü açılırdı).
TEST_CASE("file_url encodes spaces, #, % and Turkish letters and keeps a drive path") {
    using noteapp::gui::file_url;
    CHECK(file_url("/home/a b/#1 %/şçğ") == "file:///home/a%20b/%231%20%25/%C5%9F%C3%A7%C4%9F");
    CHECK(file_url("C:/Users/a b") == "file:///C:/Users/a%20b");
#ifdef _WIN32
    CHECK(file_url("C:\\Users\\a b") == "file:///C:/Users/a%20b");
#endif
}

TEST_CASE_FIXTURE(ExportFixture, "a relative export path still gives the notice an absolute folder") {
    add_note("alpha.md", "# a");
    auto state = make_state();
    const fs::path before = fs::current_path();
    fs::current_path(out.path());
    wire(*state, fs::path(u8"göreli.pdf"));
    state->export_pdf();
    fs::current_path(before);
    REQUIRE(state->notices().items().size() == 1);
    const auto& folder = state->notices().items().front().folder;
    REQUIRE(folder.has_value());
    CHECK(fs::path(*folder).is_absolute());
    CHECK(fs::equivalent(fs::path(*folder), out.path()));
}
