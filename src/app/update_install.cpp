#include "app/update_install.hpp"

#include "core/atomic_file.hpp"
#include "core/log.hpp"
#include "core/process.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace noteapp::app {

namespace {

namespace fs = std::filesystem;
using std::chrono::steady_clock;

// Geçici dizinler hedefin yanında açılır: aynı dosya sisteminde rename atomiktir. Ad öneki temizliğin
// anahtarıdır; kullanıcının kendi `.update-*` dizinleriyle karışmasın diye uygulamaya özgüdür.
constexpr std::string_view kStagingPrefix = ".noteapp-update-";
constexpr int kStagingAttempts = 16;
// Kesilmiş güncellemenin artığı; daha tazesi çalışan başka bir güncellemenin olabilir (install-local.sh ile
// aynı).
constexpr auto kStaleStagingAge = std::chrono::hours(1);
// Çocuk süreç bekleme adımı: iptal ve süre aşımına tepki süresi, kapasite değil.
constexpr auto kPollInterval = std::chrono::milliseconds(100);
// Durdurmada SIGTERM'den sonra SIGKILL'e dek beklenen süre: betiğin EXIT tuzağı kendi geçici dizinini
// (.install.*) silebilsin. Protokol sabiti: tuzak yalnız `rm -rf` koşar.
constexpr auto kTerminateGrace = std::chrono::seconds(2);

// Yerel kurulum düzeni (scripts/install-local.sh):
// <HOME>/.local/share/noteapp/<sürüm>/{AppRun,usr/bin/<exe>}. Betiği pakete
// packaging/linux/build-in-container.sh koyar.
constexpr std::string_view kLocalShare = ".local/share/noteapp";
constexpr std::string_view kAppRun = "AppRun";
constexpr std::string_view kInstallScript = "usr/share/noteapp/install-local.sh";
// Onaylı karar: güncellemeden sonra en yeni iki sürüm dizini kalır (geri dönüş için bir önceki).
constexpr int kKeptVersions = 2;
// install-local.sh --progress-dir sözleşmesi.
constexpr std::string_view kProgressDir = "progress";
constexpr std::string_view kCommittingFile = "committing";  // değiştirme başladı
constexpr std::string_view kCommittedFile = "committed";    // bağlar yeni sürümde; kalan adımlar isteğe bağlı
// Betiğin kendi geçici dizini (install-local.sh `.install.XXXXXX`): betik öldürülürse artığı açılışta
// silinir.
constexpr std::string_view kScriptStagingPrefix = ".install.";
constexpr std::string_view kStderrFile = "stderr.log";

// Windows zip düzeni (scripts/make-release.sh): kökte üç exe, sahiplik işareti ve licenses/.
constexpr std::array<std::string_view, 3> kWindowsExes = {"note.exe", "notes.exe", "notes-gui.exe"};
constexpr std::string_view kLicensesDir = "licenses";
constexpr std::string_view kOldSuffix = ".old";
constexpr std::string_view kZipName = "package.zip";
constexpr std::string_view kExtractDir = "new";
// Windows aygıt adları: uzantılı hâli de (NUL.txt) dosya değil aygıttır.
constexpr std::array<std::string_view, 4> kDeviceNames = {"CON", "PRN", "AUX", "NUL"};
constexpr std::array<std::string_view, 2> kNumberedDeviceNames = {"COM", "LPT"};

core::Error install_error(std::string key, std::vector<std::pair<std::string, std::string>> args = {}) {
    return core::Error{core::Error::Code::Io, std::move(key), std::move(args)};
}

std::string to_utf8(const fs::path& path) {
    const std::u8string u = path.u8string();
    return {u.begin(), u.end()};
}

fs::path from_utf8(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

core::Error io_error(const fs::path& path, const std::error_code& ec) {
    return install_error("error.io", {{"path", to_utf8(path)}, {"detail", ec.message()}});
}

// `path` `root`un içinde (kendisi değil); ikisi de aynı biçimde (kanonik) verilir.
bool is_inside(const fs::path& path, const fs::path& root) {
    const fs::path relative = path.lexically_relative(root);
    return !relative.empty() && relative != "." && *relative.begin() != "..";
}

// Sembolik bağ ya da (Windows) junction gibi herhangi bir reparse point: açılan pakette hiçbiri olmamalı.
bool is_link(const fs::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(path, ec)) || ec;
#endif
}

std::string old_name(std::string_view item) {
    return std::string(item) + std::string(kOldSuffix);
}

// `<parent>/.noteapp-update-XXXXXXXX`, yalnız sahibine açık (POSIX 0700); yıkıcı içeriğiyle, bağ izlemeden
// siler.
class StagingDir {
public:
    static std::expected<StagingDir, core::Error> create(const fs::path& parent) {
        std::random_device random;
        std::error_code ec;
        for (int attempt = 0; attempt < kStagingAttempts; ++attempt) {
            const fs::path candidate = parent / std::format("{}{:08x}", kStagingPrefix, random());
            if (fs::create_directory(candidate, ec)) {
                StagingDir staging(candidate);
                fs::permissions(candidate, fs::perms::owner_all, fs::perm_options::replace, ec);
                if (ec)
                    return std::unexpected(io_error(candidate, ec));
                return staging;
            }
            if (ec)
                return std::unexpected(io_error(candidate, ec));
        }
        return std::unexpected(io_error(parent, std::make_error_code(std::errc::file_exists)));
    }

