#include "app/stray_notice.hpp"

#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/migration.hpp"
#include "core/note_store.hpp"
#include "core/paths.hpp"

#include <cstdint>
#include <exception>
#include <string>

namespace noteapp::app {

// Ölçüm (2026-11-02, Debug, 4 KiB'lik 2000 dosya): tarama ~600 ms; tuşu bekletmemesi için işçide koşar. Depo
// config'le açılır: içe alınamayacak dosyanın kararı limits.core.folder_path_bytes'ı kullanır.
std::size_t count_stray_files(const core::Config& config, const std::stop_token& stop) noexcept {
    try {
        const core::NoteStore store = core::NoteStore::for_config(config);
        const core::TuiLimits& limits = config.limits.tui;
        const std::uintmax_t max_bytes =
            limits.stray_notice_bytes.unlimited() ? UINTMAX_MAX : limits.stray_notice_bytes.value;
        const auto scan = core::find_stray_files(
            store, config.notes_dir, core::data_dir(),
            {.max_entries = limits.stray_notice_files.cap(), .max_bytes = max_bytes, .stop = stop});
        if (!scan) {
            core::log_warn("event=stray_scan_failed detail=" + core::format_error(scan.error()));
            return 0;
        }
        return stop.stop_requested() ? 0 : scan->files.size();
    } catch (const std::exception& error) {
        core::log_warn(std::string("event=stray_scan_failed detail=") + error.what());
    } catch (...) {
        core::log_warn("event=stray_scan_failed detail=unknown_exception");
    }
    return 0;
}

}  // namespace noteapp::app
