// app::update_install: kurulum türü tespiti ve doğrulanmış paketin kurulumu. Hepsi geçici dizinde; yerel
// kurulum testleri geçici HOME ile gerçek scripts/install-local.sh'i çalıştırır, Windows zip çekirdeği sahte
// arşiv işlemleriyle her platformda koşar (Windows'ta ad değiştirme gerçek MoveFileExW'dir).
#include "app/update_install.hpp"
#include "core/i18n.hpp"
#include "core/paths.hpp"
#include "core/process.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

using noteapp::app::asset_kind;
using noteapp::app::AssetKind;
using noteapp::app::cleanup_after_update;
using noteapp::app::detect_install_target;
using noteapp::app::EnvReader;
using noteapp::app::install_update;
using noteapp::app::install_windows_zip;
using noteapp::app::InstallKind;
using noteapp::app::InstallOutcome;
using noteapp::app::InstallTarget;
using noteapp::app::InstallLock;
using noteapp::app::kInstallLockName;
using noteapp::app::kReplacingMarker;
using noteapp::app::kRollbackFailedMarker;
using noteapp::app::kWindowsPackageMarker;
using noteapp::app::ReleaseInfo;
using noteapp::app::remove_tree;
using noteapp::app::ZipInstallOps;
using noteapp::core::Error;
using noteapp::core::UpdateLimits;
using noteapp::test::slurp;
using noteapp::test::TempDir;
namespace fs = std::filesystem;

namespace {

ReleaseInfo release(const std::string& version = "2.0.0") {
    ReleaseInfo info;
    info.version = version;
    info.tag = "v" + version;
    info.appimage.name = "noteapp-" + version + "-x86_64.AppImage";
    info.windows_zip.name = "noteapp-" + version + "-windows-x86_64.zip";
    info.sums.name = "SHA256SUMS";
    return info;
}

UpdateLimits limits() {
    UpdateLimits l;
    l.install_timeout = {60U * 1000U, "1min"};
    l.tool_output_bytes = {1024U * 1024U, "1MiB"};
    return l;
}

void write(const fs::path& file, const std::string& content) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << content;
}

void write_executable(const fs::path& file, const std::string& content) {
    write(file, content);
    fs::permissions(file, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                              fs::perms::others_read | fs::perms::others_exec);
}

EnvReader env_of(std::map<std::string, std::string> values) {
    return [values = std::move(values)](std::string_view name) -> std::optional<std::string> {
        const auto found = values.find(std::string(name));
        return found == values.end() ? std::nullopt : std::optional(found->second);
    };
}

std::string error_key(const auto& result) {
    REQUIRE_FALSE(result.has_value());
    return result.error().message_key;
}

std::string arg(const Error& error, std::string_view name) {
    for (const auto& [key, value] : error.args)
        if (key == name)
            return value;
    return {};
}

// `prefix`le başlayan girdiler: varsayılan uygulamanın geçici dizinleri, ".install." betiğinkiler.
std::vector<std::string> staging_dirs(const fs::path& dir, std::string_view prefix = ".noteapp-update-") {
    std::vector<std::string> found;
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.path().filename().string().starts_with(prefix))
            found.push_back(entry.path().filename().string());
    return found;
}

// Log birikir (aynı NOTEAPP_HOME'da önceki testler de yazar): yalnız `since` baytından sonrası okunur.
std::size_t log_size() {
    return slurp(noteapp::core::state_dir() / "log.txt").size();
}
std::string log_since(std::size_t since) {
    const std::string text = slurp(noteapp::core::state_dir() / "log.txt");
    return since <= text.size() ? text.substr(since) : text;
}

}  // namespace

TEST_CASE("update install: each install kind downloads its own asset, unsupported none") {
    CHECK(asset_kind(InstallKind::DirectAppImage) == AssetKind::AppImage);
    CHECK(asset_kind(InstallKind::LocalInstall) == AssetKind::AppImage);
    CHECK(asset_kind(InstallKind::WindowsZip) == AssetKind::WindowsZip);
    CHECK_FALSE(asset_kind(InstallKind::Unsupported).has_value());
}

TEST_CASE("update install: an unsupported installation is refused with the release page") {
    const auto result = install_update(InstallTarget{}, release(), "bytes", limits(), {});
    REQUIRE(error_key(result) == "update.unsupported_install");
    CHECK(arg(result.error(), "url") == "https://github.com/ismetdyn/noteapp/releases/tag/v2.0.0");
    noteapp::core::init_i18n("en");
    CHECK(noteapp::core::format_error(result.error()).find("releases/tag/v2.0.0") != std::string::npos);
    noteapp::core::init_i18n("tr");
    CHECK(noteapp::core::format_error(result.error()).find("releases/tag/v2.0.0") != std::string::npos);
    noteapp::core::init_i18n("en");
}

TEST_CASE("update install: no executable path is unsupported") {
    CHECK(detect_install_target({}, env_of({})) == InstallTarget{});
}

TEST_CASE("update install: every error key the installer produces has a catalog text in both languages") {
    for (const char* language : {"en", "tr"}) {
        noteapp::core::init_i18n(language);
        for (const char* key :
             {"update.install_cancelled", "update.unsupported_install", "update.install_script_failed",
              "update.link_not_updated", "update.package_layout", "update.version_mismatch",
              "update.extract_failed", "update.old_in_use", "update.replace_failed",
              "update.rollback_failed", "update.rollback_pending", "update.install_timeout",
              "update.output_too_large", "update.install_busy"}) {
            CAPTURE(language);
            CAPTURE(key);
            CHECK(noteapp::core::tr(key) != key);
        }
    }
    noteapp::core::init_i18n("en");
}

TEST_CASE("update install: a stop request before installing changes nothing") {
    const TempDir dir;
    const fs::path image = dir.path() / "noteapp.AppImage";
    write_executable(image, "old");
    std::stop_source stop;
    stop.request_stop();
    const auto result = install_update(InstallTarget{InstallKind::DirectAppImage, image}, release(), "new",
                                       limits(), stop.get_token());
    CHECK(error_key(result) == "update.install_cancelled");
    CHECK(slurp(image) == "old");
}