    StagingDir(StagingDir&& other) noexcept : path_(std::exchange(other.path_, {})) {}
    StagingDir& operator=(StagingDir&&) = delete;
    StagingDir(const StagingDir&) = delete;
    StagingDir& operator=(const StagingDir&) = delete;
    ~StagingDir() {
        // En iyi çaba: kalan artığı açılıştaki cleanup_after_update siler.
        if (!path_.empty())
            static_cast<void>(remove_tree(path_));
    }

    const fs::path& path() const { return path_; }

private:
    explicit StagingDir(fs::path path) : path_(std::move(path)) {}
    fs::path path_;
};

// Yazma izni denetimi: ACL ve salt-okunur bağlama ancak deneyince belli olur.
bool is_writable_dir(const fs::path& dir) {
    return StagingDir::create(dir).has_value();
}

// `prefixes`le başlayan, `kStaleStagingAge`den eski dizinler; bağa dokunulmaz.
void remove_stale_staging(const fs::path& dir, std::initializer_list<std::string_view> prefixes) {
    std::error_code ec;
    const auto cutoff = fs::file_time_type::clock::now() - kStaleStagingAge;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& entry = it->path();
        const std::string name = to_utf8(entry.filename());
        std::error_code entry_ec;
        const auto has_prefix = [&name](std::string_view prefix) { return name.starts_with(prefix); };
        if (std::ranges::none_of(prefixes, has_prefix) || is_link(entry) ||
            !fs::is_directory(entry, entry_ec))
            continue;
        const auto modified = fs::last_write_time(entry, entry_ec);
        if (!entry_ec && modified < cutoff)
            static_cast<void>(remove_tree(entry));
    }
}

core::Error cancelled_error() {
    return install_error("update.install_cancelled");
}

std::expected<void, core::Error> check_stop(const std::stop_token& stop) {
    if (stop.stop_requested())
        return std::unexpected(cancelled_error());
    return {};
}

// Dosyanın son `max_bytes`ı içindeki son boş olmayan satır; okunamazsa boş.
std::string last_line(const fs::path& file, const core::Limit& max_bytes) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec || size == 0)
        return {};
    const std::uintmax_t length = std::min<std::uintmax_t>(size, max_bytes.cap());
    std::ifstream in(file, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(size - length));
    std::string tail(static_cast<std::size_t>(length), '\0');
    in.read(tail.data(), static_cast<std::streamsize>(tail.size()));
    tail.resize(static_cast<std::size_t>(in.gcount()));
    while (!tail.empty() && (tail.back() == '\n' || tail.back() == '\r'))
        tail.pop_back();
    const std::size_t start = tail.find_last_of('\n');
    return start == std::string::npos ? tail : tail.substr(start + 1);
}

// Süreç ağacını durdurur. POSIX'te önce SIGTERM (grup): betiğin EXIT tuzağı geçici dizinini siler; bitmezse
// kTerminateGrace sonra SIGKILL. Çocuk kendi oturumunda başlar (spawn_detached): grup kimliği = pid.
void stop_process(core::ProcessHandle& process) {
#ifndef _WIN32
    if (::kill(-static_cast<pid_t>(process.pid()), SIGTERM) == 0) {
        const auto exited = core::wait_with_timeout(process, kTerminateGrace);
        if (exited && *exited)
            return;
    }
#endif
    core::terminate(process);
}

