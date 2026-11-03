#include "app/settings.hpp"
#include "core/i18n.hpp"

#include <doctest/doctest.h>
#include <string>

using namespace noteapp;

TEST_CASE("config_warnings_message joins translated warnings with a semicolon, empty without any") {
    CHECK(app::config_warnings_message({}).empty());

    const core::Error unknown{core::Error::Code::ConfigInvalid, "config.unknown_key", {{"key", "bogus"}}};
    const core::Error syntax{core::Error::Code::ConfigInvalid,
                             "config.syntax_error",
                             {{"line", "3"}, {"column", "5"}, {"detail", "x"}}};
    CHECK(app::config_warnings_message({unknown}) == core::format_error(unknown));
    CHECK(app::config_warnings_message({unknown, syntax}) ==
          core::format_error(unknown) + "; " + core::format_error(syntax));
}

TEST_CASE("sync_restart_needed is true only for the fields AutoSync reads at start") {
    const core::Config base = core::parse_config("").config;
    const auto changed = [&](auto edit) {
        core::Config next = base;
        edit(next);
        return app::sync_restart_needed(base, next);
    };
    CHECK_FALSE(app::sync_restart_needed(base, base));
    CHECK_FALSE(changed([](core::Config& c) { c.editor = "vim"; }));
    CHECK_FALSE(changed([](core::Config& c) { c.sync.author_name = "Other"; }));
    CHECK(changed([](core::Config& c) { c.sync.enabled = !c.sync.enabled; }));
    CHECK(changed([](core::Config& c) { c.sync.remote = "https://example.invalid/r.git"; }));
    CHECK(changed([](core::Config& c) { c.sync.branch = "other"; }));
    CHECK(changed([](core::Config& c) { c.sync.idle_seconds += 1; }));
    CHECK(changed([](core::Config& c) { c.notes_dir /= "sub"; }));
}