TEST_CASE("update install: removing a tree deletes links as themselves, never what they point to") {
    const TempDir dir;
    const fs::path outside = dir.path() / "outside";
    write(outside / "keep.txt", "mine");
    const fs::path tree = dir.path() / "tree";
    write(tree / "a/file.txt", "x");
    REQUIRE(noteapp::test::make_dir_link(outside, tree / "a/link"));
    REQUIRE(remove_tree(tree).has_value());
    CHECK_FALSE(fs::exists(tree));
    CHECK(slurp(outside / "keep.txt") == "mine");
    CHECK(remove_tree(tree).has_value());  // yoksa başarı
}

#ifndef _WIN32

TEST_CASE("update install: a directly run AppImage is detected only for a binary inside APPDIR") {
    const TempDir dir;
    const fs::path appdir = dir.path() / "mount";
    const fs::path exe = appdir / "usr/bin/notes";
    write_executable(exe, "");
    const fs::path image = dir.path() / "apps/noteapp-1.0.0-x86_64.AppImage";
    write_executable(image, "old");
    const std::string appdir_text = noteapp::test::utf8(appdir);
    const auto env = env_of({{"APPIMAGE", noteapp::test::utf8(image)}, {"APPDIR", appdir_text}});
    const InstallTarget expected{InstallKind::DirectAppImage, fs::canonical(image)};

    CHECK(detect_install_target(exe, env) == expected);
    // Ortam AppImage'dan miras kaldı ama ikili başka yerde: doğrudan AppImage değil.
    CHECK(detect_install_target(dir.path() / "elsewhere/note", env).kind == InstallKind::Unsupported);
    CHECK(detect_install_target(exe, env_of({{"APPIMAGE", noteapp::test::utf8(image)}})).kind ==
          InstallKind::Unsupported);

    // Kullanıcının bağı değil gerçek dosya değişir.
    const fs::path link = dir.path() / "noteapp";
    fs::create_symlink(image, link);
    const auto via_link = env_of({{"APPIMAGE", noteapp::test::utf8(link)}, {"APPDIR", appdir_text}});
    CHECK(detect_install_target(exe, via_link) == expected);
}

TEST_CASE("update install: the AppImage is replaced atomically and stays executable") {
    const TempDir dir;
    const fs::path image = dir.path() / "noteapp-1.0.0-x86_64.AppImage";
    write_executable(image, "old");
    const auto result =
        install_update(InstallTarget{InstallKind::DirectAppImage, image}, release(), "new", limits(), {});
    REQUIRE(result.has_value());
    CHECK(*result == InstallOutcome{.restart_required = false});
    CHECK(slurp(image) == "new");
    const fs::perms perms = fs::status(image).permissions();
    CHECK((perms & fs::perms::owner_exec) != fs::perms::none);
    CHECK((perms & fs::perms::others_exec) != fs::perms::none);
    // Geçici dosya kalmaz: dizinde yalnız paket var.
    CHECK(std::distance(fs::directory_iterator(dir.path()), fs::directory_iterator()) == 1);
}

namespace {

// install-local.sh düzeninde kurulu bir sürüm: AppRun, usr/bin ikilileri ve paketteki betik.
struct LocalHome {
    TempDir dir;
    fs::path home = dir.path() / "home";
    fs::path share = home / ".local/share/noteapp";
    fs::path bin = home / ".local/bin";
    noteapp::test::ScopedEnv home_env{"HOME", noteapp::test::utf8(home)};
    // Tamamlama ve menü dosyaları geçici HOME'a yazılsın.
    noteapp::test::ScopedEnv xdg_env{"XDG_DATA_HOME", std::string()};
    noteapp::test::ScopedEnv completion_env{"BASH_COMPLETION_USER_DIR", std::string()};

    LocalHome() { fs::create_directories(bin); }

    fs::path add_version(const std::string& version, bool packaged_script) {
        const fs::path dir = share / version;
        write_executable(dir / "AppRun", "#!/bin/sh\nexit 0\n");
        write_executable(dir / "usr/bin/note", "");
        if (packaged_script) {
            const fs::path packaged = dir / "usr/share/noteapp";
            fs::create_directories(packaged);
            fs::copy_file(NOTEAPP_INSTALL_LOCAL_SCRIPT, packaged / "install-local.sh");
            fs::copy_file(NOTEAPP_DESKTOP_TEMPLATE, packaged / "noteapp.desktop");
        }
        return dir;
    }
    EnvReader env() const { return env_of({{"HOME", noteapp::test::utf8(home)}}); }
};

// Sahte AppImage (tests/cli/install_local_test.sh deseni): açılınca `note --version`a `version` der.
// `extract_delay`: açma o kadar saniye sürer (iptal ve süre aşımı testleri); `completion_delay`: kurulumdan
// sonra betiğin istediği `note completion` o kadar sürer (değiştirme başladıktan sonraki iptal testi).
std::string fake_appimage(const std::string& version, int extract_delay = 0, int completion_delay = 0) {
    return "#!/bin/sh\n"
           "[ \"$1\" = --appimage-extract ] || exit 2\n"
           "sleep " +
           std::to_string(extract_delay) +
           "\n"
           "mkdir -p squashfs-root\n"
           "cat >squashfs-root/AppRun <<'INNER'\n"
           "#!/bin/sh\n"
           "case \"${0##*/}:$1\" in\n"
           "    AppRun:note) echo \"note " +
           version +
           "\" ;;\n"
           "    note:completion) sleep " +
           std::to_string(completion_delay) +
           "; exit 3 ;;\n"
           "    *) exit 3 ;;\n"
           "esac\n"
           "INNER\n"
           "chmod +x squashfs-root/AppRun\n";
}

}  // namespace