// Çocuk bitene dek bekler. `may_cancel` true iken `stop` ya da süre aşımı süreç ağacını durdurur; false
// olduğu sürece (değiştirme yarıda) yok sayılır: yarım kalmış kurulum bırakılmaz.
std::expected<int, core::Error> wait_for(core::ProcessHandle& process, const std::string& program,
                                         const core::Limit& timeout, const std::stop_token& stop,
                                         const std::function<bool()>& may_cancel) {
    const auto deadline = steady_clock::now() + std::chrono::milliseconds(timeout.value);
    for (;;) {
        const auto exited = core::wait_with_timeout(process, kPollInterval);
        if (!exited)
            return std::unexpected(exited.error());
        if (*exited)
            return **exited;
        if (!may_cancel())
            continue;
        if (stop.stop_requested()) {
            stop_process(process);
            return std::unexpected(cancelled_error());
        }
        if (!timeout.unlimited() && steady_clock::now() >= deadline) {
            stop_process(process);
            return std::unexpected(
                install_error("update.install_timeout", {{"program", program}, {"limit", timeout.text}}));
        }
    }
}

// --- tespit ---

[[maybe_unused]] std::optional<InstallTarget> detect_direct_appimage(const fs::path& exe,
                                                                     const EnvReader& env) {
    const std::optional<std::string> appimage = env("APPIMAGE");
    const std::optional<std::string> appdir = env("APPDIR");
    if (!appimage || !appdir)
        return std::nullopt;
    std::error_code ec;
    const fs::path mount = fs::weakly_canonical(from_utf8(*appdir), ec);
    if (ec || !is_inside(exe, mount))
        return std::nullopt;  // AppImage'dan miras kalan ortam, başka bir ikili
    // Bağ (ör. ~/bin/noteapp) değil gerçek dosya değişir: atomik yazma bağı dosyayla ezerdi.
    const fs::path image = fs::canonical(from_utf8(*appimage), ec);
    if (ec || !fs::is_regular_file(image, ec) || !is_writable_dir(image.parent_path()))
        return InstallTarget{};
    return InstallTarget{InstallKind::DirectAppImage, image};
}

[[maybe_unused]] std::optional<InstallTarget> detect_local_install(const fs::path& exe,
                                                                   const EnvReader& env) {
    const std::optional<std::string> home = env("HOME");
    if (!home)
        return std::nullopt;
    std::error_code ec;
    const fs::path share = fs::weakly_canonical(from_utf8(*home) / kLocalShare, ec);
    if (ec)
        return std::nullopt;
    // <sürüm>/usr/bin/<exe>: AppRun ikiliyi buradan çalıştırır. cmake ağacı kurulumu (<sürüm>/bin) AppImage
    // güncellemesini almaz: kaynaktan derlemedir.
    const fs::path relative = exe.lexically_relative(share);
    std::vector<fs::path> parts(relative.begin(), relative.end());
    if (parts.size() != 4 || parts[0] == ".." || to_utf8(parts[0]).starts_with('.') || parts[1] != "usr" ||
        parts[2] != "bin")
        return std::nullopt;
    const fs::path version_dir = share / parts[0];
    if (!fs::is_regular_file(version_dir / kAppRun, ec) ||
        !fs::is_regular_file(version_dir / kInstallScript, ec))
        return std::nullopt;
    if (!is_writable_dir(share))
        return InstallTarget{};
    return InstallTarget{InstallKind::LocalInstall, version_dir};
}

[[maybe_unused]] std::optional<InstallTarget> detect_windows_zip(const fs::path& exe) {
    const fs::path dir = exe.parent_path();
    std::error_code ec;
    for (const std::string_view name : kWindowsExes)
        if (!fs::is_regular_file(dir / name, ec))
            return std::nullopt;
    if (!fs::is_regular_file(dir / kWindowsPackageMarker, ec) || !fs::is_directory(dir / kLicensesDir, ec) ||
        !is_writable_dir(dir))
        return std::nullopt;
    return InstallTarget{InstallKind::WindowsZip, dir};
}

// --- kurulum ---

InstallResult install_direct_appimage(const InstallTarget& target, std::string_view bytes) {
    // Var olan dosyanın modu (çalıştırma biti) korunur. Çalışan FUSE bağlaması eski inode'u tutar.
    NOTEAPP_TRY(core::write_file_atomically(target.path, bytes));
    return InstallOutcome{};
}

