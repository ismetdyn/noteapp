// note sync … (SPEC-cli "Komutlar"/"Senkron", SPEC-sync). Senkron hataları çıkış 4.
#include "cli/commands.hpp"
#include "cli/confirm.hpp"
#include "cli/highlight.hpp"
#include "core/error.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/process.hpp"
#include "sync/auth.hpp"
#include "sync/sync_service.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace noteapp::cli {

namespace {

namespace fs = std::filesystem;

using Values = std::vector<std::pair<std::string, std::string>>;

// Arka plan senkronu meşgul kilidi bu aralıkla yeniden dener (senkronlar saniyeler sürer).
constexpr std::chrono::milliseconds kBusyRetryInterval{500};

int sync_fail(const core::Error& error) {
    std::cerr << sanitize_for_terminal(core::format_error(error)) << '\n';
    return kExitSync;
}

int usage() {
    std::cerr << core::tr("cli.sync_usage") << '\n';
    return kExitUsage;
}

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

// Kullanıcı kendi saatiyle okur: "YYYY-MM-DD HH:MM".
std::string local_time(std::chrono::system_clock::time_point when) {
    const std::time_t time = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                       local.tm_hour, local.tm_min);
}

// load_config_for_update ile okunmuş config'e değerleri uygulayıp kaydeder (config set ile aynı doğrulama).
int save_values(core::Config& config, const Values& values) {
    for (const auto& [key, value] : values) {
        if (const auto set = core::set_config_value(config, key, value); !set) {
            std::cerr << sanitize_for_terminal(core::format_error(set.error())) << '\n'
                      << core::tr("cli.config_not_saved") << '\n';
            return kExitUsage;
        }
    }
    if (const auto saved = core::save_config(config); !saved) {
        std::cerr << sanitize_for_terminal(core::format_error(saved.error())) << '\n';
        return kExitInternal;
    }
    return kExitOk;
}

// Token ekosuz okunur; terminal ayarı her çıkışta (Ctrl-C dahil) geri yüklenir, yoksa kabuk yazılanı
// göstermeyen bir terminalde kalırdı.
class EchoOff {
public:
    EchoOff() {
#ifdef _WIN32
        input_ = GetStdHandle(STD_INPUT_HANDLE);
        active_ = GetConsoleMode(input_, &saved_) != 0 && SetConsoleMode(input_, saved_ & ~ENABLE_ECHO_INPUT);
#else
        active_ = tcgetattr(STDIN_FILENO, &saved_) == 0;
        if (active_) {
            termios quiet = saved_;
            quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
            active_ = tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) == 0;
        }
#endif
        previous_ = std::signal(SIGINT, &EchoOff::on_interrupt);
    }
    ~EchoOff() {
        restore();
        std::signal(SIGINT, previous_);
    }
    EchoOff(const EchoOff&) = delete;
    EchoOff& operator=(const EchoOff&) = delete;

private:
    static void restore() {
        if (!active_)
            return;
#ifdef _WIN32
        SetConsoleMode(input_, saved_);
#else
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_);
#endif
    }
    static void on_interrupt(int) {
        restore();
        std::_Exit(130);  // kabuk geleneği: 128 + SIGINT
    }

    // Sinyal işleyicisi nesneye erişemez; tek seferlik okuma için süreç geneli durum yeterli.
#ifdef _WIN32
    static inline HANDLE input_ = nullptr;
    static inline DWORD saved_ = 0;
#else
    static inline termios saved_{};
#endif
    static inline bool active_ = false;
    void (*previous_)(int) = SIG_DFL;
};

std::string read_token(const std::string& host) {
    std::string token;
    if (!stdin_is_tty()) {  // boru: ilk satır (store_token boşlukları kırpar)
        std::getline(std::cin, token);
        return token;
    }
    std::cerr << core::tr("cli.sync_token_prompt", {{"host", host}}) << std::flush;
    {
        const EchoOff echo_off;
        std::getline(std::cin, token);
    }
    std::cerr << '\n';  // Enter ekoya düşmedi: imleç alt satıra
    return token;
}

// HOST verilmezse config.sync.remote'un host'u (kasa hesabı = host, SPEC.md §8).
std::optional<std::string> token_host(const core::Config& config, const std::vector<std::string>& operands) {
    const std::string host =
        operands.empty() ? sync::parse_remote(config.sync.remote).host : operands.front();
    if (host.empty()) {
        std::cerr << core::tr("cli.sync_login_no_host") << '\n';
        return std::nullopt;
    }
    return host;
}

