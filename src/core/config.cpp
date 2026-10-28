#include "core/config.hpp"

#include "core/atomic_file.hpp"
#include "core/paths.hpp"
#include "core/resources.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <toml++/toml.hpp>

namespace noteapp::core {

namespace fs = std::filesystem;

namespace {

constexpr std::array<std::string_view, 3> kLanguages = {"auto", "tr", "en"};

bool is_valid_type(std::string_view type) {
    return !type.empty() && type.find('.') == std::string_view::npos;
}

// Bir TOML tablosunu okur: tür uyuşmazlığı ve bilinmeyen anahtar uyarı olur, değer uygulanmaz.
// Uygulanmayan alan çağıranın elindeki (önceki katmandan gelen) değeri korur → "varsayılana düş".
class Section {
public:
    Section(const toml::table& table, std::string prefix, std::vector<std::string>& warnings)
        : table_(table), prefix_(std::move(prefix)), warnings_(warnings) {}

    void invalid(std::string_view key, std::string_view rule) {
        warnings_.push_back(std::format("config: '{}{}' {}; using the default", prefix_, key, rule));
    }

    std::optional<std::string> string(std::string_view key) {
        const toml::node* node = find(key);
        if (node == nullptr)
            return std::nullopt;
        if (const auto* value = node->as_string())
            return value->get();
        invalid(key, "must be a string");
        return std::nullopt;
    }

    std::optional<bool> boolean(std::string_view key) {
        const toml::node* node = find(key);
        if (node == nullptr)
            return std::nullopt;
        if (const auto* value = node->as_boolean())
            return value->get();
        invalid(key, "must be true or false");
        return std::nullopt;
    }

    // Tam sayı tuning/ayar değerleri hep pozitiftir; `limit` hedef türün taşmasını önler.
    std::optional<std::int64_t> positive_integer(std::string_view key, std::int64_t limit = INT64_MAX) {
        const toml::node* node = find(key);
        if (node == nullptr)
            return std::nullopt;
        const auto* value = node->as_integer();
        if (value == nullptr || value->get() <= 0 || value->get() > limit) {
            invalid(key, "must be a positive integer");
            return std::nullopt;
        }
        return value->get();
    }

    std::optional<double> fraction(std::string_view key) {
        const toml::node* node = find(key);
        if (node == nullptr)
            return std::nullopt;
        const auto* value = node->as_floating_point();
        const auto* integer = node->as_integer();
        const std::optional<double> number = value != nullptr     ? std::optional<double>(value->get())
                                             : integer != nullptr ? std::optional<double>(integer->get())
                                                                  : std::nullopt;
        if (!number || *number < 0.0 || *number > 1.0) {
            invalid(key, "must be a number between 0 and 1");
            return std::nullopt;
        }
        return number;
    }

    std::optional<std::vector<std::string>> string_array(std::string_view key) {
        const toml::node* node = find(key);
        if (node == nullptr)
            return std::nullopt;
        const auto* array = node->as_array();
        if (array == nullptr) {
            invalid(key, "must be an array of strings");
            return std::nullopt;
        }
        std::vector<std::string> out;
        for (const toml::node& element : *array) {
            const auto* value = element.as_string();
            if (value == nullptr || !is_valid_type(value->get())) {
                invalid(key, "must contain only non-empty names without dots");
                return std::nullopt;
            }
            out.push_back(value->get());
        }
        return out;
    }

    // Alt tablo; var ama tablo değilse uyarı verir.
    std::optional<Section> subsection(std::string_view key) {
        const toml::node* node = find(key);
        if (node == nullptr)
            return std::nullopt;
        if (const auto* table = node->as_table())
            return Section(*table, prefix_ + std::string(key) + ".", warnings_);
        invalid(key, "must be a table");
        return std::nullopt;
    }

    // Tüm anahtarlar okunduktan sonra çağrılır; okunmayanlar bilinmeyen anahtardır.
    void warn_unknown() {
        for (const auto& [key, node] : table_) {
            if (std::find(known_.begin(), known_.end(), key.str()) == known_.end())
                warnings_.push_back(std::format("config: unknown key '{}{}' ignored", prefix_, key.str()));
        }
    }

private:
    const toml::node* find(std::string_view key) {
        known_.emplace_back(key);
        return table_.get(key);
    }