InstallResult install_local(const InstallTarget& target, const ReleaseInfo& release, std::string_view bytes,
                            const core::UpdateLimits& limits, const std::stop_token& stop) {
    // Çalışan sürümün paketindeki betik: güncellemeyi alan her sürüm onu taşır (aynı derlemeye girdi).
    const fs::path share = target.path.parent_path();
    // Sürüm dizinleri ve geçici dizinler <share>'dedir: iki güncelleme birbirinin budamasına ve açılış temizliği
    // süren kuruluma karışmasın. Geçici dizinden önce alınır, ondan sonra bırakılır.
    const auto lock = InstallLock::try_acquire(share);
    if (!lock)
        return std::unexpected(lock.error());
    const fs::path script = target.path / kInstallScript;
    const std::optional<fs::path> bash = core::find_program("bash");
    if (!bash)
        return std::unexpected(
            core::Error{core::Error::Code::NotFound, "error.process_not_found", {{"program", "bash"}}});

    auto staging = StagingDir::create(share);
    if (!staging)
        return std::unexpected(std::move(staging.error()));
    const fs::path image = staging->path() / release.appimage.name;
    const fs::path progress = staging->path() / kProgressDir;
    if (const core::FileWriteResult written = core::write_new_file(image, bytes, /*owner_only=*/true);
        written.ec)
        return std::unexpected(io_error(image, written.ec));
    std::error_code ec;
    fs::permissions(image, fs::perms::owner_exec, fs::perm_options::add, ec);
    if (!ec)
        fs::create_directory(progress, ec);
    if (ec)
        return std::unexpected(io_error(staging->path(), ec));
    NOTEAPP_TRY(check_stop(stop));

    // Ayrık başlar (kendi oturumu, çıktısı boş aygıt): TUI ekranına ve terminaline dokunmaz. Betiğin hata
    // çıktısı progress/stderr.log'a gider; son satırı yalnız loga yazılır.
    auto process = core::spawn_detached(
        {to_utf8(*bash), to_utf8(script), "--keep", std::to_string(kKeptVersions), "--expect-version",
         release.version, "--progress-dir", to_utf8(progress), to_utf8(image)},
        core::without_secrets());
    if (!process)
        return std::unexpected(std::move(process.error()));
    const auto marked = [&progress](std::string_view name) {
        std::error_code exists_ec;
        return fs::exists(progress / name, exists_ec);
    };
    // Yalnız değiştirme sırasında (committing var, committed yok) iptal ve süre aşımı bekletilir; sonraki
    // isteğe bağlı adımlar (tamamlama, menü önbelleği, budama) takılırsa işçiyi ve uygulamanın kapanışını
    // bekletmesin.
    const auto may_cancel = [&marked] { return !marked(kCommittingFile) || marked(kCommittedFile); };
    const auto exit_code =
        wait_for(*process, "install-local.sh", limits.install_timeout, stop, may_cancel);
    const std::string stderr_tail = last_line(progress / kStderrFile, limits.tool_output_bytes);
    const std::string outcome = exit_code ? std::to_string(*exit_code) : exit_code.error().message_key;
    if ((!exit_code || *exit_code != 0) && marked(kCommittedFile)) {
        // Bağlar yeni sürümde: kurulum tamam, yalnız isteğe bağlı adımlar yarıda kaldı (aşağıda doğrulanır).
        core::log_warn(std::format("event=update_install_optional_steps_stopped outcome={} last_line=\"{}\"",
                                   outcome, stderr_tail));
    } else if (!exit_code || *exit_code != 0) {
        core::log_warn(std::format("event=update_install_script_failed exit_code={} last_line=\"{}\"",
                                   outcome, stderr_tail));
        if (!exit_code)
            return std::unexpected(exit_code.error());
        return std::unexpected(install_error(
            "update.install_script_failed",
            {{"code", std::to_string(*exit_code)}, {"url", release_page_url(release)}}));
    }
    else if (!stderr_tail.empty())  // isteğe bağlı adımın uyarısı: kurulum tamam, ayrıntı logda
        core::log_info(std::format("event=update_install_script_warning last_line=\"{}\"", stderr_tail));

    // Betik başarıyla bitse de sonuç doğrulanır: `note` bağı yeni sürümün AppRun'ını göstermeli.
    const fs::path link = share.parent_path().parent_path() / "bin" / "note";
    std::error_code link_ec;
    std::error_code expected_ec;
    const fs::path linked = fs::canonical(link, link_ec);
    const fs::path expected = fs::canonical(share / release.version / kAppRun, expected_ec);
    if (link_ec || expected_ec || linked != expected)
        return std::unexpected(install_error("update.link_not_updated", {{"path", to_utf8(link)}}));
    return InstallOutcome{};
}

// --- Windows zip çekirdeği ---

bool is_windows_exe(std::string_view name) {
    return std::ranges::find(kWindowsExes, name) != kWindowsExes.end();
}