int sync_foreground(const core::Config& config) {
    // Ön planda süreç içinde: kullanıcı başında, asılı kalan ssh'ı Ctrl-C ile keser (SPEC-sync karar 3).
    const auto result = sync::SyncService(config).sync_now();
    if (!result)
        return sync_fail(result.error());
    std::cout << core::tr("cli.sync_done",
                          {{"committed", std::to_string(result->committed)},
                           {"pulled", std::to_string(result->pulled)},
                           {"pushed", core::tr(result->pushed ? "cli.sync_pushed" : "cli.sync_not_pushed")},
                           {"conflicts", std::to_string(result->conflict_copies.size())}})
              << '\n';
    for (const core::NotePath& copy : result->conflict_copies)
        std::cout << sanitize_for_terminal(core::tr("cli.sync_conflict_copy", {{"path", copy.string()}}))
                  << '\n';
    for (const core::Error& issue : result->log_issues)
        std::cerr << sanitize_for_terminal(core::format_error(issue)) << '\n';
    return kExitOk;
}

// Ayrık süreç (start_background_sync / AutoSync): hiçbir çıktı yok, sonuç log + sync-status.toml'da.
// Başka bir senkron kilidi tutuyorsa (sync.busy) bekleyip yeniden dener: o senkron bu değişikliği görmemiş
// olabilir ve ayrık sürecin "sonra deneyen" bir çağıranı yok. Bekleme bütçenin üçte biriyle sınırlı: AutoSync
// bu süreci sync_timeout_seconds sonunda öldürür; kilit tam öldürülmeden önce alınırsa senkron her seferinde
// yarıda kesilirdi. Kalan üçte iki senkronun kendisine kalır.
int sync_background(const core::Config& config) {
    if (!config.sync.enabled)
        return kExitOk;  // süreç başlatıldıktan sonra kapatıldıysa: kapalıyken otomatik senkron yok
    sync::SyncService service(config);
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::chrono::seconds(config.tuning.sync_timeout_seconds)) / 3;
    for (;;) {
        const auto result = service.sync_now();
        if (result)
            return kExitOk;
        if (result.error().message_key != "sync.busy" ||
            std::chrono::steady_clock::now() + kBusyRetryInterval >= deadline)
            return kExitSync;
        std::this_thread::sleep_for(kBusyRetryInterval);
    }
}

int sync_status(const core::Config& config) {
    std::cout << core::tr(config.sync.enabled ? "cli.sync_status_on" : "cli.sync_status_off") << '\n';
    if (config.sync.remote.empty())
        std::cout << core::tr("cli.sync_status_no_remote") << '\n';
    else
        std::cout << core::tr("cli.sync_status_remote",
                              {{"remote", sync::without_userinfo(config.sync.remote)}})
                  << '\n';
    const sync::SyncStatus status = sync::SyncService(config).status();
    if (status.last_success)
        std::cout << core::tr("cli.sync_status_last_success", {{"time", local_time(*status.last_success)}})
                  << '\n';
    else
        std::cout << core::tr("cli.sync_status_never") << '\n';
    if (status.last_error)
        std::cout << sanitize_for_terminal(core::tr("cli.sync_status_last_error",
                                                    {{"error", core::format_error(*status.last_error)}}))
                  << '\n';
    for (const core::Error& issue : status.log_issues)
        std::cout << sanitize_for_terminal(core::format_error(issue)) << '\n';
    for (const sync::DeviceLogHead& head : status.heads)
        std::cout << sanitize_for_terminal(
                         core::tr("cli.sync_status_head", {{"device", head.device},
                                                           {"log", std::to_string(head.log_seq)},
                                                           {"applied", std::to_string(head.applied_seq)}}))
                  << '\n';
    return kExitOk;
}

// Uzak adresin onayı gerekiyor (origin farklı): terminalde sorulur, değilse --replace yönlendirmesi.
std::expected<void, core::Error> confirm_and_replace(sync::SyncService& service, const std::string& remote,
                                                     const core::Error& mismatch, bool& declined) {
    std::cerr << sanitize_for_terminal(core::format_error(mismatch)) << '\n';
    if (!stdin_is_tty()) {
        std::cerr << core::tr("cli.sync_replace_hint", {{"remote", sync::without_userinfo(remote)}}) << '\n';
        declined = true;
        return {};
    }
    std::cerr << core::tr("cli.sync_confirm_replace", {{"remote", sync::without_userinfo(remote)}})
              << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer) || !is_delete_confirmed(answer)) {
        std::cerr << core::tr("cli.sync_not_replaced") << '\n';
        declined = true;
        return {};
    }
    return service.enable(remote, true);
}

