// note update [--check] [--yes] (SPEC-cli "Güncelleme"): seçenekler, Ctrl-C ve üretim bağımlılıkları. Akışın
// kendisi update_flow.cpp'dedir.
#include "app/update_worker.hpp"
#include "cli/commands.hpp"
#include "cli/term.hpp"
#include "cli/update_flow.hpp"

#include <atomic>
#include <csignal>
#include <iostream>
#include <optional>
#include <string>

namespace noteapp::cli {

namespace {

// Ctrl-C işçiye iptal olarak gider, süreci öldürmez: değiştirme sırasında ölen süreç yarım kurulum
// bırakırdı (kurulum iptali değiştirme başlayana dek kabul eder, sonra yok sayar). Sinyal herhangi bir iş
// parçacığına (Windows: yeni bir iş parçacığına) gelebilir: kilitsiz atomik hem sinyal güvenli hem iş
// parçacıkları arasında görünür.
std::atomic<bool> g_interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free);

extern "C" void record_interrupt(int signal) {
    g_interrupted.store(true);
    // Windows CRT işleyiciyi çağırmadan önce SIG_DFL'e döndürür: ikinci Ctrl-C kurulumu yarıda öldürmesin.
    // POSIX'te (glibc) işleyici kalır; yeniden kurmak etkisizdir.
    std::signal(signal, record_interrupt);
}

using SignalHandler = void (*)(int);

// Kapsam boyunca SIGINT `handler`a gider; çıkışta önceki işleyici geri gelir. Devralınan SIG_IGN (ör. `nohup`,
// arka plan işi) korunur: çağıran Ctrl-C'yi bu sürece hiç istemiyor.
class SigintScope {
public:
    explicit SigintScope(SignalHandler handler) : previous_(std::signal(SIGINT, handler)) {
        if (previous_ == SIG_IGN)
            std::signal(SIGINT, SIG_IGN);
    }
    ~SigintScope() {
        if (previous_ != SIG_ERR)
            std::signal(SIGINT, previous_);
    }
    SigintScope(const SigintScope&) = delete;
    SigintScope& operator=(const SigintScope&) = delete;

private:
    SignalHandler previous_;
};

// Onay istemi: Ctrl-C burada süreci her zamanki gibi bitirir (henüz hiçbir şey indirilmedi ya da kurulmadı);
// yakalanırsa satır okuması kesilmez, kullanıcı Enter'a basana dek beklerdi.
std::optional<std::string> read_answer() {
    const SigintScope default_interrupt(SIG_DFL);
    std::string answer;
    if (!std::getline(std::cin, answer))
        return std::nullopt;
    return answer;
}

}  // namespace

int run_update(const ParsedArgs& args, const core::Config& config) {
    UpdateOptions options;
    for (const std::string& arg : args.rest) {
        if (arg == "--check")
            options.check_only = true;
        else if (arg == "--yes" || arg == "-y")
            options.yes = true;
        else if (arg.size() > 1 && arg.front() == '-')
            return unknown_option_error("update", arg);
        else
            return usage_error("update");
    }

    app::UpdateSettings settings;
    settings.limits = config.limits.update;
    const SigintScope interrupt(record_interrupt);
    app::UpdateWorker worker(std::move(settings), app::default_update_deps());
    UpdateConsole console{.out = std::cout,
                          .err = std::cerr,
                          .stdin_tty = stdin_is_tty(),
                          .read_line = read_answer,
                          .interrupted = [] { return g_interrupted.load(); },
                          .reset_interrupt = [] { g_interrupted.store(false); }};
    return run_update_flow(options, worker, console);
}

}  // namespace noteapp::cli