std::string upper_ascii(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// CON, NUL.txt, com1.exe: Windows'ta dosya değil aygıt açılır.
bool is_device_name(std::string_view part) {
    const std::string base = upper_ascii(part.substr(0, part.find('.')));
    if (std::ranges::find(kDeviceNames, base) != kDeviceNames.end())
        return true;
    return base.size() == 4 && base[3] >= '1' && base[3] <= '9' &&
           std::ranges::find(kNumberedDeviceNames, std::string_view(base).substr(0, 3)) !=
               kNumberedDeviceNames.end();
}

// Ad parçası Windows'ta yazıldığı gibi açılmalı: sondaki nokta ve boşluk sessizce atılır (`note.exe.` →
// `note.exe`), `.`/`..` dizin değiştirir, aygıt adı dosya değildir.
bool is_plain_part(std::string_view part) {
    return !part.empty() && !part.ends_with('.') && !part.ends_with(' ') && !is_device_name(part);
}

// Arşiv girdisi: kökte üç exe'den biri, sahiplik işareti ya da licenses/ altı. Mutlak yol, sürücü, ters bölü
// ve NTFS akışı (`:`) reddedilir; ad Windows'ta büyük/küçük harf duyarsız olsa da birebir eşleşme istenir.
bool is_allowed_entry(std::string_view name) {
    if (name.empty() || name.find_first_of(std::string_view("\\:\0", 3)) != std::string_view::npos)
        return false;
    if (name.ends_with('/'))
        name.remove_suffix(1);  // dizin girdisi
    std::vector<std::string_view> parts;
    for (std::size_t start = 0;;) {
        const std::size_t slash = name.find('/', start);
        const std::size_t length = slash == std::string_view::npos ? std::string_view::npos : slash - start;
        parts.push_back(name.substr(start, length));
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    if (!std::ranges::all_of(parts, is_plain_part))
        return false;
    return parts.front() == kLicensesDir ||
           (parts.size() == 1 && (is_windows_exe(parts.front()) || parts.front() == kWindowsPackageMarker));
}

core::Error package_layout_error() {
    // Paket içeriği iletiye konmaz (sunucu verisi).
    return install_error("update.package_layout");
}

std::expected<void, core::Error> check_zip_entries(const std::vector<std::string>& names) {
    std::array<bool, kWindowsExes.size()> seen{};
    bool marker = false;
    for (const std::string& name : names) {
        if (!is_allowed_entry(name))
            return std::unexpected(package_layout_error());
        for (std::size_t i = 0; i < kWindowsExes.size(); ++i)
            seen[i] = seen[i] || name == kWindowsExes[i];
        marker = marker || name == kWindowsPackageMarker;
    }
    if (!marker || !std::ranges::all_of(seen, [](bool present) { return present; }))
        return std::unexpected(package_layout_error());
    return {};
}

// Açılan ağaç da denetlenir: liste ile açılan aynı olmayabilir (arşiv aracının davranışı).
std::expected<void, core::Error> check_extracted_tree(const fs::path& root) {
    std::error_code ec;
    const fs::path canonical_root = fs::weakly_canonical(root, ec);
    if (ec)
        return std::unexpected(io_error(root, ec));
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& entry = it->path();
        std::error_code entry_ec;
        if (is_link(entry) || !is_inside(fs::weakly_canonical(entry, entry_ec), canonical_root) || entry_ec)
            return std::unexpected(package_layout_error());
        const fs::path relative = entry.lexically_relative(root);
        const bool is_file = fs::is_regular_file(entry, entry_ec);
        const bool is_dir = fs::is_directory(entry, entry_ec);
        const bool top_level = std::next(relative.begin()) == relative.end();
        const std::string top = to_utf8(*relative.begin());
        const bool allowed = top_level ? (is_file && (is_windows_exe(top) || top == kWindowsPackageMarker)) ||
                                             (is_dir && top == kLicensesDir)
                                       : top == kLicensesDir && (is_file || is_dir);
        if (!allowed)
            return std::unexpected(package_layout_error());
    }
    if (ec)
        return std::unexpected(io_error(root, ec));
    for (const std::string_view exe : kWindowsExes)
        if (!fs::is_regular_file(root / exe, ec))
            return std::unexpected(package_layout_error());
    if (!fs::is_regular_file(root / kWindowsPackageMarker, ec) || !fs::is_directory(root / kLicensesDir, ec))
        return std::unexpected(package_layout_error());
    return {};
}

// Değiştirilen öğeler, bu sırayla; geri alma tersinden yürür.
std::vector<std::string_view> replaced_items() {
    std::vector<std::string_view> items(kWindowsExes.begin(), kWindowsExes.end());
    items.push_back(kWindowsPackageMarker);
    items.push_back(kLicensesDir);
    return items;
}

struct ReplaceStep {
    fs::path live;   // <exe_dir>/note.exe
    fs::path old;    // <exe_dir>/note.exe.old
    fs::path fresh;  // <staging>/new/note.exe
    bool backed_up = false;
    bool moved_in = false;
};

// Ters sırayla: yeni dosya geçici dizine geri, eski yerine. Hepsi geri alındıysa true.
bool roll_back(std::vector<ReplaceStep>& steps, const ZipInstallOps& ops) {
    bool restored = true;
    for (auto step = steps.rbegin(); step != steps.rend(); ++step) {
        if (step->moved_in && !ops.rename(step->live, step->fresh)) {
            restored = false;
            continue;  // yeni dosya yerinde kaldı: eskisi üzerine konamaz
        }
        if (step->backed_up && !ops.rename(step->old, step->live)) {
            restored = false;
            // Eskisi geri konamadı: yer boş kalmasın, yeni dosya (geçici dizinle silinecekti) geri konur.
            if (step->moved_in)
                static_cast<void>(ops.rename(step->fresh, step->live));
        }
    }
    return restored;
}

std::string args_text(const core::Error& error) {
    std::string text;
    for (const auto& [key, value] : error.args)
        text += (text.empty() ? "" : ";") + key + "=" + value;
    return text;
}

// Yarım kalmış değiştirmenin işareti (önce kalıcı olan) ya da boş.
std::optional<std::string_view> pending_marker(const fs::path& exe_dir) {
    for (const std::string_view marker : {kRollbackFailedMarker, kReplacingMarker}) {
        std::error_code ec;
        if (fs::exists(fs::symlink_status(exe_dir / marker, ec)))
            return marker;
    }
    return std::nullopt;
}

// Değiştirme işaretini kaldırır. Kaldırılamazsa loglanır: kalan işaret sonraki kurulumu ve temizliği durdurur.
void remove_replacing_marker(const fs::path& exe_dir) {
    std::error_code ec;
    fs::remove(exe_dir / kReplacingMarker, ec);
    if (ec)
        core::log_error("event=update_replacing_marker_not_removed detail=\"" + ec.message() + "\"");
}

std::expected<void, core::Error> replace_installed(const fs::path& exe_dir, const fs::path& new_root,
                                                   const ReleaseInfo& release, const ZipInstallOps& ops) {
    // İlk ad değişikliğinden önce: süreç değiştirmenin ortasında ölürse açılış temizliği yedeklere (*.old)
    // dokunmaz, sonraki kurulum başlamaz. Yazılamazsa değiştirme başlamaz.
    if (const core::FileWriteResult marked =
            core::write_new_file(exe_dir / kReplacingMarker, release.version + "\n");
        marked.ec)
        return std::unexpected(io_error(exe_dir / kReplacingMarker, marked.ec));
    std::vector<ReplaceStep> steps;
    std::optional<core::Error> failure;
    for (const std::string_view item : replaced_items()) {
        ReplaceStep step{exe_dir / item, exe_dir / old_name(item), new_root / item};
        // Önceki güncellemenin artığı; açık bir pencere hâlâ çalıştırıyorsa silinemez.
        std::error_code ec;
        if (!ops.remove(step.old) || fs::exists(fs::symlink_status(step.old, ec))) {
            failure = install_error("update.old_in_use", {{"path", to_utf8(step.old)}});
            break;
        }
        if (fs::exists(fs::symlink_status(step.live, ec))) {
            if (auto moved = ops.rename(step.live, step.old); !moved) {
                failure = std::move(moved.error());
                break;
            }
            step.backed_up = true;
        }
        steps.push_back(step);
        if (auto moved = ops.rename(step.fresh, step.live); !moved) {
            failure = std::move(moved.error());
            break;
        }
        steps.back().moved_in = true;
    }
    if (!failure) {
        remove_replacing_marker(exe_dir);
        return {};
    }
    if (roll_back(steps, ops)) {
        remove_replacing_marker(exe_dir);
        return std::unexpected(std::move(*failure));
    }

    // Yedekler (*.old) tek kurtarma yoludur: işaret varken temizlik ve yeni güncelleme onlara dokunmaz.
    // Kalıcı işaret yazıldıysa değiştirme işareti gereksizdir (kullanıcı tek dosya siler); yazılamadıysa
    // değiştirme işareti korumayı sürdürür.
    core::log_error(std::format("event=update_rollback_failed dir=\"{}\" cause={} cause_args=\"{}\"",
                                to_utf8(exe_dir), failure->message_key, args_text(*failure)));
    if (const core::FileWriteResult marked =
            core::write_new_file(exe_dir / kRollbackFailedMarker, failure->message_key + "\n");
        marked.ec && !marked.already_exists)
        core::log_error("event=update_rollback_marker_failed detail=\"" + marked.ec.message() + "\"");
    else
        remove_replacing_marker(exe_dir);
    return std::unexpected(install_error("update.rollback_failed",
                                         {{"path", to_utf8(exe_dir)}, {"url", release_page_url(release)}}));
}

std::string_view trim_line_end(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.remove_suffix(1);
    return text;
}

}  // namespace

