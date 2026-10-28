#include "core/editor.hpp"

#include "core/process.hpp"

#include <array>
#include <cctype>
#include <utility>

namespace noteapp::core {

namespace {

#ifdef _WIN32
constexpr std::array<std::string_view, 2> kPlatformCandidates = {"edit", "notepad"};
#else
constexpr std::array<std::string_view, 2> kPlatformCandidates = {"nano", "vi"};
#endif

}  // namespace

// Ters eğik çizgi bilerek düz karakterdir: Windows yolları (C:\Program Files\x.exe) bozulmasın.
// Tek istisna çift tırnak içindeki \" dizisidir (kaçışlı tırnak). Tek tırnak içinde hiçbir şey
// kaçışlanmaz. `in_token`, boş tırnaklı argümanın ("") kaybolmamasını sağlar.
std::vector<std::string> split_command_line(std::string_view command) {
    std::vector<std::string> args;
    std::string current;
    bool in_token = false;
    char quote = '\0';
    for (std::size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (quote != '\0') {
            if (c == quote)
                quote = '\0';
            else if (quote == '"' && c == '\\' && i + 1 < command.size() && command[i + 1] == '"')
                current += command[++i];
            else
                current += c;
        } else if (c == '"' || c == '\'') {
            quote = c;
            in_token = true;
        } else if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (in_token)
                args.push_back(std::exchange(current, ""));
            in_token = false;
        } else {
            current += c;
            in_token = true;
        }
    }
    if (in_token)
        args.push_back(std::move(current));
    return args;
}

namespace {

std::string candidate_list() {
    std::string joined;
    for (const std::string_view name : kPlatformCandidates)
        joined += (joined.empty() ? "" : ", ") + std::string(name);
    return joined;
}

}  // namespace

std::expected<std::vector<std::string>, Error> resolve_editor(const Config& config) {
    for (const std::string& command :
         {config.editor, get_env_utf8("VISUAL").value_or(""), get_env_utf8("EDITOR").value_or("")}) {
        if (auto argv = split_command_line(command); !argv.empty())
            return argv;
    }
    for (const std::string_view name : kPlatformCandidates) {
        if (find_program(name))
            return std::vector<std::string>{std::string(name)};
    }
    return std::unexpected(
        Error{Error::Code::EditorNotFound, "error.editor_not_found", {{"candidates", candidate_list()}}});
}

std::expected<void, Error> run_editor(const Config& config, const std::filesystem::path& file) {
    auto argv = resolve_editor(config);
    if (!argv)
        return std::unexpected(argv.error());
    const std::string editor = argv->front();
    const std::u8string path = file.u8string();
    argv->emplace_back(path.begin(), path.end());

    const auto code = run_and_wait(*argv);
    if (!code) {
        if (code.error().code == Error::Code::NotFound)
            return std::unexpected(
                Error{Error::Code::EditorNotFound, "error.editor_not_found", {{"candidates", editor}}});
        return std::unexpected(code.error());
    }
    if (*code != 0)
        return std::unexpected(Error{Error::Code::EditorFailed,
                                     "error.editor_failed",
                                     {{"editor", editor}, {"code", std::to_string(*code)}}});
    return {};
}

}  // namespace noteapp::core