TEST_CASE("update install: a local install needs the install-local.sh layout and the packaged script") {
    LocalHome local;
    const fs::path running = local.add_version("1.0.0", true);
    const fs::path canonical_running = fs::canonical(running);
    CHECK(detect_install_target(running / "usr/bin/note", local.env()) ==
          InstallTarget{InstallKind::LocalInstall, canonical_running});
    CHECK(detect_install_target(running / "usr/bin/notes-gui", local.env()) ==
          InstallTarget{InstallKind::LocalInstall, canonical_running});

    // Betiği taşımayan eski paket, cmake ağacı (<sürüm>/bin), kaynaktan derleme ve sistem kurulumu
    // desteklenmez.
    const fs::path old = local.add_version("0.9.0", false);
    CHECK(detect_install_target(old / "usr/bin/note", local.env()).kind == InstallKind::Unsupported);
    CHECK(detect_install_target(running / "bin/note", local.env()).kind == InstallKind::Unsupported);
    CHECK(detect_install_target(local.dir.path() / "build/src/cli/note", local.env()).kind ==
          InstallKind::Unsupported);
    CHECK(detect_install_target("/usr/bin/note", local.env()).kind == InstallKind::Unsupported);
    CHECK(detect_install_target(running / "usr/bin/note", env_of({})).kind == InstallKind::Unsupported);
    // Gizli dizin (betiğin geçici dizini) sürüm değildir.
    const fs::path hidden = local.share / ".install.abc";
    fs::create_directories(hidden);
    fs::copy(running, hidden, fs::copy_options::recursive);
    CHECK(detect_install_target(hidden / "usr/bin/note", local.env()).kind == InstallKind::Unsupported);

    // Yazılamayan veri dizini: root izinleri aşar, yalnız root olmayan kullanıcıda sınanır.
    if (geteuid() != 0) {
        fs::permissions(local.share, fs::perms::owner_write, fs::perm_options::remove);
        CHECK(detect_install_target(running / "usr/bin/note", local.env()).kind == InstallKind::Unsupported);
        fs::permissions(local.share, fs::perms::owner_write, fs::perm_options::add);
    }
    CHECK(staging_dirs(local.share).empty());  // yazma denemesi iz bırakmaz
}

TEST_CASE("update install: a local install runs the packaged script, keeps two versions and the notes data") {
    LocalHome local;
    local.add_version("0.8.0", false);
    local.add_version("0.9.0", false);
    const fs::path running = local.add_version("1.0.0", true);
    fs::create_symlink(local.share / "1.0.0/AppRun", local.bin / "note");
    write(local.share / "graph.db", "notes");

    const InstallTarget target = detect_install_target(running / "usr/bin/notes", local.env());
    REQUIRE(target.kind == InstallKind::LocalInstall);
    const auto result = install_update(target, release("2.0.0"), fake_appimage("2.0.0"), limits(), {});
    REQUIRE(result.has_value());
    CHECK_FALSE(result->restart_required);

    CHECK(fs::read_symlink(local.bin / "note") == local.share / "2.0.0/AppRun");
    CHECK(fs::exists(local.share / "2.0.0/AppRun"));
    CHECK(fs::exists(running / "usr/share/noteapp/install-local.sh"));  // çalışan sürüm kalır
    CHECK_FALSE(fs::exists(local.share / "0.9.0"));
    CHECK_FALSE(fs::exists(local.share / "0.8.0"));
    CHECK(slurp(local.share / "graph.db") == "notes");
    CHECK(staging_dirs(local.share).empty());
}

TEST_CASE("update install: a package of another version is refused before anything changes") {
    LocalHome local;
    local.add_version("0.8.0", false);
    local.add_version("0.9.0", false);
    const fs::path running = local.add_version("1.0.0", true);
    fs::create_symlink(local.share / "1.0.0/AppRun", local.bin / "note");
    const InstallTarget target = detect_install_target(running / "usr/bin/note", local.env());
    REQUIRE(target.kind == InstallKind::LocalInstall);

    // Paket 3.0.0 der, release 2.0.0: betik değiştirmeden önce düşer (--expect-version).
    const std::size_t log_before = log_size();
    const auto mismatch = install_update(target, release("2.0.0"), fake_appimage("3.0.0"), limits(), {});
    REQUIRE(error_key(mismatch) == "update.install_script_failed");
    CHECK(arg(mismatch.error(), "code") == "1");
    CHECK(arg(mismatch.error(), "url") == "https://github.com/ismetdyn/noteapp/releases/tag/v2.0.0");
    CHECK(fs::read_symlink(local.bin / "note") == local.share / "1.0.0/AppRun");
    for (const char* version : {"0.8.0", "0.9.0", "1.0.0"}) {
        CAPTURE(version);
        CHECK(fs::exists(local.share / version / "AppRun"));  // hiçbir sürüm budanmadı
    }
    CHECK_FALSE(fs::exists(local.share / "3.0.0"));
    CHECK(staging_dirs(local.share).empty());
    // Betiğin hata satırı yalnız loga gider, kullanıcı iletisine değil.
    CHECK(log_since(log_before).find("the package is version 3.0.0, expected 2.0.0") != std::string::npos);
    CHECK(noteapp::core::format_error(mismatch.error()).find("expected") == std::string::npos);
}