#ifdef _WIN32

std::expected<InstallLock, core::Error> InstallLock::try_acquire(const fs::path& dir) {
    const fs::path file = dir / kInstallLockName;
    // Paylaşım 0: tutulurken başka hiçbir açış (bu süreçteki dahil) geçmez. Tutamaç kapanınca (süreç ölümü
    // dahil) dosya silinir.
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION)
            return std::unexpected(install_error("update.install_busy", {{"path", to_utf8(dir)}}));
        return std::unexpected(
            io_error(file, std::error_code(static_cast<int>(error), std::system_category())));
    }
    return InstallLock(handle);
}

InstallLock::InstallLock(InstallLock&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

InstallLock::~InstallLock() {
    if (handle_ != nullptr)
        CloseHandle(handle_);
}

#else

std::expected<InstallLock, core::Error> InstallLock::try_acquire(const fs::path& dir) {
    const fs::path file = dir / kInstallLockName;
    // Bırakan sahip dosyayı siler: kilitlenen tanım yoldaki dosya değilse (araya silme girdi) yeniden açılır.
    // Deneme sayısı kapasite değil, aynı anlık yarışın tekrarına karşı pay.
    for (int attempt = 0; attempt < kStagingAttempts; ++attempt) {
        const int fd = ::open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
        if (fd < 0)
            return std::unexpected(io_error(file, std::error_code(errno, std::generic_category())));
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            ::close(fd);
            if (error == EWOULDBLOCK)
                return std::unexpected(install_error("update.install_busy", {{"path", to_utf8(dir)}}));
            return std::unexpected(io_error(file, std::error_code(error, std::generic_category())));
        }
        struct stat locked{};
        struct stat current{};
        if (::fstat(fd, &locked) == 0 && ::lstat(file.c_str(), &current) == 0 &&
            locked.st_dev == current.st_dev && locked.st_ino == current.st_ino)
            return InstallLock(fd, file);
        ::close(fd);
    }
    return std::unexpected(install_error("update.install_busy", {{"path", to_utf8(dir)}}));
}

