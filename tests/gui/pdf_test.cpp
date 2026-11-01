// Ctrl+E akışı: PDF motorunun kendisi tests/pdf'te sınanır.
#include "gui/gui_state.hpp"
#include "support/test_support.hpp"
#include "window_fixture.hpp"

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
