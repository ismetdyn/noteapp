// PDF dışa aktarmanın arayüzden bağımsız kuralları (CLI, TUI, GUI aynı): ad, var olan hedef, ikili not.
#include "app/export.hpp"
#include "core/i18n.hpp"
#include "support/test_support.hpp"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using noteapp::core::NotePath;

namespace {

NotePath path(const std::string& text) {
    return *NotePath::parse(text);
}

}  // namespace

TEST_CASE("export: the suggested name drops only the last extension and the folder") {
    CHECK(noteapp::app::default_export_name(path("k/a.tar.gz")) == "a.tar.pdf");
    CHECK(noteapp::app::default_export_name(path("günlük.md")) == "günlük.pdf");
    CHECK(noteapp::app::default_export_name(path("README")) == "README.pdf");
}

TEST_CASE("export: an existing file or a dangling link is an existing target") {
    const noteapp::test::TempDir dir;
    CHECK_FALSE(noteapp::app::export_target_exists(dir.path() / "a.pdf"));
    std::ofstream(dir.path() / "a.pdf") << "x";
    CHECK(noteapp::app::export_target_exists(dir.path() / "a.pdf"));
#ifndef _WIN32
    fs::create_symlink(dir.path() / "missing.pdf", dir.path() / "link.pdf");
    CHECK(noteapp::app::export_target_exists(dir.path() / "link.pdf"));
#endif
}

TEST_CASE("export: a binary note is refused and nothing is written; text becomes a PDF") {
    noteapp::core::init_i18n("en");
    const noteapp::test::TempDir dir;
    const fs::path out = dir.path() / "o.pdf";
    const auto refused = noteapp::app::export_note_pdf(std::string("ab\0cd", 5), path("blob.bin"), out);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message_key == noteapp::app::kExportBinaryKey);
    CHECK(noteapp::core::format_error(refused.error()).find("blob.bin") != std::string::npos);
    CHECK_FALSE(fs::exists(out));
    REQUIRE(noteapp::app::export_note_pdf("# başlık\n", path("a.md"), out));
    CHECK(noteapp::test::slurp(out).starts_with("%PDF-"));
}