InstallLock::InstallLock(InstallLock&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), file_(std::move(other.file_)) {}

InstallLock::~InstallLock() {
    if (fd_ < 0)
        return;
    // Kilit tutulurken silinir: sonra açan yeni dosyayı kilitler, eski tanımı tutan yeniden dener.
    ::unlink(file_.c_str());
    ::close(fd_);
}

#endif

std::expected<void, core::Error> remove_tree(const fs::path& path) {
    std::error_code ec;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return {};
        return std::unexpected(
            io_error(path, std::error_code(static_cast<int>(error), std::system_category())));
    }
    const bool is_dir = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool descend = is_dir && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
    const fs::file_status status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found)
        return {};
    if (ec)
        return std::unexpected(io_error(path, ec));
    const bool descend = fs::is_directory(status);  // symlink_status: bağ dizin sayılmaz
#endif
    if (descend) {
        // Önce listelenir: silinen girdiler yinelemeyi bozmasın.
        std::vector<fs::path> children;
        for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
            children.push_back(it->path());
        if (ec)
            return std::unexpected(io_error(path, ec));
        for (const fs::path& child : children)
            NOTEAPP_TRY(remove_tree(child));
    }
#ifdef _WIN32
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0)
        SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    // RemoveDirectoryW junction'ın kendisini kaldırır, hedefine dokunmaz.
    if (!(is_dir ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str())))
        return std::unexpected(
            io_error(path, std::error_code(static_cast<int>(GetLastError()), std::system_category())));
#else
    fs::remove(path, ec);
    if (ec)
        return std::unexpected(io_error(path, ec));
#endif
    return {};
}

InstallTarget detect_install_target(const fs::path& exe, [[maybe_unused]] const EnvReader& env) {
    if (exe.empty())
        return {};
    std::error_code ec;
    const fs::path canonical_exe = fs::weakly_canonical(exe, ec);
    if (ec)
        return {};
#ifdef _WIN32
    return detect_windows_zip(canonical_exe).value_or(InstallTarget{});
#else
    if (auto target = detect_direct_appimage(canonical_exe, env))
        return *target;
    return detect_local_install(canonical_exe, env).value_or(InstallTarget{});
#endif
}

