// note export --pdf (T33): GUI'yi başsız çağıran CLI tarafı.
#include "support/cli_sandbox.hpp"

#include <doctest/doctest.h>
#include <fstream>

using namespace noteapp::test;

namespace {

void put(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}

}  // namespace

TEST_CASE("export without --pdf is a usage error") {
    const Sandbox box;
    put(box.notes() / "a.md", "x");
    CHECK(box.run({"export", "a.md"}).exit_code == 2);
    CHECK(box.run({"export", "--pdf"}).exit_code == 2);
}

TEST_CASE("export with no matching note exits 1") {
    const Sandbox box;
    const auto result = box.run({"export", "--pdf", "yokboyle"});
    CHECK(result.exit_code == 1);
    CHECK_FALSE(result.err.empty());
}

TEST_CASE("export of a binary note exits 1") {
    const Sandbox box;
    put(box.notes() / "resim.png", std::string("\x89PNG\0\1\2", 7));
    const auto out = box.notes().parent_path() / "resim.pdf";
    CHECK(box.run({"export", "--pdf", "resim.png", "-o", utf8(out)}).exit_code == 1);
    CHECK_FALSE(fs::exists(out));
}

TEST_CASE("missing GUI executable exits 3 with the install hint") {
    const Sandbox box("en");
    put(box.notes() / "a.md", "x");
    // Test dikişi: NOTEAPP_GUI_EXE var olmayan bir yolu gösterir (SPEC-cli kabul 10).
    const ScopedEnv gui("NOTEAPP_GUI_EXE", utf8(box.notes().parent_path() / "yok" / "notes-gui"));
    const auto out = box.notes().parent_path() / "a.pdf";
    const auto result = box.run({"export", "--pdf", "a.md", "-o", utf8(out)});
    CHECK(result.exit_code == 3);
    CHECK(result.err.find("notes-gui") != std::string::npos);
    CHECK_FALSE(fs::exists(out));
}

#ifdef NOTEAPP_GUI_EXE
TEST_CASE("export writes a PDF into a subfolder via -o") {
    const Sandbox box;
    const ScopedEnv gui("NOTEAPP_GUI_EXE", std::string(NOTEAPP_GUI_EXE));
    put(box.notes() / "toplantı notu.md", "# Başlık\n\nçğıİöşü içerik\n");
    const auto out = box.notes().parent_path() / "çıktı" / "not.pdf";
    fs::create_directories(out.parent_path());
    const auto result = box.run({"export", "--pdf", "toplantı notu.md", "-o", utf8(out)});
    CHECK_MESSAGE(result.exit_code == 0, result.err);
    CHECK(slurp(out).rfind("%PDF-", 0) == 0);
    CHECK(result.out.find("not.pdf") != std::string::npos);
}

TEST_CASE("export defaults to <stem>.pdf in the current directory") {
    const Sandbox box;
    const ScopedEnv gui("NOTEAPP_GUI_EXE", std::string(NOTEAPP_GUI_EXE));
    put(box.notes() / "k" / "günlük.md", "merhaba");
    const TempDir cwd;
    const auto old = fs::current_path();
    fs::current_path(cwd.path());
    const auto result = box.run({"export", "--pdf", "k/günlük.md"});
    fs::current_path(old);
    CHECK_MESSAGE(result.exit_code == 0, result.err);
    CHECK(slurp(cwd.path() / "günlük.pdf").rfind("%PDF-", 0) == 0);
}

TEST_CASE("unwritable output exits 3 and leaves no file") {
    const Sandbox box;
    const ScopedEnv gui("NOTEAPP_GUI_EXE", std::string(NOTEAPP_GUI_EXE));
    put(box.notes() / "a.md", "x");
    const auto out = box.notes().parent_path() / "yok" / "a.pdf";
    const auto result = box.run({"export", "--pdf", "a.md", "-o", utf8(out)});
    CHECK(result.exit_code == 3);
    CHECK_FALSE(result.err.empty());
    CHECK_FALSE(fs::exists(out));
}
#endif