int sync_init(const core::Config& config, const std::vector<std::string>& operands) {
    std::vector<std::string> remotes;
    bool replace = false;
    for (const std::string& arg : operands) {
        if (arg == "--replace")
            replace = true;
        else
            remotes.push_back(arg);
    }
    if (remotes.size() != 1)
        return usage();
    const std::string& remote = remotes.front();
    // Ağa ve depoya dokunmadan önce: sonuç kaydedilemeyecekse hiç başlanmaz.
    std::optional<core::Config> fresh = load_config_for_update();
    if (!fresh)
        return kExitUser;

    sync::SyncService service(config);
    auto enabled = service.enable(remote, replace);
    if (!enabled && enabled.error().message_key == "sync.remote_mismatch" && !replace) {
        bool declined = false;
        enabled = confirm_and_replace(service, remote, enabled.error(), declined);
        if (declined)
            return kExitUser;
    }
    if (!enabled)
        return sync_fail(enabled.error());
    if (const int code = save_values(*fresh, {{"sync.remote", remote}, {"sync.enabled", "true"}});
        code != kExitOk)
        return code;
    std::cout << core::tr("cli.sync_init_done", {{"remote", sync::without_userinfo(remote)}}) << '\n';
    return kExitOk;
}

int sync_toggle(bool on) {
    std::optional<core::Config> fresh = load_config_for_update();
    if (!fresh)
        return kExitUser;
    if (on) {
        if (fresh->sync.remote.empty()) {
            std::cerr << core::tr("cli.sync_needs_init") << '\n';
            return kExitUser;
        }
        // Tekrar açmak enable'ı çağırır (SPEC-sync "Aç/kapa"): depo kuruluysa ve origin aynıysa no-op.
        if (const auto enabled = sync::SyncService(*fresh).enable(fresh->sync.remote); !enabled) {
            if (enabled.error().message_key != "sync.remote_mismatch")
                return sync_fail(enabled.error());
            std::cerr << sanitize_for_terminal(core::format_error(enabled.error())) << '\n'
                      << core::tr("cli.sync_replace_hint",
                                  {{"remote", sync::without_userinfo(fresh->sync.remote)}})
                      << '\n';
            return kExitUser;
        }
    }
    if (const int code = save_values(*fresh, {{"sync.enabled", on ? "true" : "false"}}); code != kExitOk)
        return code;
    std::cout << core::tr(on ? "cli.sync_turned_on" : "cli.sync_turned_off") << '\n';
    return kExitOk;
}

int sync_login(const core::Config& config, const std::vector<std::string>& operands) {
    const auto host = token_host(config, operands);
    if (!host)
        return kExitUsage;
    // Token hiçbir yere yazdırılmaz/loglanmaz; hata mesajları da onu taşımaz (store_token sözleşmesi).
    if (const auto stored = sync::store_token(*host, read_token(*host)); !stored)
        return sync_fail(stored.error());
    std::cout << core::tr("cli.sync_login_done", {{"host", *host}}) << '\n';
    return kExitOk;
}

int sync_logout(const core::Config& config, const std::vector<std::string>& operands) {
    const auto host = token_host(config, operands);
    if (!host)
        return kExitUsage;
    if (const auto cleared = sync::clear_token(*host); !cleared)
        return sync_fail(cleared.error());
    std::cout << core::tr("cli.sync_logout_done", {{"host", *host}}) << '\n';
    return kExitOk;
}

}  // namespace

int run_sync(const ParsedArgs& args, const core::Config& config) {
    if (args.rest.empty())
        return sync_foreground(config);
    const std::string& verb = args.rest.front();
    const std::vector<std::string> operands(args.rest.begin() + 1, args.rest.end());
    if (verb == "--background" && operands.empty())
        return sync_background(config);
    if (verb == "status" && operands.empty())
        return sync_status(config);
    if ((verb == "on" || verb == "off") && operands.empty())
        return sync_toggle(verb == "on");
    if (verb == "init")
        return sync_init(config, operands);
    if (verb == "login" && operands.size() <= 1)
        return sync_login(config, operands);
    if (verb == "logout" && operands.size() <= 1)
        return sync_logout(config, operands);
    return usage();
}

void start_background_sync(const core::Config& config, bool no_sync) {
    if (!config.sync.enabled || no_sync)
        return;
    const fs::path self = core::current_executable();
    const auto child =
        core::spawn_detached({self.empty() ? std::string("note") : utf8(self), "sync", "--background"});
    if (!child)
        core::log_warn("sync: background sync could not start: " + core::format_error(child.error()));
}

}  // namespace noteapp::cli