InstallTarget detect_install_target() {
    return detect_install_target(core::current_executable(),
                                 [](std::string_view name) { return core::get_env_utf8(name); });
}

std::optional<AssetKind> asset_kind(InstallKind kind) {
    switch (kind) {
    case InstallKind::DirectAppImage:
    case InstallKind::LocalInstall:
        return AssetKind::AppImage;
    case InstallKind::WindowsZip:
        return AssetKind::WindowsZip;
    case InstallKind::Unsupported:
        break;
    }
    return std::nullopt;
}

InstallResult install_update(const InstallTarget& target, const ReleaseInfo& release, std::string_view bytes,
                             const core::UpdateLimits& limits, std::stop_token stop) {
    NOTEAPP_TRY(check_stop(stop));
    switch (target.kind) {
    case InstallKind::DirectAppImage:
        return install_direct_appimage(target, bytes);
    case InstallKind::LocalInstall:
        return install_local(target, release, bytes, limits, stop);
    case InstallKind::WindowsZip:
#ifdef _WIN32
        return install_windows_zip(target.path, release, bytes, windows_zip_ops(limits), stop);
#else
        break;  // Windows dışında tespit edilmez
#endif
    case InstallKind::Unsupported:
        break;
    }
    return std::unexpected(install_error("update.unsupported_install", {{"url", release_page_url(release)}}));
}

void cleanup_after_update(const InstallTarget& target) {
    switch (target.kind) {
    case InstallKind::WindowsZip: {
        // Başka süreç kuruyor (kilit) ya da bir değiştirme yarıda kaldı (işaret): yedekler ve geçici dizin geri
        // almanın ya da elle kurtarmanın malzemesidir.
        const auto lock = InstallLock::try_acquire(target.path);
        if (!lock || pending_marker(target.path))
            break;
        for (const std::string_view item : replaced_items())
            static_cast<void>(remove_tree(target.path / old_name(item)));  // kullanımda: sonraki açılış
        remove_stale_staging(target.path, {kStagingPrefix});
        break;
    }
    case InstallKind::LocalInstall: {
        const fs::path share = target.path.parent_path();
        if (const auto lock = InstallLock::try_acquire(share); lock)
            remove_stale_staging(share, {kStagingPrefix, kScriptStagingPrefix});
        break;
    }
    case InstallKind::DirectAppImage:
    case InstallKind::Unsupported:
        break;
    }
}

InstallResult install_windows_zip(const fs::path& exe_dir, const ReleaseInfo& release, std::string_view bytes,
                                  const ZipInstallOps& ops, std::stop_token stop) {
    // Önce kilit: süren başka bir kurulumun değiştirme işareti yarım kalmış sanılmasın. Geçici dizinden önce
    // alınır, ondan sonra bırakılır.
    const auto lock = InstallLock::try_acquire(exe_dir);
    if (!lock)
        return std::unexpected(lock.error());
    if (const auto marker = pending_marker(exe_dir))
        return std::unexpected(install_error("update.rollback_pending", {{"path", to_utf8(exe_dir)},
                                                                         {"url", release_page_url(release)},
                                                                         {"marker", std::string(*marker)}}));
    std::error_code ec;
    auto staging = StagingDir::create(exe_dir);
    if (!staging)
        return std::unexpected(std::move(staging.error()));
    const fs::path zip = staging->path() / kZipName;
    if (const core::FileWriteResult written = core::write_new_file(zip, bytes); written.ec)
        return std::unexpected(io_error(zip, written.ec));

    auto names = ops.list(zip, stop);
    if (!names)
        return std::unexpected(std::move(names.error()));
    NOTEAPP_TRY(check_zip_entries(*names));
    const fs::path new_root = staging->path() / kExtractDir;
    fs::create_directory(new_root, ec);
    if (ec)
        return std::unexpected(io_error(new_root, ec));
    NOTEAPP_TRY(check_stop(stop));
    NOTEAPP_TRY(ops.extract(zip, new_root, stop));
    NOTEAPP_TRY(check_extracted_tree(new_root));

    const auto version = ops.version_output(new_root / kWindowsExes[0], stop);
    if (!version)
        return std::unexpected(std::move(version.error()));
    if (trim_line_end(*version) != "note " + release.version)
        return std::unexpected(install_error("update.version_mismatch", {{"expected", release.version}}));
    NOTEAPP_TRY(check_stop(stop));  // son çıkış: bundan sonrası yarıda kesilmez

    NOTEAPP_TRY(replace_installed(exe_dir, new_root, release, ops));
    return InstallOutcome{.restart_required = true};
}

}  // namespace noteapp::app
