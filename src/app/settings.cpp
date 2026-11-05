#include "app/settings.hpp"

#include "core/i18n.hpp"
#include "sync/auth.hpp"
#include "sync/sync_service.hpp"

namespace noteapp::app {

TokenStore system_token_store() {
    return {[](std::string_view host) { return sync::has_token(host); },
            [](std::string_view host, std::string_view token) { return sync::store_token(host, token); },
            [](std::string_view host) { return sync::clear_token(host); }};
}

SyncEnabler system_sync_enabler() {
    return [](const core::Config& next, bool replace_origin) {
        return sync::SyncService(next).enable(next.sync.remote, replace_origin);
    };
}

std::expected<void, core::Error> run_connection_test(const core::Config& candidate,
                                                     const SyncEnabler& enable) {
    if (auto enabled = enable(candidate, false); !enabled)
        return enabled;
    if (auto synced = sync::SyncService(candidate).sync_now(); !synced)
        return std::unexpected(std::move(synced.error()));
    return {};
}

std::string config_warnings_message(const std::vector<core::Error>& warnings) {
    std::string joined;
    for (const auto& warning : warnings) {
        if (!joined.empty())
            joined += "; ";
        joined += core::format_error(warning);
    }
    return joined;
}

bool sync_restart_needed(const core::Config& before, const core::Config& after) {
    return before.sync.enabled != after.sync.enabled || before.sync.remote != after.sync.remote ||
           before.sync.branch != after.sync.branch || before.sync.idle_seconds != after.sync.idle_seconds ||
           before.notes_dir != after.notes_dir;
}

void adopt_live_limits(core::Config& live, const core::Config& on_disk) {
    live.limits.app.map_nodes = on_disk.limits.app.map_nodes;
    live.limits.app.map_depth = on_disk.limits.app.map_depth;
}

}  // namespace noteapp::app
