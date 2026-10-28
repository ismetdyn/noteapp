// note export --pdf (T33): GUI'yi başsız çağıran CLI tarafı.
#include "support/cli_sandbox.hpp"

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

// Okunamayan not dışa aktarılmaz: hata yazılır, notes-gui hiç başlatılmaz (fake_editor GUI yerine durur).
TEST_CASE("export stops on a read error and never starts notes-gui") {
    const Sandbox box;
    put(box.notes() / "a.md", "x");
    const TempDir out;
    const ScopedEnv gui("NOTEAPP_GUI_EXE", std::string(NOTEAPP_FAKE_EDITOR));
    const ScopedEnv args_out("FAKE_EDITOR_ARGS_OUT", utf8(out.path() / "args.txt"));
    const std::vector<std::string> args{"export", "--pdf", "a.md", "-o", utf8(out.path() / "a.pdf")};
#ifdef _WIN32
    // Paylaşımsız açık tutamaç: `note` notu listeler ama okuyamaz.
    const HANDLE lock = CreateFileW((box.notes() / "a.md").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    const auto result = box.run(args);
    CloseHandle(lock);
#else
    if (geteuid() == 0) {
        MESSAGE("root okuma iznini aşar; test atlandı");
        return;
    }
    fs::permissions(box.notes() / "a.md", fs::perms::none, fs::perm_options::replace);
    const auto result = box.run(args);
    fs::permissions(box.notes() / "a.md", fs::perms::owner_all, fs::perm_options::replace);
#endif
    CHECK(result.exit_code == 3);
    CHECK_FALSE(result.err.empty());
    CHECK_FALSE(fs::exists(out.path() / "args.txt"));
}

TEST_CASE("export never reads a note through a directory link") {
    const Sandbox box;
    const TempDir outside;
    put(outside.path() / "secret.md", "gizli");
    fs::create_directories(box.notes());
    REQUIRE(make_dir_link(outside.path(), box.notes() / "link"));
    const ScopedEnv gui("NOTEAPP_GUI_EXE", std::string(NOTEAPP_FAKE_EDITOR));
    const ScopedEnv args_out("FAKE_EDITOR_ARGS_OUT", utf8(outside.path() / "args.txt"));

    CHECK(box.run({"export", "--pdf", "link/secret.md", "-o", utf8(outside.path() / "s.pdf")}).exit_code !=
          0);
    CHECK_FALSE(fs::exists(outside.path() / "args.txt"));
    CHECK(slurp(outside.path() / "secret.md") == "gizli");
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
