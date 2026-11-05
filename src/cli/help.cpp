// note help [KOMUT] ve `note KOMUT -h|--help` (SPEC-cli "Yardım"). Kullanım satırı `cli.<komut>_usage`,
// seçenekler ve fiiller command_table.hpp'den gelir; açıklama, örnekler ve "ayrıca bakınız" katalogdadır. Yardım
// elle yazılmış seçenek listesi taşımaz: tablo ile metin ayrışmasın.
#include "cli/command_table.hpp"
#include "cli/commands.hpp"
#include "cli/highlight.hpp"
#include "cli/suggest.hpp"
#include "cli/term.hpp"
#include "core/i18n.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noteapp::cli {

namespace {

// Okunabilir satır uzunluğu; terminal daha darsa onun genişliği. Altındaki genişlikte sütunlar anlamını
// yitirir.
constexpr std::size_t kMaxWidth = 80;
constexpr std::size_t kMinWidth = 40;
// İki sütunlu satırlarda sol sütunun en çok payı; daha uzun etiketin açıklaması alt satıra iner.
constexpr std::size_t kMaxLabelColumn = 30;
constexpr std::string_view kBoldOn = "\x1b[1m";
constexpr std::string_view kBoldOff = "\x1b[0m";

std::size_t output_width() {
    const unsigned columns = term_size().columns;
    return columns == 0 ? kMaxWidth : std::clamp<std::size_t>(columns, kMinWidth, kMaxWidth);
}

// Terminal sütunu yaklaşık kod noktasıdır (ek kütüphane yok; katalog metni geniş karakter taşımaz).
std::size_t columns_of(std::string_view text) {
    return static_cast<std::size_t>(
        std::ranges::count_if(text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

bool has_key(const std::string& key) {
    return core::tr(key) != key;
}

class HelpWriter {
public:
    explicit HelpWriter(std::size_t width) : width_(width), bold_(color_enabled(Stream::Out)) {}

    void heading(std::string_view text) {
        if (!out_.empty() && !out_.ends_with("\n\n"))
            out_ += '\n';
        if (bold_)
            out_.append(kBoldOn).append(text).append(kBoldOff);
        else
            out_.append(text);
        out_ += '\n';
    }

    void blank() { out_ += '\n'; }

    // `lead` satırın başıdır; metin sözcük sınırında sarılır, devam satırları `indent` sütunundan başlar.
    void block(std::string lead, std::string_view text, std::size_t indent) {
        std::string line = std::move(lead);
        std::size_t used = columns_of(line);
        bool has_word = false;
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t end = std::min(text.find(' ', start), text.size());
            const std::string_view word = text.substr(start, end - start);
            start = end + 1;
            if (word.empty())
                continue;
            const std::size_t width = columns_of(word);
            if (has_word && used + 1 + width > width_) {
                out_ += line + '\n';
                line.assign(indent, ' ');
                used = indent;
                has_word = false;
            }
            if (has_word) {
                line += ' ';
                ++used;
            }
            line += word;
            used += width;
            has_word = true;
        }
        out_ += line + '\n';
    }

    // Satır başına (etiket, açıklama); açıklamalar ortak sütunda hizalanır. `column` 0 ise en uzun etiketten.
    void rows(const std::vector<std::pair<std::string, std::string>>& entries, std::size_t column = 0) {
        if (column == 0) {
            for (const auto& [label, text] : entries)
                column = std::max(column, columns_of(label) + 2);
        }
        column = std::min(column, kMaxLabelColumn);
        for (const auto& [label, text] : entries) {
            const std::size_t used = columns_of(label);
            if (used + 2 > column) {
                out_ += label + '\n';
                block(std::string(column, ' '), text, column);
            } else {
                block(label + std::string(column - used, ' '), text, column);
            }
        }
    }

    const std::string& text() const { return out_; }

private:
    std::size_t width_;
    bool bold_;
    std::string out_;
};

std::string placeholder(ValueKind kind) {
    switch (kind) {
    case ValueKind::None:
        return "";
    case ValueKind::Text:
        return core::tr("cli.value_text");
    case ValueKind::Note:
        return core::tr("cli.value_note");
    case ValueKind::Folder:
        return core::tr("cli.value_folder");
    case ValueKind::Type:
        return core::tr("cli.value_type");
    case ValueKind::View:
        return core::tr("cli.value_view");
    case ValueKind::ConfigKey:
        return core::tr("cli.value_config_key");
    case ValueKind::Query:
        return core::tr("cli.value_query");
    case ValueKind::Command:
        return core::tr("cli.value_command");
    case ValueKind::File:
        return core::tr("cli.value_file");
    case ValueKind::Directory:
        return core::tr("cli.value_directory");
    }
    return "";
}

// "  -f, --folder KLASÖR"; yalnız uzun adı olan seçenek kısa adların sütununa hizalanır.
std::string option_label(const OptionSpec& option, std::size_t indent) {
    const bool name_is_long = option.name.starts_with("--");
    const std::string_view short_name = name_is_long ? option.alias : option.name;
    const std::string_view long_name = name_is_long ? option.name : option.alias;
    std::string label(indent, ' ');
    if (short_name.empty())
        label += "    ";
    else
        label.append(short_name).append(long_name.empty() ? "" : ", ");
    label.append(long_name);
    if (const std::string value = placeholder(option.value); !value.empty())
        label += ' ' + value;
    return label;
}

void option_rows(std::vector<std::pair<std::string, std::string>>& rows, std::span<const OptionSpec> options,
                 std::size_t indent) {
    for (const OptionSpec& option : options)
        rows.emplace_back(option_label(option, indent), core::tr(option.description));
}

// `Usage: note export --pdf ... | note export --all ...` -> her biçim ayrı satır; "Usage:" öneki başlıktır.
std::vector<std::string> usage_forms(std::string_view command) {
    const std::string usage = core::tr("cli." + std::string(command) + "_usage");
    const std::string prefix = "note " + std::string(command);
    std::vector<std::string> forms;
    std::size_t start = usage.find(prefix);
    while (start != std::string::npos) {
        const std::size_t next = usage.find(" | " + prefix, start + 1);
        forms.push_back(usage.substr(start, next == std::string::npos ? std::string::npos : next - start));
        start = next == std::string::npos ? next : next + 3;
    }
    return forms;
}

// Katalogdaki satır başına "SOL<TAB>AÇIKLAMA" listesi (örnekler, hızlı ekleme biçimleri).
std::vector<std::pair<std::string, std::string>> catalog_rows(const std::string& key) {
    std::vector<std::pair<std::string, std::string>> rows;
    const std::string text = core::tr(key);
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = std::string_view(text).substr(start, end - start);
        start = end + 1;
        if (line.empty())
            continue;
        const std::size_t tab = line.find('\t');
        rows.emplace_back("  " + std::string(line.substr(0, tab)),
                          tab == std::string_view::npos ? std::string() : std::string(line.substr(tab + 1)));
    }
    return rows;
}

void examples(HelpWriter& writer, std::string_view command) {
    const std::string key = "cli.examples_" + std::string(command);
    if (!has_key(key))
        return;
    writer.heading(core::tr("cli.help_heading_examples"));
    writer.rows(catalog_rows(key));
}

std::string_view group_heading(CommandGroup group) {
    switch (group) {
    case CommandGroup::Notes:
        return "cli.group_notes";
    case CommandGroup::Sync:
        return "cli.group_sync";
    case CommandGroup::Advanced:
        return "cli.group_advanced";
    case CommandGroup::Other:
        return "cli.group_other";
    }
    return "cli.group_other";
}

// `note help`: hızlı ekleme biçimleri ve seçenekleri, sonra gruplu komut listesi (tek satırlık `cli.desc_*`).
std::string top_help() {
    HelpWriter writer(output_width());
    writer.heading(core::tr("cli.help_heading_usage"));
    writer.rows(catalog_rows("cli.help"));
    writer.heading(core::tr("cli.help_heading_options"));
    std::vector<std::pair<std::string, std::string>> options;
    option_rows(options, kAddOptions, 2);
    writer.rows(options);
    // Gruplar tek sütunda hizalanır: liste bir bütün olarak okunur.
    std::size_t column = 0;
    for (const CommandSpec& command : kCommands)
        column = std::max(column, columns_of("  note " + std::string(command.name)) + 2);
    for (const CommandGroup group :
         {CommandGroup::Notes, CommandGroup::Sync, CommandGroup::Advanced, CommandGroup::Other}) {
        writer.heading(core::tr(group_heading(group)));
        std::vector<std::pair<std::string, std::string>> rows;
        for (const CommandSpec& command : kCommands)
            if (command.group == group)
                rows.emplace_back("  note " + std::string(command.name), core::tr(command.description));
        writer.rows(rows, column);
    }
    writer.blank();
    const std::string footer = core::tr("cli.help_footer");
    std::size_t start = 0;
    while (start < footer.size()) {
        const std::size_t end = std::min(footer.find('\n', start), footer.size());
        writer.block("", std::string_view(footer).substr(start, end - start), 0);
        start = end + 1;
    }
    return writer.text();
}

std::string command_help(const CommandSpec& command) {
    HelpWriter writer(output_width());
    const std::string name(command.name);
    writer.block("note " + name + "  ", core::tr(command.description), 2);
    if (const std::string key = "cli.help_" + name; has_key(key)) {
        writer.blank();
        writer.block("", core::tr(key), 0);
    }

    writer.heading(core::tr("cli.help_heading_usage"));
    for (const std::string& form : usage_forms(command.name))
        writer.block("  ", form, 4);

    if (!command.verbs.empty()) {
        writer.heading(core::tr("cli.help_heading_commands"));
        std::vector<std::pair<std::string, std::string>> rows;
        for (const VerbSpec& verb : command.verbs) {
            std::string label = "  " + std::string(verb.name);
            if (const std::string value = placeholder(verb.operand); !value.empty())
                label += ' ' + value;
            rows.emplace_back(std::move(label), core::tr(verb.description));
            option_rows(rows, verb.options, 4);
        }
        writer.rows(rows);
    }

    writer.heading(core::tr("cli.help_heading_options"));
    std::vector<std::pair<std::string, std::string>> rows;
    option_rows(rows, command.options, 2);
    constexpr OptionSpec kHelp{"--help", "-h", ValueKind::None, "cli.opt_help"};
    option_rows(rows, std::span(&kHelp, 1), 2);
    writer.rows(rows);

    examples(writer, command.name);
    if (const std::string key = "cli.see_also_" + name; has_key(key)) {
        writer.blank();
        writer.block(core::tr("cli.help_heading_see_also") + ' ', core::tr(key), 2);
    }
    return writer.text();
}

}  // namespace

int run_help(const ParsedArgs& args) {
    if (args.command.empty() || args.command == "help") {
        std::cout << top_help();
        return kExitOk;
    }
    const CommandSpec* command = find_command(args.command);
    if (command == nullptr) {
        std::cerr << sanitize_for_terminal(core::tr("cli.unknown_command", {{"command", args.command}})) << '\n';
        std::vector<std::string_view> names;
        for (const CommandSpec& candidate : kCommands)
            names.push_back(candidate.name);
        if (const auto guess = suggest(args.command, names))
            std::cerr << core::tr("cli.hint_did_you_mean", {{"word", std::string(*guess)}}) << '\n';
        std::cerr << core::tr("cli.hint_help_list") << '\n';
        return kExitUsage;
    }
    std::cout << command_help(*command);
    return kExitOk;
}

}  // namespace noteapp::cli
