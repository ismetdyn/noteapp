// `note prop` ve `note relation prop` (SPEC-cli "Özellikler"): kural core'un (core/note_props.hpp, sorgu
// çubuğunun korumasıyla aynı), ileti app::Inspector'ın (GUI ve TUI denetçisiyle aynı metin).
#include "app/inspector.hpp"
#include "cli/commands.hpp"
#include "cli/highlight.hpp"
#include "core/i18n.hpp"
#include "core/json_text.hpp"
#include "core/note_props.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <expected>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace noteapp::cli {

namespace {

struct PropArgs {
    std::string verb;
    std::vector<std::string> positional;
    std::optional<core::PropertyType> type;
    bool json = false;
};

// Eksi sayı (`-5`, `-0.5`) değerdir, seçenek değil: `note prop set a.md n -5`.
bool is_number_word(std::string_view arg) {
    return arg.size() > 1 && arg[0] == '-' &&
           (std::isdigit(static_cast<unsigned char>(arg[1])) != 0 || arg[1] == '.');
}

// rest[first]: fiil; sonrası konumlu argümanlar ve seçenekler. `--` sonrası her şey konumludur.
std::expected<PropArgs, int> parse_prop_args(std::span<const std::string> rest, std::string_view command) {
    PropArgs parsed;
    parsed.verb = rest.empty() ? std::string() : rest.front();
    bool only_positional = false;
    for (std::size_t i = 1; i < rest.size(); ++i) {
        const std::string& arg = rest[i];
        if (only_positional || arg.empty() || arg[0] != '-' || is_number_word(arg)) {
            parsed.positional.push_back(arg);
        } else if (arg == "--") {
            only_positional = true;
        } else if (arg == "--json" && parsed.verb == "get") {
            parsed.json = true;
        } else if (arg == "--type" && parsed.verb == "set") {
            if (i + 1 >= rest.size())
                return std::unexpected(missing_value_error(command, arg));
            parsed.type = core::property_type_named(rest[++i]);
            if (!parsed.type) {
                std::cerr << sanitize_for_terminal(core::tr("cli.prop_bad_type", {{"type", rest[i]}}))
                          << '\n';
                return std::unexpected(kExitUsage);
            }
        } else {
            return std::unexpected(unknown_option_error(command, arg));
        }
    }
    return parsed;
}

// JSON'da değer kendi türünde: metin dize, sayı ve bool yalın; zaman ve bayt metniyle; büyük değer null.
void append_value(std::string& out, const core::Property& property) {
    if (property.large) {
        out += "null";
        return;
    }
    const auto type = core::property_type(property.value);
    if (type == core::PropertyType::Number || type == core::PropertyType::Bool)
        out += core::property_text(property.value);
    else
        core::append_json_string(out, core::property_text(property.value));
}

std::string type_word(const core::Property& property) {
    if (property.large)
        return "large";
    if (const auto type = core::property_type(property.value))
        return std::string(core::property_type_name(*type));
    return std::holds_alternative<graph::Timestamp>(property.value) ? "time" : "bytes";
}

std::string value_text(const core::Property& property) {
    return property.large ? "<" + std::to_string(property.size) + " bytes>"
                          : core::property_text(property.value);
}

// Tümü: ANAHTAR<TAB>TÜR<TAB>DEĞER, salt okunur satırın sonunda `<TAB>read-only`; --json aynı alanlar.
// Tek anahtar: yalnız değer (betikler için); yoksa çıkış 1.
int print_props(const std::vector<core::Property>& props, const PropArgs& args) {
    if (args.positional.size() == 2) {
        const auto found = std::ranges::find(props, args.positional[1], &core::Property::key);
        if (found == props.end())
            return fail(core::Error{
                core::Error::Code::NotFound, "error.property_not_found", {{"key", args.positional[1]}}});
        if (args.json) {
            std::string out;
            append_value(out, *found);
            std::cout << out << '\n';
        } else {
            std::cout << sanitize_for_terminal(value_text(*found)) << '\n';
        }
        return kExitOk;
    }
    if (!args.json) {
        for (const core::Property& property : props)
            std::cout << sanitize_for_terminal(property.key) << '\t' << type_word(property) << '\t'
                      << sanitize_for_terminal(value_text(property))
                      << (property.read_only ? "\tread-only" : "") << '\n';
        return kExitOk;
    }
    std::string out = "[";
    for (const core::Property& property : props) {
        out += out.size() > 1 ? ",{\"key\":" : "{\"key\":";
        core::append_json_string(out, property.key);
        out += ",\"type\":\"" + type_word(property) + "\",\"value\":";
        append_value(out, property);
        out += std::string(",\"read_only\":") + (property.read_only ? "true" : "false") + "}";
    }
    std::cout << out << "]\n";
    return kExitOk;
}

int print_done(const std::expected<std::string, core::Error>& done, const core::Config& config) {
    if (!done)
        return fail(done.error());
    std::cout << sanitize_for_terminal(*done) << '\n';
    start_background_sync(config, false);
    return kExitOk;
}

// get HEDEF [ANAHTAR] | set HEDEF ANAHTAR DEĞER [--type T] | rm HEDEF ANAHTAR; hedef `target`ın kurduğu konu.
int run_verb(const PropArgs& args, std::string_view command, const core::Config& config,
             const std::function<std::expected<app::InspectTarget, int>(const std::string&)>& target) {
    const std::size_t want = args.verb == "set" ? 3 : 2;
    const bool get = args.verb == "get";
    if ((get && (args.positional.empty() || args.positional.size() > 2)) ||
        (!get && args.positional.size() != want))
        return usage_error(command);
    const auto subject = target(args.positional[0]);
    if (!subject)
        return subject.error();
    core::NoteStore store = core::NoteStore::for_config(config);
    if (get) {
        const auto props = subject->kind == app::InspectTarget::Kind::Note
                               // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- of_note: hep dolu
                               ? core::note_properties(store, *subject->note)
                               : core::relation_properties(store, subject->relation);
        if (!props)
            return fail(props.error());
        return print_props(*props, args);
    }
    app::Inspector inspector;
    inspector.inspect(*subject);
    if (args.verb == "set")
        return print_done(inspector.set(store, args.positional[1], args.positional[2], args.type), config);
    return print_done(inspector.remove(store, args.positional[1]), config);
}

bool known_verb(std::string_view verb) {
    return verb == "get" || verb == "set" || verb == "rm";
}

}  // namespace

int run_prop(const ParsedArgs& args, const core::Config& config) {
    const auto parsed = parse_prop_args(args.rest, "prop");
    if (!parsed)
        return parsed.error();
    if (!known_verb(parsed->verb))
        return usage_error("prop");
    return run_verb(*parsed, "prop", config,
                    [](const std::string& text) -> std::expected<app::InspectTarget, int> {
                        auto path = core::NotePath::parse(text);
                        if (!path)
                            return std::unexpected(fail(path.error()));
                        return app::InspectTarget::of_note(std::move(*path));
                    });
}

int run_relation_prop(std::span<const std::string> rest, const core::Config& config) {
    const auto parsed = parse_prop_args(rest, "relation");
    if (!parsed)
        return parsed.error();
    if (!known_verb(parsed->verb))
        return usage_error("relation");
    return run_verb(
        *parsed, "relation", config, [](const std::string& text) -> std::expected<app::InspectTarget, int> {
            const auto id = relation_id_from_hex(text);
            if (!id) {
                std::cerr << sanitize_for_terminal(core::tr("cli.relation_bad_id", {{"id", text}})) << '\n';
                return std::unexpected(kExitUsage);
            }
            return app::InspectTarget::of_relation(*id);
        });
}

}  // namespace noteapp::cli
