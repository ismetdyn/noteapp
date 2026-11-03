// note view save|ls|run|rm|show: kaydedilmiş Cypher görünümleri (SPEC-cli "Görünümler").
#include "cli/commands.hpp"
#include "cli/highlight.hpp"
#include "core/i18n.hpp"
#include "core/json_text.hpp"
#include "core/note_views.hpp"

#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::cli {

using core::append_json_string;

namespace {

int usage() {
    std::cerr << core::tr("cli.view_usage") << '\n';
    return kExitUsage;
}

struct ViewArgs {
    std::vector<std::string> positional;
    bool json = false;
    bool force = false;
};

// `--` sonrası her şey konumludur: `-` ile başlayan ad ya da sorgu böyle verilir.
std::optional<ViewArgs> parse_view_args(const std::vector<std::string>& rest, bool takes_json,
                                        bool takes_force) {
    ViewArgs parsed;
    bool only_positional = false;
    for (std::size_t i = 1; i < rest.size(); ++i) {
        const std::string& arg = rest[i];
        if (only_positional || arg.empty() || arg[0] != '-')
            parsed.positional.push_back(arg);
        else if (arg == "--")
            only_positional = true;
        else if (takes_json && arg == "--json")
            parsed.json = true;
        else if (takes_force && arg == "--force")
            parsed.force = true;
        else
            return std::nullopt;
    }
    return parsed;
}

std::int64_t unix_ms(const std::optional<std::chrono::system_clock::time_point>& when) {
    return when ? std::chrono::duration_cast<std::chrono::milliseconds>(when->time_since_epoch()).count() : 0;
}

void append_json_time(std::string& out, std::string_view key,
                      const std::optional<std::chrono::system_clock::time_point>& when) {
    out += ",\"";
    out += key;
    out += "\":";
    out += when ? std::to_string(unix_ms(when)) : "null";
}

// Satır: ad<TAB>sorgu. Ad ve sorgu senkronla gelebilir: terminal temizliğinden geçer, sorgunun satır sonu
// görünür kaçışa döner (satır başına bir görünüm).
std::string list_line(const core::SavedView& view) {
    std::string query = sanitize_for_terminal(view.query);
    std::string flat;
    for (const char c : query)
        flat += c == '\n' ? std::string("\\n") : c == '\t' ? std::string("\\t") : std::string(1, c);
    return sanitize_for_terminal(view.name) + '\t' + flat;
}

int run_list(const ViewArgs& args, const core::NoteStore& store) {
    if (!args.positional.empty())
        return usage();
    const auto views = core::list_views(store);
    if (!views)
        return fail(views.error());
    if (!args.json) {
        for (const core::SavedView& view : *views)
            std::cout << list_line(view) << '\n';
        return kExitOk;
    }
    std::string out = "[";
    for (const core::SavedView& view : *views) {
        out += out.size() > 1 ? ",{\"name\":" : "{\"name\":";
        append_json_string(out, view.name);
        out += ",\"query\":";
        append_json_string(out, view.query);
        append_json_time(out, "created_at", view.created_at);
        append_json_time(out, "updated_at", view.updated_at);
        out += '}';
    }
    std::cout << out << "]\n";
    return kExitOk;
}

int run_save(const ViewArgs& args, core::NoteStore& store, const core::Config& config) {
    if (args.positional.size() != 2)
        return usage();
    const std::string& name = args.positional[0];
    if (const auto saved = core::save_view(store, name, args.positional[1], args.force); !saved) {
        if (saved.error().code != core::Error::Code::AlreadyExists)
            return fail(saved.error());
        std::cerr << sanitize_for_terminal(core::tr("cli.view_exists", saved.error().args)) << '\n';
        return kExitUser;
    }
    std::cout << sanitize_for_terminal(core::tr("cli.view_saved", {{"name", name}})) << '\n';
    start_background_sync(config, false);
    return kExitOk;
}

}  // namespace

int run_view(const ParsedArgs& args, const core::Config& config) {
    if (args.rest.empty())
        return usage();
    const std::string& action = args.rest.front();
    const bool lists = action == "ls";
    const bool runs = action == "run";
    const auto parsed = parse_view_args(args.rest, lists || runs, action == "save");
    if (!parsed)
        return usage();
    core::NoteStore store = core::NoteStore::for_config(config);
    if (action == "save")
        return run_save(*parsed, store, config);
    if (lists)
        return run_list(*parsed, store);
    if (parsed->positional.size() != 1 || (action != "run" && action != "rm" && action != "show"))
        return usage();
    const std::string& name = parsed->positional.front();
    if (runs) {
        const auto result = core::run_view(store, name);
        if (!result)
            return fail(result.error());
        std::cout << query_output(*result, parsed->json);
        return kExitOk;
    }
    if (action == "show") {
        const auto view = core::find_view(store, name);
        if (!view)
            return fail(view.error());
        std::cout << sanitize_for_terminal(view->query) << '\n';
        return kExitOk;
    }
    if (const auto removed = core::remove_view(store, name); !removed)
        return fail(removed.error());
    std::cout << sanitize_for_terminal(core::tr("cli.view_removed", {{"name", name}})) << '\n';
    start_background_sync(config, false);
    return kExitOk;
}

}  // namespace noteapp::cli