    const toml::table& table_;
    std::string prefix_;
    std::vector<std::string>& warnings_;
    std::vector<std::string_view> known_;
};

void apply_sync(Section& s, SyncConfig& sync) {
    if (auto v = s.boolean("enabled"))
        sync.enabled = *v;
    if (auto v = s.string("remote"))
        sync.remote = std::move(*v);
    if (auto v = s.string("branch")) {
        if (v->empty())
            s.invalid("branch", "must not be empty");
        else
            sync.branch = std::move(*v);
    }
    if (auto v = s.positive_integer("idle_seconds", INT_MAX))
        sync.idle_seconds = static_cast<int>(*v);
    if (auto v = s.string("author_name"))
        sync.author_name = std::move(*v);
    if (auto v = s.string("author_email"))
        sync.author_email = std::move(*v);
    s.warn_unknown();
}

void apply_tuning(Section& s, Tuning& tuning) {
    if (auto v = s.positive_integer("slug_max_length"))
        tuning.slug_max_length = static_cast<std::size_t>(*v);
    if (auto v = s.fraction("min_confidence"))
        tuning.min_confidence = *v;
    if (auto v = s.positive_integer("max_scan_bytes"))
        tuning.max_scan_bytes = static_cast<std::size_t>(*v);
    if (auto v = s.positive_integer("max_note_bytes"))
        tuning.max_note_bytes = static_cast<std::uintmax_t>(*v);
    if (auto v = s.positive_integer("rescan_seconds", INT_MAX))
        tuning.rescan_seconds = static_cast<int>(*v);
    if (auto v = s.positive_integer("autosave_ms", INT_MAX))
        tuning.autosave_ms = static_cast<int>(*v);
    if (auto v = s.positive_integer("exit_timeout_seconds", INT_MAX))
        tuning.exit_timeout_seconds = static_cast<int>(*v);
    s.warn_unknown();
}

// `config` önceki katmanın değerlerini taşır; yalnız geçerli anahtarlar üzerine yazılır.
void apply(const toml::table& root, Config& config, std::vector<std::string>& warnings) {
    Section top(root, "", warnings);
    if (auto v = top.string("language")) {
        if (std::find(kLanguages.begin(), kLanguages.end(), *v) == kLanguages.end())
            top.invalid("language", "must be one of auto, tr, en");
        else
            config.language = std::move(*v);
    }
    if (auto v = top.string("notes_dir")) {
        if (v->empty())
            top.invalid("notes_dir", "must not be empty");
        else
            config.notes_dir = expand_user_path(*v);
    }
    if (auto v = top.string("editor"))
        config.editor = std::move(*v);
    if (auto v = top.string("default_type")) {
        if (!is_valid_type(*v))
            top.invalid("default_type", "must be a non-empty name without dots");
        else
            config.default_type = std::move(*v);
    }
    if (auto v = top.string("default_folder"))
        config.default_folder = std::move(*v);
    if (auto v = top.string_array("types"))
        config.types = std::move(*v);
    if (auto sync = top.subsection("sync"))
        apply_sync(*sync, config.sync);
    if (auto tuning = top.subsection("tuning"))
        apply_tuning(*tuning, config.tuning);
    top.warn_unknown();
}

// Ayrıştırma hatası: istisna yok (TOML_EXCEPTIONS=0), parse_result döner.
std::optional<toml::table> parse_table(std::string_view text, std::string& error) {
    toml::parse_result result = toml::parse(text);
    if (!result) {
        const toml::parse_error& e = result.error();
        error = std::format("syntax error at line {}, column {}: {}", e.source().begin.line,
                            e.source().begin.column, e.description());
        return std::nullopt;
    }
    return std::move(result).table();
}

std::string quote(std::string_view text) {
    std::ostringstream out;
    out << toml::value<std::string>(std::string(text));  // kaçışları toml++ yapar
    return out.str();
}

std::string to_utf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

Error io_error(const fs::path& path, std::string_view detail) {
    return Error{Error::Code::Io, "error.io", {{"path", to_utf8(path)}, {"detail", std::string(detail)}}};
}

}  // namespace

LoadedConfig parse_config(std::string_view user_toml) {
    LoadedConfig loaded{};
    std::string error;

    const auto defaults_text = embedded_resource("defaults.toml");
    const auto defaults = defaults_text ? parse_table(*defaults_text, error) : std::nullopt;
    if (!defaults) {
        // Derleme hatası sayılır (testler yakalar); yine de çökmeden uyarıyla sıfır değerler döner.
        loaded.warnings.push_back("config: embedded defaults are unusable " + error);
        return loaded;
    }
    apply(*defaults, loaded.config, loaded.warnings);

    // Kısmi uygulama yok: sözdizimi hatalı dosya bütünüyle yok sayılır (yarım yapılandırma şaşırtır).
    if (auto user = parse_table(user_toml, error))
        apply(*user, loaded.config, loaded.warnings);
    else
        loaded.warnings.push_back("config: " + error + "; using all defaults");
    return loaded;
}

LoadedConfig load_config() {
    const fs::path file = config_file();
    std::error_code ec;
    if (!fs::exists(file, ec))
        return parse_config("");

    std::ifstream in(file, std::ios::binary);
    std::string text(std::istreambuf_iterator<char>(in), {});
    if (!in && !in.eof()) {
        LoadedConfig loaded = parse_config("");
        loaded.warnings.push_back("config: cannot read " + to_utf8(file) + "; using all defaults");
        return loaded;
    }
    return parse_config(text);
}

std::expected<void, Error> save_config(const Config& config) {
    std::ostringstream out;
    out << "# noteapp ayarları. Uygulama bu dosyayı yeniden yazar; yorumlar korunmaz.\n";
    out << "language = " << quote(config.language) << "\n";
    out << "notes_dir = " << quote(to_utf8(config.notes_dir)) << "\n";
    out << "editor = " << quote(config.editor) << "\n";
    out << "default_type = " << quote(config.default_type) << "\n";
    out << "default_folder = " << quote(config.default_folder) << "\n";
    out << "types = [";
    for (std::size_t i = 0; i < config.types.size(); ++i)
        out << (i == 0 ? "" : ", ") << quote(config.types[i]);
    out << "]\n\n[sync]\n";
    out << "enabled = " << (config.sync.enabled ? "true" : "false") << "\n";
    out << "remote = " << quote(config.sync.remote) << "\n";
    out << "branch = " << quote(config.sync.branch) << "\n";
    out << "idle_seconds = " << config.sync.idle_seconds << "\n";
    out << "author_name = " << quote(config.sync.author_name) << "\n";
    out << "author_email = " << quote(config.sync.author_email) << "\n";

    const fs::path dir = config_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        return std::unexpected(io_error(dir, ec.message()));
    return write_file_atomically(config_file(), out.str());
}

}  // namespace noteapp::core
