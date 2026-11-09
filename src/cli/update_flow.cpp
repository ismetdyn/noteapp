#include "cli/update_flow.hpp"

#include "cli/commands.hpp"
#include "cli/confirm.hpp"
#include "cli/highlight.hpp"
#include "cli/term.hpp"
#include "core/i18n.hpp"

#include <chrono>
#include <optional>
#include <ostream>
#include <string>
#include <variant>

namespace noteapp::cli {

namespace {

// Kabuk geleneği 128 + SIGINT; `sync login`in Ctrl-C çıkışıyla aynı.
constexpr int kExitInterrupted = 130;
// Ctrl-C ve ilerleme yoklaması: iptal en geç bu kadar sonra işçiye iletilir.
constexpr std::chrono::milliseconds kPollInterval{100};

std::string progress_text(const app::UpdateStatus& status) {
    if (std::holds_alternative<app::UpdateChecking>(status))
        return core::tr("cli.update_checking");
    if (const auto* downloading = std::get_if<app::UpdateDownloading>(&status))
        return core::tr("cli.update_downloading", {{"version", downloading->version}});
    if (const auto* installing = std::get_if<app::UpdateInstalling>(&status))
        return core::tr("cli.update_installing", {{"version", installing->version}});
    return {};
}

// İşçi boşa çıkana dek bekler; stderr TTY ise ne yaptığını tek satırda gösterir. Ctrl-C gelince bir kez
// iptal ister. İptal istendiyse true (işlem yine de bitmiş olabilir: kurulum değiştirme başladıktan sonra
// iptali yok sayar).
bool wait_for(app::UpdateWorker& worker, const UpdateConsole& console) {
    bool cancelled = false;
    std::string shown = progress_text(worker.status());
    std::optional<ProgressLine> progress;
    if (!shown.empty())
        progress.emplace(shown);
    while (!worker.wait_idle_for(kPollInterval)) {
        if (!cancelled && console.interrupted && console.interrupted()) {
            worker.cancel();
            cancelled = true;
        }
        if (std::string text = progress_text(worker.status()); !text.empty() && text != shown) {
            if (progress)
                progress->update(text);
            else
                progress.emplace(text);
            shown = std::move(text);
        }
    }
    return cancelled;
}

int report_failure(const core::Error& error, const UpdateConsole& console) {
    console.err << sanitize_for_terminal(core::format_error(error)) << '\n';
    return kExitInternal;
}

int report_cancelled(const UpdateConsole& console) {
    console.err << core::tr("cli.update_cancelled") << '\n';
    return kExitInterrupted;
}

// Onay: TTY'de sorulur; TTY yoksa --yes'siz kurulum reddedilir (asla sessiz kurulum yok).
bool confirmed(const std::string& version, const UpdateConsole& console) {
    if (!console.stdin_tty) {
        console.err << core::tr("cli.update_needs_yes", {{"version", version}}) << '\n';
        return false;
    }
    console.err << core::tr("cli.update_confirm", {{"version", version}}) << std::flush;
    const std::optional<std::string> answer = console.read_line ? console.read_line() : std::nullopt;
    if (answer && is_delete_confirmed(*answer))
        return true;
    console.err << core::tr("cli.update_not_confirmed") << '\n';
    return false;
}

int install(app::UpdateWorker& worker, const UpdateConsole& console) {
    if (!worker.install())
        return kExitInternal;  // denetim kurulabilir sürüm buldu, işçi boşta: olmamalı
    const bool cancelled = wait_for(worker, console);
    const app::UpdateStatus status = worker.status();
    if (const auto* done = std::get_if<app::UpdateDone>(&status)) {
        console.out << core::tr("cli.update_installed", {{"version", done->version}}) << '\n';
        if (done->restart_required)
            console.out << core::tr("cli.update_restart") << '\n';
        return kExitOk;
    }
    // Hata iptalden önce: değiştirme başladıktan sonra Ctrl-C gelse de kurulumun hatası gösterilir.
    if (const auto* failed = std::get_if<app::UpdateFailed>(&status))
        return report_failure(failed->error, console);
    if (cancelled)
        return report_cancelled(console);
    return kExitInternal;
}

}  // namespace

int run_update_flow(const UpdateOptions& options, app::UpdateWorker& worker, UpdateConsole& console) {
    if (!worker.check())
        return kExitInternal;  // yeni işçi boştadır: olmamalı
    if (wait_for(worker, console))
        return report_cancelled(console);
    if (console.reset_interrupt)
        console.reset_interrupt();
    const app::UpdateStatus checked = worker.status();
    if (const auto* failed = std::get_if<app::UpdateFailed>(&checked))
        return report_failure(failed->error, console);
    if (const auto* latest = std::get_if<app::UpToDate>(&checked)) {
        console.out << core::tr("cli.update_up_to_date",
                                {{"current", worker.current_version()}, {"latest", latest->latest_version}})
                    << '\n';
        return kExitOk;
    }
    const auto* available = std::get_if<app::UpdateAvailable>(&checked);
    if (available == nullptr)
        return kExitInternal;  // denetim yalnız bu üç sonuçla biter

    console.out << core::tr("cli.update_available",
                            {{"version", available->version}, {"current", worker.current_version()}})
                << '\n';
    if (!available->installable) {
        // --check bilgi verir (çıkış 0); kurulum istendiyse yapılamaz (çıkış 1).
        std::ostream& stream = options.check_only ? console.out : console.err;
        const std::string text = core::tr("update.unsupported_install", {{"url", available->release_page}});
        stream << sanitize_for_terminal(text) << '\n';
        return options.check_only ? kExitOk : kExitUser;
    }
    if (options.check_only)
        return kExitOk;
    if (!options.yes && !confirmed(available->version, console))
        return kExitUser;
    return install(worker, console);
}

}  // namespace noteapp::cli