TEST_CASE("update install: a stop request or the timeout kills the install script and changes nothing") {
    LocalHome local;
    const fs::path running = local.add_version("1.0.0", true);
    fs::create_symlink(local.share / "1.0.0/AppRun", local.bin / "note");
    const InstallTarget target = detect_install_target(running / "usr/bin/note", local.env());
    REQUIRE(target.kind == InstallKind::LocalInstall);
    constexpr int kSlowExtractSeconds = 30;
    constexpr auto kPromptly = std::chrono::seconds(10);

    SUBCASE("stop request while the script runs") {
        std::stop_source stop;
        const auto started = std::chrono::steady_clock::now();
        std::jthread canceller([&stop] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            stop.request_stop();
        });
        const auto result = install_update(target, release("2.0.0"),
                                           fake_appimage("2.0.0", kSlowExtractSeconds), limits(),
                                           stop.get_token());
        CHECK(error_key(result) == "update.install_cancelled");
        CHECK(std::chrono::steady_clock::now() - started < kPromptly);
    }
    SUBCASE("install timeout") {
        UpdateLimits short_limits = limits();
        short_limits.install_timeout = {300, "300ms"};
        const auto started = std::chrono::steady_clock::now();
        const auto result = install_update(target, release("2.0.0"),
                                           fake_appimage("2.0.0", kSlowExtractSeconds), short_limits, {});
        REQUIRE(error_key(result) == "update.install_timeout");
        CHECK(arg(result.error(), "limit") == "300ms");
        CHECK(std::chrono::steady_clock::now() - started < kPromptly);
    }
    CHECK(fs::read_symlink(local.bin / "note") == local.share / "1.0.0/AppRun");
    CHECK_FALSE(fs::exists(local.share / "2.0.0"));
    CHECK(staging_dirs(local.share).empty());
    // Önce SIGTERM: betiğin EXIT tuzağı kendi geçici dizinini siler.
    CHECK(staging_dirs(local.share, ".install.").empty());
}

