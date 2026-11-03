#include "app/settings.hpp"

#include "core/i18n.hpp"
#include "sync/auth.hpp"

namespace noteapp::app {

TokenStore system_token_store() {
    return {[](std::string_view host) { return sync::has_token(host); },
            [](std::string_view host, std::string_view token) { return sync::store_token(host, token); },
            [](std::string_view host) { return sync::clear_token(host); }};
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

}  // namespace noteapp::app
