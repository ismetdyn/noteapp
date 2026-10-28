#include "support/cli_sandbox.hpp"

#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <vector>

using namespace noteapp::test;

namespace {

// Gerçek editör yerine fake_editor; VISUAL kullanıcının makinesinden sızmasın diye silinir.
class EditorEnv {
public:
    EditorEnv() : editor_("EDITOR", std::string(NOTEAPP_FAKE_EDITOR)), visual_("VISUAL", std::nullopt) {}

private:
    ScopedEnv editor_;
    ScopedEnv visual_;
};

}  // namespace

TEST_CASE("editor flow: content written by the editor becomes a note with a detected type") {
    const Sandbox box;
    const EditorEnv env;
    const std::string code = "public class A {}\nimport java.util.*;";
    const ScopedEnv content("FAKE_EDITOR_CONTENT", code);

    const auto result = box.run({"-e"});
    CHECK(result.exit_code == 0);
    CHECK(result.out.find("(java, önerilen)") != std::string::npos);
    CHECK(slurp(box.notes() / "public-class-a.java") == code);
}

// Betik `note < NUL` çağırırsa editör açılıp süreç asılı kalmamalı: stdin TTY değil, boş girdi reddedilir.
TEST_CASE("editor flow: stdin from the null device does not open the editor") {
    const Sandbox box;
    const EditorEnv env;
    const ScopedEnv content("FAKE_EDITOR_CONTENT", "editor must not run");

    const auto result = box.run_null_stdin({});
    CHECK(result.exit_code == 1);
    CHECK_FALSE(box.has_any_note());
}

TEST_CASE("editor flow: chosen type and folder rules match quick add") {
    const Sandbox box;
    const EditorEnv env;
    const ScopedEnv content("FAKE_EDITOR_CONTENT", "merhaba");

    const auto result = box.run({"-e", "-t", "txt", "-f", "work", "-n", "plan"});
    CHECK(result.exit_code == 0);
    CHECK(result.out == "Kaydedildi: work/plan.txt (txt, seçilen)\n");
    CHECK(slurp(box.notes() / "work" / "plan.txt") == "merhaba");
}

TEST_CASE("editor flow: a blank result cancels with exit 1 and creates nothing") {
    const Sandbox box;
    const EditorEnv env;
    SUBCASE("editor writes nothing") {
        const auto result = box.run({"-e"});
        CHECK(result.exit_code == 1);
        CHECK(result.err.find("ptal edildi") != std::string::npos);
    }
    SUBCASE("editor writes only whitespace") {
        const ScopedEnv content("FAKE_EDITOR_CONTENT", " \n\t \n");
        CHECK(box.run({"-e"}).exit_code == 1);
    }
    CHECK_FALSE(box.has_any_note());
}

TEST_CASE("editor flow: -e text arguments prefill the file and become the note if left unchanged") {
    const Sandbox box;
    const EditorEnv env;
    const TempDir out;
    const ScopedEnv args_out("FAKE_EDITOR_ARGS_OUT", utf8(out.path() / "args.txt"));

    const auto result = box.run({"-e", "Başlık"});
    CHECK(result.exit_code == 0);
    CHECK(slurp(box.notes() / "baslik.md") == "Başlık");
    // Geçici dosya: OS temp dizininde noteapp-*.md; başarıdan sonra silinmiş olmalı.
    const std::string args = slurp(out.path() / "args.txt");
    CHECK(args.find("noteapp-") != std::string::npos);
    CHECK(args.find(".md") != std::string::npos);
    std::string temp_path = args.substr(0, args.find('\n'));
    CHECK_FALSE(fs::exists(fs::path(std::u8string(temp_path.begin(), temp_path.end()))));
}

TEST_CASE("editor flow: Turkish content round-trips") {
    const Sandbox box;
    const EditorEnv env;
    const std::string text = "Çalışma günlüğü: İstanbul ığüşöç";
    const ScopedEnv content("FAKE_EDITOR_CONTENT", text);

    CHECK(box.run({"-e", "-n", "gunluk"}).exit_code == 0);
    CHECK(slurp(box.notes() / "gunluk.md") == text);
}

TEST_CASE("editor flow: a failing editor exits 3 and keeps the temp file with its path in the error") {
    const Sandbox box;
    const EditorEnv env;
    const TempDir out;
    const ScopedEnv args_out("FAKE_EDITOR_ARGS_OUT", utf8(out.path() / "args.txt"));
    const ScopedEnv content("FAKE_EDITOR_CONTENT", "yazdığım değerli metin");
    const ScopedEnv code("FAKE_EDITOR_EXIT", "3");

    const auto result = box.run({"-e"});
    CHECK(result.exit_code == 3);
    CHECK_FALSE(box.has_any_note());

    std::string temp_path = slurp(out.path() / "args.txt");
    temp_path = temp_path.substr(0, temp_path.find('\n'));
    const fs::path temp(std::u8string(temp_path.begin(), temp_path.end()));
    REQUIRE(fs::exists(temp));
    CHECK(slurp(temp) == "yazdığım değerli metin");
    CHECK(result.err.find(temp_path) != std::string::npos);
    std::error_code ec;
    fs::remove(temp, ec);
}

TEST_CASE("editor flow: a missing editor exits 3") {
    const Sandbox box;
    const ScopedEnv editor("EDITOR", "noteapp-no-such-editor-xyz");
    const ScopedEnv visual("VISUAL", std::nullopt);
    const auto result = box.run({"-e"});
    CHECK(result.exit_code == 3);
    CHECK_FALSE(result.err.empty());
    CHECK_FALSE(box.has_any_note());
}

TEST_CASE("editor flow: failing to save keeps the temp file and prints its path") {
    const Sandbox box;
    const EditorEnv env;
    const TempDir out;
    const ScopedEnv args_out("FAKE_EDITOR_ARGS_OUT", utf8(out.path() / "args.txt"));
    const ScopedEnv content("FAKE_EDITOR_CONTENT", "kurtarılacak metin");

    // Geçersiz klasör, editörden SONRA reddedilir: yazılan metin yine de kaybolmamalı.
    const auto result = box.run({"-e", "-f", "a/../b"});
    CHECK(result.exit_code != 0);
    CHECK_FALSE(box.has_any_note());
    std::string temp_path = slurp(out.path() / "args.txt");
    temp_path = temp_path.substr(0, temp_path.find('\n'));
    const fs::path temp(std::u8string(temp_path.begin(), temp_path.end()));
    REQUIRE(fs::exists(temp));
    CHECK(slurp(temp) == "kurtarılacak metin");
    CHECK(result.err.find(temp_path) != std::string::npos);
    std::error_code ec;
    fs::remove(temp, ec);
}