TEST_CASE("update install: optional steps hanging after the link switch are stopped, the update succeeds") {
    LocalHome local;
    const fs::path running = local.add_version("1.0.0", true);
    fs::create_symlink(local.share / "1.0.0/AppRun", local.bin / "note");
    const InstallTarget target = detect_install_target(running / "usr/bin/note", local.env());
    REQUIRE(target.kind == InstallKind::LocalInstall);
    // `note completion` bağlar değiştikten sonra çağrılır ve takılır.
    constexpr int kHangingCompletionSeconds = 60;
    constexpr auto kPromptly = std::chrono::seconds(20);
    const std::size_t log_before = log_size();
    const auto started = std::chrono::steady_clock::now();

    SUBCASE("install timeout") {
        UpdateLimits short_limits = limits();
        short_limits.install_timeout = {5000, "5s"};
        const auto result = install_update(target, release("2.0.0"),
                                           fake_appimage("2.0.0", 0, kHangingCompletionSeconds), short_limits,
                                           {});
        REQUIRE(result.has_value());
    }
    SUBCASE("stop request") {
        std::stop_source stop;
        std::jthread canceller([&stop, share = local.share](std::stop_token done) {
            while (!done.stop_requested()) {
                std::error_code ec;
                for (fs::directory_iterator it(share, ec), end; !ec && it != end; it.increment(ec))
                    if (fs::exists(it->path() / "progress/committed", ec)) {
                        stop.request_stop();
                        return;
                    }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
        const auto result = install_update(target, release("2.0.0"),
                                           fake_appimage("2.0.0", 0, kHangingCompletionSeconds), limits(),
                                           stop.get_token());
        canceller.request_stop();
        CHECK(stop.stop_requested());
        REQUIRE(result.has_value());
    }
    CHECK(std::chrono::steady_clock::now() - started < kPromptly);
    CHECK(fs::read_symlink(local.bin / "note") == local.share / "2.0.0/AppRun");
    CHECK(log_since(log_before).find("event=update_install_optional_steps_stopped") != std::string::npos);
    CHECK(staging_dirs(local.share).empty());
    CHECK(staging_dirs(local.share, ".install.").empty());
}

TEST_CASE("update install: a stop request after the script started replacing is ignored") {
    LocalHome local;
    const fs::path running = local.add_version("1.0.0", true);
    fs::create_symlink(local.share / "1.0.0/AppRun", local.bin / "note");
    const InstallTarget target = detect_install_target(running / "usr/bin/note", local.env());
    REQUIRE(target.kind == InstallKind::LocalInstall);
    constexpr int kSlowCompletionSeconds = 2;

    // Betik `committing` işaretini yazınca iptal istenir; o andan sonra kurulum sona dek sürmeli.
    std::stop_source stop;
    std::jthread canceller([&stop, share = local.share](std::stop_token done) {
        while (!done.stop_requested()) {
            // Dizinler bu sırada yaratılıp silinir: istisnasız yineleme.
            std::error_code ec;
            for (fs::directory_iterator it(share, ec), end; !ec && it != end; it.increment(ec))
                if (fs::exists(it->path() / "progress/committing", ec)) {
                    stop.request_stop();
                    return;
                }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    const auto result = install_update(target, release("2.0.0"),
                                       fake_appimage("2.0.0", 0, kSlowCompletionSeconds), limits(),
                                       stop.get_token());
    canceller.request_stop();
    CHECK(stop.stop_requested());
    REQUIRE(result.has_value());
    CHECK(fs::read_symlink(local.bin / "note") == local.share / "2.0.0/AppRun");
}

TEST_CASE("update install: cleanup removes only stale local staging directories") {
    LocalHome local;
    const fs::path running = local.add_version("1.0.0", true);
    fs::create_directories(local.share / ".noteapp-update-stale/x");
    fs::create_directories(local.share / ".noteapp-update-fresh");
    fs::create_directories(local.share / ".update-users-own");
    // install-local.sh'in öldürülmüş kurulumundan kalan dizin; tazesi çalışan bir kurulumun olabilir.
    fs::create_directories(local.share / ".install.stale/squashfs-root");
    fs::create_directories(local.share / ".install.fresh");
    for (const char* stale : {".noteapp-update-stale", ".install.stale"})
        fs::last_write_time(local.share / stale, fs::file_time_type::clock::now() - std::chrono::hours(2));
    fs::last_write_time(local.share / ".update-users-own",
                        fs::file_time_type::clock::now() - std::chrono::hours(2));
    write(local.share / "graph.db", "notes");
    cleanup_after_update(InstallTarget{InstallKind::LocalInstall, fs::canonical(running)});
    CHECK(staging_dirs(local.share) == std::vector<std::string>{".noteapp-update-fresh"});
    CHECK(staging_dirs(local.share, ".install.") == std::vector<std::string>{".install.fresh"});
    CHECK(fs::exists(local.share / ".update-users-own"));  // uygulamanın öneki değil
    CHECK(fs::exists(running / "AppRun"));
    CHECK(slurp(local.share / "graph.db") == "notes");
}

#else

TEST_CASE("update install: a writable directory with the programs, licenses and marker is a zip install") {
    const TempDir dir;
    for (const char* exe : {"note.exe", "notes.exe", "notes-gui.exe"})
        write(dir.path() / exe, "exe");
    fs::create_directories(dir.path() / "licenses");
    write(dir.path() / kWindowsPackageMarker, "noteapp");
    // AppImage ortamı Windows'ta anlamsızdır.
    const auto env = env_of({{"APPIMAGE", "C:/x.AppImage"}, {"APPDIR", noteapp::test::utf8(dir.path())}});
    const fs::path canonical_dir = fs::weakly_canonical(dir.path());
    CHECK(detect_install_target(dir.path() / "notes.exe", env) ==
          InstallTarget{InstallKind::WindowsZip, canonical_dir});
    CHECK(staging_dirs(dir.path()).empty());

    // Sahiplik işareti yoksa (zip elle ortak bir klasöre açıldı ya da başka bir şey) dizine dokunulmaz.
    fs::remove(dir.path() / kWindowsPackageMarker);
    CHECK(detect_install_target(dir.path() / "notes.exe", env).kind == InstallKind::Unsupported);
    write(dir.path() / kWindowsPackageMarker, "noteapp");
    fs::remove(dir.path() / "notes-gui.exe");
    CHECK(detect_install_target(dir.path() / "notes.exe", env).kind == InstallKind::Unsupported);
    write(dir.path() / "notes-gui.exe", "exe");
    fs::remove(dir.path() / "licenses");
    CHECK(detect_install_target(dir.path() / "notes.exe", env).kind == InstallKind::Unsupported);
}

TEST_CASE("update install: the real tool runner bounds a child's output") {
    // Test ikilisinin kendisi (doctest) `--version`a stdout'ta birkaç satır basar.
    const fs::path self = noteapp::core::current_executable();
    UpdateLimits tiny = limits();
    tiny.tool_output_bytes = {8, "8B"};
    const auto bounded = noteapp::app::windows_zip_ops(tiny).version_output(self, {});
    REQUIRE(error_key(bounded) == "update.output_too_large");
    CHECK(arg(bounded.error(), "limit") == "8B");
    const auto full = noteapp::app::windows_zip_ops(limits()).version_output(self, {});
    REQUIRE(full.has_value());
    CHECK(full->find("doctest") != std::string::npos);
}

#endif

namespace {

using Files = std::map<std::string, std::string>;

constexpr std::array kOldFiles = {"note.exe.old", "notes.exe.old", "notes-gui.exe.old",
                                  "noteapp-package.txt.old", "licenses.old"};

// Kurulu Windows dizini ve sahte arşiv: `list` adları, `extract` dosyaları verir; ad değiştirme ve silme
// Windows'ta gerçek (MoveFileExW, remove_tree), başka yerde aynı sözleşmeli std::filesystem karşılıkları.
struct ZipFixture {
    TempDir dir;
    fs::path exe_dir = dir.path() / "noteapp";
    std::vector<std::string> names = {"note.exe",
                                      "notes.exe",
                                      "notes-gui.exe",
                                      std::string(kWindowsPackageMarker),
                                      "licenses/",
                                      "licenses/a/LICENSE"};
    Files package = {{"note.exe", "new note"},
                     {"notes.exe", "new notes"},
                     {"notes-gui.exe", "new gui"},
                     {std::string(kWindowsPackageMarker), "new marker"},
                     {"licenses/a/LICENSE", "new license"}};
    std::string version = "note 2.0.0\r\n";
    // Düşürülecek ad değiştirme: (kaynak adı, hedef adı) eşleşmesi.
    std::vector<std::pair<std::string, std::string>> failing_renames;
    std::vector<std::string> failing_removes;  // kullanımda gibi davranan yedekler
    std::function<void()> on_version;          // version_output içinden çağrılır (iptal testleri)
    bool extracted = false;
    std::vector<std::string> rename_log;  // "kaynak>hedef"; geçici dizindekiler "new/" önekli

    ZipFixture() {
        write(exe_dir / "note.exe", "old note");
        write(exe_dir / "notes.exe", "old notes");
        write(exe_dir / "notes-gui.exe", "old gui");
        write(exe_dir / kWindowsPackageMarker, "old marker");
        write(exe_dir / "licenses/a/LICENSE", "old license");
    }

    static std::string label(const fs::path& path) {
        const std::string name = path.filename().string();
        return path.parent_path().filename() == "new" ? "new/" + name : name;
    }

    static std::expected<void, Error> real_rename(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
        return noteapp::app::windows_zip_ops(limits()).rename(from, to);
#else
        std::error_code ec;
        if (fs::exists(fs::symlink_status(to, ec)))
            return std::unexpected(Error{Error::Code::AlreadyExists, "update.replace_failed", {}});
        fs::rename(from, to, ec);
        if (ec)
            return std::unexpected(Error{Error::Code::Io, "update.replace_failed", {}});
        return {};
#endif
    }

    ZipInstallOps ops() {
        ZipInstallOps result;
        result.list = [this](const fs::path& zip,
                             std::stop_token) -> std::expected<std::vector<std::string>, Error> {
            CHECK(slurp(zip) == "zip bytes");
            return names;
        };
        result.extract = [this](const fs::path&, const fs::path& destination,
                                std::stop_token) -> std::expected<void, Error> {
            extracted = true;
            for (const auto& [name, content] : package)
                write(destination / name, content);
            return {};
        };
        result.version_output = [this](const fs::path& exe,
                                       std::stop_token) -> std::expected<std::string, Error> {
            CHECK(slurp(exe) == package["note.exe"]);
            if (on_version)
                on_version();
            return version;
        };
        result.rename = [this](const fs::path& from, const fs::path& to) -> std::expected<void, Error> {
            rename_log.push_back(label(from) + ">" + label(to));
            for (const auto& [failing_from, failing_to] : failing_renames)
                if (label(from) == failing_from && label(to) == failing_to)
                    return std::unexpected(Error{Error::Code::Io, "update.replace_failed", {{"code", "5"}}});
            return real_rename(from, to);
        };
        result.remove = [this](const fs::path& path) -> std::expected<void, Error> {
            for (const std::string& name : failing_removes)
                if (path.filename() == name && fs::exists(path))
                    return std::unexpected(Error{Error::Code::Io, "error.io", {}});
            return remove_tree(path);
        };
        return result;
    }

    std::expected<InstallOutcome, Error> install(std::stop_token stop = {}) {
        return install_windows_zip(exe_dir, release("2.0.0"), "zip bytes", ops(), stop);
    }

    // Kurulu sürüm hiç değişmedi: eski içerik, .old ve geçici dizin yok.
    void check_unchanged() {
        CHECK(slurp(exe_dir / "note.exe") == "old note");
        CHECK(slurp(exe_dir / "notes.exe") == "old notes");
        CHECK(slurp(exe_dir / "notes-gui.exe") == "old gui");
        CHECK(slurp(exe_dir / kWindowsPackageMarker) == "old marker");
        CHECK(slurp(exe_dir / "licenses/a/LICENSE") == "old license");
        for (const char* old : kOldFiles)
            CHECK_FALSE(fs::exists(exe_dir / old));
        CHECK(staging_dirs(exe_dir).empty());
    }
};

}  // namespace

TEST_CASE("update install: the Windows package replaces programs, marker and licenses, asks for a restart") {
    ZipFixture fixture;
    const auto result = fixture.install();
    REQUIRE(result.has_value());
    CHECK(result->restart_required);
    CHECK(slurp(fixture.exe_dir / "note.exe") == "new note");
    CHECK(slurp(fixture.exe_dir / "notes.exe") == "new notes");
    CHECK(slurp(fixture.exe_dir / "notes-gui.exe") == "new gui");
    CHECK(slurp(fixture.exe_dir / kWindowsPackageMarker) == "new marker");
    CHECK(slurp(fixture.exe_dir / "licenses/a/LICENSE") == "new license");
    // Çalışan exe silinemez: eskisi .old olarak kalır, açılıştaki temizlik siler.
    CHECK(slurp(fixture.exe_dir / "note.exe.old") == "old note");
    CHECK(slurp(fixture.exe_dir / "licenses.old/a/LICENSE") == "old license");
    CHECK(staging_dirs(fixture.exe_dir).empty());

    fs::create_directories(fixture.exe_dir / ".noteapp-update-fresh");
#ifndef _WIN32
    // MinGW libstdc++ dizinin zamanını değiştiremez (_wutime, Permission denied): bayat dizin yalnız burada
    // kurulur.
    fs::create_directories(fixture.exe_dir / ".noteapp-update-stale");
    fs::last_write_time(fixture.exe_dir / ".noteapp-update-stale",
                        fs::file_time_type::clock::now() - std::chrono::hours(2));
#endif
    cleanup_after_update(InstallTarget{InstallKind::WindowsZip, fixture.exe_dir});
    for (const char* old : kOldFiles)
        CHECK_FALSE(fs::exists(fixture.exe_dir / old));
    CHECK(staging_dirs(fixture.exe_dir) == std::vector<std::string>{".noteapp-update-fresh"});
    CHECK(slurp(fixture.exe_dir / "note.exe") == "new note");

    // Bir sonraki güncelleme önceki güncellemenin .old artığının üzerine kurulur.
    ZipFixture again;
    REQUIRE(again.install().has_value());
    again.package["note.exe"] = "newer note";
    again.version = "note 2.0.0\n";
    REQUIRE(again.install().has_value());
    CHECK(slurp(again.exe_dir / "note.exe") == "newer note");
    CHECK(slurp(again.exe_dir / "note.exe.old") == "new note");
}

TEST_CASE("update install: archive entries outside the allowlist are refused before unpacking") {
    const std::vector<std::string> complete = {"note.exe", "notes.exe", "notes-gui.exe",
                                               std::string(kWindowsPackageMarker)};
    std::vector<std::vector<std::string>> cases;
    for (const char* extra :
         {"../evil.exe", "licenses/../../evil.exe", "/etc/evil", "C:/Windows/evil.exe",
          "licenses\\..\\evil.exe", "note.exe:stream", "./note.exe", "extra.dll", "NOTE.EXE", "note.exe/x",
          "",
          // Windows sondaki noktayı ve boşluğu atar, aygıt adı dosya değildir.
          "note.exe.", "notes.exe ", "licenses/a./b", "licenses/a /b", "licenses/x.", "licenses/CON",
          "licenses/nul.txt", "licenses/a/com1", "licenses/LPT9.log", "licenses/Aux", "licenses/prn.x.y"}) {
        cases.push_back(complete);
        cases.back().emplace_back(extra);
    }
    cases.push_back({"note.exe", "notes.exe", std::string(kWindowsPackageMarker), "licenses/"});  // exe eksik
    cases.push_back({"note.exe", "notes.exe", "notes-gui.exe", "licenses/"});                  // işaret eksik
    for (const std::vector<std::string>& names : cases) {
        ZipFixture fixture;
        fixture.names = names;
        CAPTURE(names.back());
        CHECK(error_key(fixture.install()) == "update.package_layout");
        CHECK_FALSE(fixture.extracted);
        fixture.check_unchanged();
    }
    // Aygıt adına benzeyen sıradan adlar geçer.
    ZipFixture lookalike;
    lookalike.names.insert(lookalike.names.end(), {"licenses/console.txt", "licenses/COM10", "licenses/lpt"});
    CHECK(lookalike.install().has_value());
}

TEST_CASE("update install: an unpacked tree with extra or missing files is refused") {
    ZipFixture extra;
    extra.package["evil.dll"] = "x";
    CHECK(error_key(extra.install()) == "update.package_layout");
    extra.check_unchanged();

    for (const char* name : {"notes-gui.exe", "licenses/a/LICENSE", "noteapp-package.txt"}) {
        CAPTURE(name);
        ZipFixture missing;
        missing.package.erase(name);
        CHECK(error_key(missing.install()) == "update.package_layout");
        missing.check_unchanged();
    }
}

TEST_CASE("update install: a link in the unpacked tree is refused and its target is left alone") {
    for (const bool inside : {false, true}) {
        CAPTURE(inside);
        ZipFixture fixture;
        const fs::path outside = fixture.dir.path() / "outside";
        write(outside / "keep.txt", "mine");
        ZipInstallOps ops = fixture.ops();
        const auto extract = ops.extract;
        ops.extract = [&](const fs::path& zip, const fs::path& destination,
                          std::stop_token stop) -> std::expected<void, Error> {
            if (auto done = extract(zip, destination, stop); !done)
                return done;
            // İçeriyi gösteren bağ da bağdır; dışarıyı gösterenin hedefi geçici dizinle birlikte silinmemeli.
            const fs::path target = inside ? destination / "licenses/a" : outside;
            REQUIRE(noteapp::test::make_dir_link(target, destination / "licenses/link"));
            return {};
        };
        CHECK(error_key(install_windows_zip(fixture.exe_dir, release("2.0.0"), "zip bytes", ops, {})) ==
              "update.package_layout");
        fixture.check_unchanged();
        CHECK(slurp(outside / "keep.txt") == "mine");
    }
}

TEST_CASE("update install: a package reporting another version changes nothing") {
    ZipFixture fixture;
    fixture.version = "note 1.9.0\n";
    const auto result = fixture.install();
    REQUIRE(error_key(result) == "update.version_mismatch");
    CHECK(arg(result.error(), "expected") == "2.0.0");
    fixture.check_unchanged();
}

TEST_CASE("update install: a failed replacement is rolled back in reverse order") {
    SUBCASE("moving a new program in") {
        ZipFixture fixture;
        fixture.failing_renames = {{"new/notes-gui.exe", "notes-gui.exe"}};
        const auto result = fixture.install();
        REQUIRE(error_key(result) == "update.replace_failed");
        CHECK(arg(result.error(), "code") == "5");
        fixture.check_unchanged();
        const std::vector<std::string> expected = {
            "note.exe>note.exe.old", "new/note.exe>note.exe", "notes.exe>notes.exe.old",
            "new/notes.exe>notes.exe",
            "notes-gui.exe>notes-gui.exe.old", "new/notes-gui.exe>notes-gui.exe",
            // geri alma: tersinden
            "notes-gui.exe.old>notes-gui.exe", "notes.exe>new/notes.exe", "notes.exe.old>notes.exe",
            "note.exe>new/note.exe", "note.exe.old>note.exe"};
        CHECK(fixture.rename_log == expected);
    }
    SUBCASE("moving the licenses aside") {
        ZipFixture fixture;
        fixture.failing_renames = {{"licenses", "licenses.old"}};
        CHECK(error_key(fixture.install()) == "update.replace_failed");
        fixture.check_unchanged();
    }
}

TEST_CASE("update install: a backup still in use stops the update before anything moves") {
    ZipFixture fixture;
    write(fixture.exe_dir / "notes.exe.old", "running older notes");  // eski pencere hâlâ açık
    fixture.failing_removes = {"notes.exe.old"};
    const auto result = fixture.install();
    REQUIRE(error_key(result) == "update.old_in_use");
    CHECK(arg(result.error(), "path").ends_with("notes.exe.old"));
    CHECK(slurp(fixture.exe_dir / "note.exe") == "old note");
    CHECK(slurp(fixture.exe_dir / "notes.exe") == "old notes");
    CHECK(slurp(fixture.exe_dir / "notes.exe.old") == "running older notes");
    CHECK_FALSE(fs::exists(fixture.exe_dir / "note.exe.old"));
}

TEST_CASE("update install: a failed rollback is marked and its backups survive cleanup and new updates") {
    ZipFixture fixture;
    fixture.failing_renames = {{"new/licenses", "licenses"}, {"note.exe.old", "note.exe"}};
    const std::size_t log_before = log_size();
    const auto result = fixture.install();
    REQUIRE(error_key(result) == "update.rollback_failed");
    CHECK(arg(result.error(), "url") == "https://github.com/ismetdyn/noteapp/releases/tag/v2.0.0");
    REQUIRE(fs::exists(fixture.exe_dir / kRollbackFailedMarker));
    CHECK(slurp(fixture.exe_dir / "note.exe.old") == "old note");  // tek kurtarma yolu
    CHECK(slurp(fixture.exe_dir / "note.exe") == "new note");      // yer boş kalmaz
    // Asıl hata (ilk düşen adım) anahtarı ve argümanlarıyla loglanır.
    CHECK(log_since(log_before).find("cause=update.replace_failed cause_args=\"code=5\"") !=
          std::string::npos);

    cleanup_after_update(InstallTarget{InstallKind::WindowsZip, fixture.exe_dir});
    CHECK(slurp(fixture.exe_dir / "note.exe.old") == "old note");

    fixture.failing_renames.clear();
    const auto again = fixture.install();
    REQUIRE(error_key(again) == "update.rollback_pending");
    CHECK(slurp(fixture.exe_dir / "note.exe.old") == "old note");
}

TEST_CASE("update install: a stop request during the Windows swap is ignored") {
    ZipFixture fixture;
    std::stop_source stop;
    ZipInstallOps ops = fixture.ops();
    const auto rename = ops.rename;
    ops.rename = [&](const fs::path& from, const fs::path& to) {
        stop.request_stop();  // ilk ad değişikliğinden sonra yarıda kesmek bozuk kurulum bırakırdı
        return rename(from, to);
    };
    const auto result =
        install_windows_zip(fixture.exe_dir, release("2.0.0"), "zip bytes", ops, stop.get_token());
    REQUIRE(result.has_value());
    CHECK(slurp(fixture.exe_dir / "licenses/a/LICENSE") == "new license");
}

TEST_CASE("update install: a stop request before the swap leaves the Windows install unchanged") {
    SUBCASE("before the call") {
        ZipFixture fixture;
        std::stop_source stop;
        stop.request_stop();
        CHECK(error_key(fixture.install(stop.get_token())) == "update.install_cancelled");
        CHECK(fixture.rename_log.empty());
        fixture.check_unchanged();
    }
    SUBCASE("during the last step before the swap") {
        ZipFixture fixture;
        std::stop_source stop;
        fixture.on_version = [&stop] { stop.request_stop(); };
        CHECK(error_key(fixture.install(stop.get_token())) == "update.install_cancelled");
        CHECK(fixture.extracted);
        CHECK(fixture.rename_log.empty());
        fixture.check_unchanged();
    }
}

// --- Süreçler arası güvenlik: kurulum kilidi ve değiştirme işareti ---

TEST_CASE("update install: cleanup keeps the backups while another process holds the install lock") {
    ZipFixture fixture;
    REQUIRE(fixture.install().has_value());
    REQUIRE(fs::exists(fixture.exe_dir / "note.exe.old"));
    {
        const auto lock = InstallLock::try_acquire(fixture.exe_dir);  // başka sürecin süren kurulumu
        REQUIRE(lock.has_value());
        cleanup_after_update(InstallTarget{InstallKind::WindowsZip, fixture.exe_dir});
        for (const char* old : kOldFiles)
            CHECK(fs::exists(fixture.exe_dir / old));
    }
    cleanup_after_update(InstallTarget{InstallKind::WindowsZip, fixture.exe_dir});
    for (const char* old : kOldFiles)
        CHECK_FALSE(fs::exists(fixture.exe_dir / old));
    CHECK_FALSE(fs::exists(fixture.exe_dir / kInstallLockName));  // kilit iz bırakmaz
}

TEST_CASE("update install: cleanup keeps the backups and staging while a replacing marker is left") {
    ZipFixture fixture;
    REQUIRE(fixture.install().has_value());
    write(fixture.exe_dir / kReplacingMarker, "2.0.0\n");  // değiştirmenin ortasında ölmüş süreç
#ifndef _WIN32
    fs::create_directories(fixture.exe_dir / ".noteapp-update-stale");
    fs::last_write_time(fixture.exe_dir / ".noteapp-update-stale",
                        fs::file_time_type::clock::now() - std::chrono::hours(2));
#endif
    cleanup_after_update(InstallTarget{InstallKind::WindowsZip, fixture.exe_dir});
    for (const char* old : kOldFiles)
        CHECK(fs::exists(fixture.exe_dir / old));
#ifndef _WIN32
    CHECK(fs::exists(fixture.exe_dir / ".noteapp-update-stale"));
#endif
}

TEST_CASE("update install: a second installer of the same directory is refused as busy") {
    ZipFixture fixture;
    const auto lock = InstallLock::try_acquire(fixture.exe_dir);
    REQUIRE(lock.has_value());
    CHECK(error_key(InstallLock::try_acquire(fixture.exe_dir)) == "update.install_busy");
    const auto result = fixture.install();
    REQUIRE(error_key(result) == "update.install_busy");
    CHECK(arg(result.error(), "path") == noteapp::test::utf8(fixture.exe_dir));
    CHECK_FALSE(fixture.extracted);
    fixture.check_unchanged();
}

TEST_CASE("update install: a replacing marker left by a killed update blocks the next install") {
    ZipFixture fixture;
    write(fixture.exe_dir / kReplacingMarker, "2.0.0\n");
    const auto result = fixture.install();
    REQUIRE(error_key(result) == "update.rollback_pending");
    CHECK(arg(result.error(), "marker") == std::string(kReplacingMarker));
    CHECK(fixture.rename_log.empty());
    CHECK(slurp(fixture.exe_dir / "note.exe") == "old note");
}

TEST_CASE("update install: the replacing marker exists exactly while the Windows swap runs") {
    SUBCASE("success removes it") {
        ZipFixture fixture;
        ZipInstallOps ops = fixture.ops();
        const auto rename = ops.rename;
        std::vector<bool> marked;
        ops.rename = [&](const fs::path& from, const fs::path& to) {
            marked.push_back(fs::exists(fixture.exe_dir / kReplacingMarker));
            return rename(from, to);
        };
        REQUIRE(install_windows_zip(fixture.exe_dir, release("2.0.0"), "zip bytes", ops, {}).has_value());
        REQUIRE_FALSE(marked.empty());
        CHECK(std::ranges::all_of(marked, [](bool is_marked) { return is_marked; }));
        CHECK_FALSE(fs::exists(fixture.exe_dir / kReplacingMarker));
    }
    SUBCASE("a complete rollback removes it") {
        ZipFixture fixture;
        fixture.failing_renames = {{"new/licenses", "licenses"}};
        CHECK(error_key(fixture.install()) == "update.replace_failed");
        CHECK_FALSE(fs::exists(fixture.exe_dir / kReplacingMarker));
        fixture.check_unchanged();
    }
}

#ifndef _WIN32
TEST_CASE("update install: a local install is refused as busy while another holds the lock") {
    LocalHome local;
    const fs::path running = local.add_version("1.0.0", true);
    fs::create_symlink(local.share / "1.0.0/AppRun", local.bin / "note");
    const InstallTarget target = detect_install_target(running / "usr/bin/note", local.env());
    REQUIRE(target.kind == InstallKind::LocalInstall);
    const auto lock = InstallLock::try_acquire(local.share);
    REQUIRE(lock.has_value());
    CHECK(error_key(install_update(target, release("2.0.0"), fake_appimage("2.0.0"), limits(), {})) ==
          "update.install_busy");
    CHECK(fs::read_symlink(local.bin / "note") == local.share / "1.0.0/AppRun");

    fs::create_directories(local.share / ".noteapp-update-stale");
    fs::last_write_time(local.share / ".noteapp-update-stale",
                        fs::file_time_type::clock::now() - std::chrono::hours(2));
    cleanup_after_update(target);
    CHECK(fs::exists(local.share / ".noteapp-update-stale"));  // kilit tutulurken dokunulmaz
}
#endif
