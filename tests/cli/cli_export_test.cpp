// note export --pdf: noteapp_pdf ile süreç içinde, notes-gui olmadan.
#include "support/cli_sandbox.hpp"

#include <cstdint>
#include <doctest/doctest.h>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace noteapp::test;

namespace {

constexpr std::uintmax_t kMinimalPdfBytes = 1000;  // boş kabuktan büyük: gerçek içerik ve font yazıldı

void put(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}

fs::path from_utf8(const std::string& name) {
    return fs::path(std::u8string(name.begin(), name.end()));
}

}  // namespace

TEST_CASE("export without --pdf is a usage error") {
    const Sandbox box;
    box.put("a.md", "x");
    CHECK(box.run({"export", "a.md"}).exit_code == 2);
    const auto no_note = box.run({"export", "--pdf"});
    CHECK(no_note.exit_code == 2);
    CHECK(no_note.err.find("export --pdf") != std::string::npos);  // kullanım metni
}

TEST_CASE("export with no matching note exits 1") {
    const Sandbox box;
    const auto result = box.run({"export", "--pdf", "yokboyle"});
    CHECK(result.exit_code == 1);
    CHECK_FALSE(result.err.empty());
}

TEST_CASE("export of a binary note exits 1") {
    const Sandbox box;
    box.put("resim.png", std::string("\x89PNG\0\1\2", 7));
    const auto out = box.notes().parent_path() / "resim.pdf";
    CHECK(box.run({"export", "--pdf", "resim.png", "-o", utf8(out)}).exit_code == 1);
    CHECK_FALSE(fs::exists(out));
}

TEST_CASE("export never reads a note through a directory link") {
    const Sandbox box;
    const TempDir outside;
    put(outside.path() / "secret.md", "gizli");
    fs::create_directories(box.notes());
    REQUIRE(make_dir_link(outside.path(), box.notes() / "link"));

    CHECK(box.run({"export", "--pdf", "link/secret.md", "-o", utf8(outside.path() / "s.pdf")}).exit_code !=
          0);
    CHECK_FALSE(fs::exists(outside.path() / "s.pdf"));
    CHECK(slurp(outside.path() / "secret.md") == "gizli");
}

// Davranış 5: `note` PDF için alt süreç başlatmaz; yanında notes-gui olmasa da ve ortamda ne olursa olsun
// çalışır.
TEST_CASE("export works without notes-gui and ignores the old GUI seam") {
    const Sandbox box;
    box.put("a.md", "x");
    const TempDir out;
    const TempDir marker_dir;
    const ScopedEnv gui("NOTEAPP_GUI_EXE", std::string(NOTEAPP_FAKE_EDITOR));
    const ScopedEnv args_out("FAKE_EDITOR_ARGS_OUT", utf8(marker_dir.path() / "args.txt"));
    const auto result = box.run({"export", "--pdf", "a.md", "-o", utf8(out.path() / "a.pdf")});
    CHECK_MESSAGE(result.exit_code == 0, result.err);
    CHECK(slurp(out.path() / "a.pdf").rfind("%PDF-", 0) == 0);
    CHECK_FALSE(fs::exists(marker_dir.path() / "args.txt"));
}

TEST_CASE("export writes a PDF into a subfolder via -o") {
    const Sandbox box;
    box.put("toplantı notu.md", "# Başlık\n\nçğıİöşü içerik\n");
    const auto out = box.notes().parent_path() / "çıktı" / "not.pdf";
    fs::create_directories(out.parent_path());
    const auto result = box.run({"export", "--pdf", "toplantı notu.md", "-o", utf8(out)});
    CHECK_MESSAGE(result.exit_code == 0, result.err);
    CHECK(slurp(out).rfind("%PDF-", 0) == 0);
    CHECK(fs::file_size(out) > kMinimalPdfBytes);
    CHECK(result.out.find("not.pdf") != std::string::npos);
}

// Aynı baytlar uzantıya göre farklı işlenir: .md başlık çizer, .txt olduğu gibi bırakır.
TEST_CASE("export of a txt note goes end to end through the plain text path") {
    const Sandbox box;
    const std::string body = "# değil başlık\nçğıİöşü\n";
    box.put("düz.txt", body);
    box.put("işaretli.md", body);
    const TempDir out;
    const auto plain = box.run({"export", "--pdf", "düz.txt", "-o", utf8(out.path() / "p.pdf")});
    const auto marked = box.run({"export", "--pdf", "işaretli.md", "-o", utf8(out.path() / "m.pdf")});
    CHECK_MESSAGE(plain.exit_code == 0, plain.err);
    CHECK_MESSAGE(marked.exit_code == 0, marked.err);
    CHECK(slurp(out.path() / "p.pdf").rfind("%PDF-", 0) == 0);
    CHECK(fs::file_size(out.path() / "p.pdf") > kMinimalPdfBytes);
    CHECK(slurp(out.path() / "p.pdf") != slurp(out.path() / "m.pdf"));
}

TEST_CASE("export defaults to <stem>.pdf in the current directory") {
    const Sandbox box;
    box.put("k/günlük.md", "merhaba");
    const TempDir cwd;
    const auto old = fs::current_path();
    fs::current_path(cwd.path());
    const auto result = box.run({"export", "--pdf", "k/günlük.md"});
    fs::current_path(old);
    CHECK_MESSAGE(result.exit_code == 0, result.err);
    CHECK(slurp(cwd.path() / "günlük.pdf").rfind("%PDF-", 0) == 0);
}

// Senkronla gelen dosya adı terminale OSC 52 (panoya yazma) yollayamasın; dosyanın kendisi yine bu adla
// yazılır.
TEST_CASE("export neutralises C1 control bytes of a synced name in the printed path") {
    const Sandbox box;
    const std::string stem = "\xC2\x9D"
                             "52;c;ZXZpbA==\xC2\x9C";
    box.put(stem + ".md", "x");
    const TempDir cwd;
    const auto old = fs::current_path();
    fs::current_path(cwd.path());
    const auto result = box.run({"export", "--pdf", stem + ".md"});
    fs::current_path(old);
    CHECK_MESSAGE(result.exit_code == 0, result.err);
    CHECK(fs::exists(cwd.path() / from_utf8(stem + ".pdf")));
    CHECK(result.out.find("\xC2\x9D") == std::string::npos);
    CHECK(result.out.find("\xC2\x9C") == std::string::npos);
    CHECK(result.out.find("52;c;ZXZpbA==") != std::string::npos);
}

TEST_CASE("unwritable output exits 3 and leaves no file") {
    const Sandbox box;
    box.put("a.md", "x");
    const auto out = box.notes().parent_path() / "yok" / "a.pdf";
    const auto result = box.run({"export", "--pdf", "a.md", "-o", utf8(out)});
    CHECK(result.exit_code == 3);
    CHECK_FALSE(result.err.empty());
    CHECK_FALSE(fs::exists(out));
}
