#include "core/config.hpp"

#include "core/atomic_file.hpp"
#include "core/paths.hpp"
#include "core/resources.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <climits>
#include <cstdint>
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
    Section(const toml::table& table, std::string prefix, std::vector<Error>& warnings)
        : table_(table), prefix_(std::move(prefix)), warnings_(warnings) {}

    void invalid(std::string_view key, std::string_view rule) {
        warnings_.push_back(Error{Error::Code::ConfigInvalid,
                                  "config.invalid_value",
                                  {{"key", prefix_ + std::string(key)}, {"reason", std::string(rule)}}});
    }

    void warn(std::string_view key, std::string message_key) {
        warnings_.push_back(
            Error{Error::Code::ConfigInvalid, std::move(message_key), {{"key", prefix_ + std::string(key)}}});
    }

    std::optional<std::string> string(std::string_view key) {
        const toml::node* node = find(key);
        if (node == nullptr)
            return std::nullopt;
        if (const auto* value = node->as_string())
            return value->get();
        invalid(key, "must_be_string");
        return std::nullopt;
    }

    std::optional<bool> boolean(std::string_view key) {
        const toml::node* node = find(key);
        if (node == nullptr)
            return std::nullopt;
        if (const auto* value = node->as_boolean())
            return value->get();
        invalid(key, "must_be_bool");
        return std::nullopt;
    }

    // Tam sayı tuning/ayar değerleri hep pozitiftir; `limit` hedef türün taşmasını önler.
    std::optional<std::int64_t> positive_integer(std::string_view key, std::int64_t limit = INT64_MAX) {
        const toml::node* node = find(key);
        if (node == nullptr)
            return std::nullopt;
        const auto* value = node->as_integer();
        if (value == nullptr || value->get() <= 0 || value->get() > limit) {
            invalid(key, "must_be_positive_integer");
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
            invalid(key, "must_be_fraction");
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
            invalid(key, "must_be_string_array");
            return std::nullopt;
        }
        std::vector<std::string> out;
        for (const toml::node& element : *array) {
            const auto* value = element.as_string();
            if (value == nullptr || !is_valid_type(value->get())) {
                invalid(key, "must_be_type_names");
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
        invalid(key, "must_be_table");
        return std::nullopt;
    }

    // Tüm anahtarlar okunduktan sonra çağrılır; okunmayanlar bilinmeyen anahtardır.
    void warn_unknown() {
        for (const auto& [key, node] : table_) {
            if (std::find(known_.begin(), known_.end(), key.str()) == known_.end())
                warnings_.push_back(Error{Error::Code::ConfigInvalid,
                                          "config.unknown_key",
                                          {{"key", prefix_ + std::string(key.str())}}});
        }
    }

private:
    const toml::node* find(std::string_view key) {
        known_.emplace_back(key);
        return table_.get(key);
    }

    const toml::table& table_;
    std::string prefix_;
    std::vector<Error>& warnings_;
    std::vector<std::string_view> known_;
};

// https://kullanıcı:token@host/… token'ı düz metin config.toml'a (ve senkronla başka makinelere) taşır; token
// kasada durur (`note sync login`). http(s) URL'sinde "://"dan sonraki yetki kısmında '@' varsa kimlik var
// sayılır ve kimliksiz URL döner; yoksa nullopt. SSH'nin kullanıcı adı (git@host:…, ssh://git@host/…) sır
// değildir. core sync'e bağımlı olamaz: sync::parse_remote'un küçük, yerel eşi.
std::optional<std::string> without_credentials(std::string_view remote) {
    const std::size_t scheme_end = remote.find("://");
    if (scheme_end == std::string_view::npos)
        return std::nullopt;
    std::string scheme(remote.substr(0, scheme_end));
    std::ranges::transform(scheme, scheme.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (scheme != "http" && scheme != "https")
        return std::nullopt;
    const std::size_t host_start = scheme_end + 3;
    const std::string_view rest = remote.substr(host_start);
    const std::size_t at = rest.substr(0, rest.find_first_of("/?#")).rfind('@');
    if (at == std::string_view::npos)
        return std::nullopt;
    return std::string(remote.substr(0, host_start)) + std::string(rest.substr(at + 1));
}

void apply_sync(Section& s, SyncConfig& sync) {
    if (auto v = s.boolean("enabled"))
        sync.enabled = *v;
    if (auto v = s.string("remote")) {
        // Elle yazılmış dosya: remote kaybolmasın, sır bellekte kalmasın ve sonraki kayıtta dosyadan
        // silinsin. URL'nin ham hâli uyarıya yazılmaz.
        if (auto clean = without_credentials(*v)) {
            s.warn("remote", "config.remote_credentials_stripped");
            sync.remote = std::move(*clean);
        } else {
            sync.remote = std::move(*v);
        }
    }
    if (auto v = s.string("branch")) {
        if (v->empty())
            s.invalid("branch", "must_not_be_empty");
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
    if (auto v = s.positive_integer("log_max_bytes"))
        tuning.log_max_bytes = static_cast<std::uintmax_t>(*v);
    if (auto v = s.positive_integer("sync_stale_lock_minutes", INT_MAX))
        tuning.sync_stale_lock_minutes = static_cast<int>(*v);
    if (auto v = s.positive_integer("sync_timeout_seconds", INT_MAX))
        tuning.sync_timeout_seconds = static_cast<int>(*v);
    s.warn_unknown();
}

// `config` önceki katmanın değerlerini taşır; yalnız geçerli anahtarlar üzerine yazılır.
void apply(const toml::table& root, Config& config, std::vector<Error>& warnings) {
    Section top(root, "", warnings);
    if (auto v = top.string("language")) {
        if (std::find(kLanguages.begin(), kLanguages.end(), *v) == kLanguages.end())
            top.invalid("language", "must_be_language");
        else
            config.language = std::move(*v);
    }
    if (auto v = top.string("notes_dir")) {
        if (v->empty())
            top.invalid("notes_dir", "must_not_be_empty");
        else
            config.notes_dir = expand_user_path(*v);
    }
    if (auto v = top.string("editor"))
        config.editor = std::move(*v);
    if (auto v = top.string("default_type")) {
        if (!is_valid_type(*v))
            top.invalid("default_type", "must_be_type_name");
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
std::optional<toml::table> parse_table(std::string_view text, Error& error) {
    toml::parse_result result = toml::parse(text);
    if (!result) {
        const toml::parse_error& e = result.error();
        error = Error{Error::Code::ConfigInvalid,
                      "config.syntax_error",
                      {{"line", std::to_string(e.source().begin.line)},
                       {"column", std::to_string(e.source().begin.column)},
                       {"detail", std::string(e.description())}}};
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

// Kullanıcıya dönük ayarlar (tuning ve token hariç): anahtar, değer türü, değeri metne çeviren işlev.
// `set_config_value` ve `config_entries` aynı tablodan beslenir; şema tek yerde.
enum class ValueKind { String, Bool, Integer, StringList };

struct Setting {
    std::string_view key;
    ValueKind kind;
    std::string (*get)(const Config&);
};

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i)
        out += (i == 0 ? "" : ", ") + items[i];
    return out;
}

const std::array<Setting, 12>& settings() {
    static const std::array<Setting, 12> table = {{
        {"language", ValueKind::String, [](const Config& c) { return c.language; }},
        {"notes_dir", ValueKind::String, [](const Config& c) { return to_utf8(c.notes_dir); }},
        {"editor", ValueKind::String, [](const Config& c) { return c.editor; }},
        {"default_type", ValueKind::String, [](const Config& c) { return c.default_type; }},
        {"default_folder", ValueKind::String, [](const Config& c) { return c.default_folder; }},
        {"types", ValueKind::StringList, [](const Config& c) { return join(c.types); }},
        {"sync.enabled", ValueKind::Bool,
         [](const Config& c) { return std::string(c.sync.enabled ? "true" : "false"); }},
        {"sync.remote", ValueKind::String, [](const Config& c) { return c.sync.remote; }},
        {"sync.branch", ValueKind::String, [](const Config& c) { return c.sync.branch; }},
        {"sync.idle_seconds", ValueKind::Integer,
         [](const Config& c) { return std::to_string(c.sync.idle_seconds); }},
        {"sync.author_name", ValueKind::String, [](const Config& c) { return c.sync.author_name; }},
        {"sync.author_email", ValueKind::String, [](const Config& c) { return c.sync.author_email; }},
    }};
    return table;
}

// [tuning] anahtarları ve TOML metinleri. save_config yalnız gömülü varsayılandan farklı olanları yazar:
// kullanıcının elle verdiği ince ayar uygulamanın kaydında kaybolmaz, varsayılanlar dosyaya gömülmez (sonraki
// sürümün varsayılanı geçerli olur). Yeni anahtar apply_tuning'e, buraya ve gidiş-dönüş testine birlikte
// eklenir.
struct TuningKey {
    std::string_view key;
    std::string (*text)(const Tuning&);
};

std::string fraction_text(double value) {
    std::ostringstream out;
    out << toml::value<double>(value);  // gidiş-dönüş hassasiyetini toml++ korur
    return out.str();
}

const std::array<TuningKey, 10>& tuning_keys() {
    static const std::array<TuningKey, 10> table = {{
        {"slug_max_length", [](const Tuning& t) { return std::to_string(t.slug_max_length); }},
        {"min_confidence", [](const Tuning& t) { return fraction_text(t.min_confidence); }},
        {"max_scan_bytes", [](const Tuning& t) { return std::to_string(t.max_scan_bytes); }},
        {"max_note_bytes", [](const Tuning& t) { return std::to_string(t.max_note_bytes); }},
        {"rescan_seconds", [](const Tuning& t) { return std::to_string(t.rescan_seconds); }},
        {"autosave_ms", [](const Tuning& t) { return std::to_string(t.autosave_ms); }},
        {"exit_timeout_seconds", [](const Tuning& t) { return std::to_string(t.exit_timeout_seconds); }},
        {"log_max_bytes", [](const Tuning& t) { return std::to_string(t.log_max_bytes); }},
        {"sync_stale_lock_minutes",
         [](const Tuning& t) { return std::to_string(t.sync_stale_lock_minutes); }},
        {"sync_timeout_seconds", [](const Tuning& t) { return std::to_string(t.sync_timeout_seconds); }},
    }};
    return table;
}

std::string_view trim(std::string_view text) {
    constexpr std::string_view kBlanks = " \t";
    const std::size_t first = text.find_first_not_of(kBlanks);
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(kBlanks) - first + 1);
}

}  // namespace

std::vector<std::pair<std::string, std::string>> config_entries(const Config& config) {
    std::vector<std::pair<std::string, std::string>> entries;
    for (const Setting& setting : settings())
        entries.emplace_back(std::string(setting.key), setting.get(config));
    return entries;
}

std::expected<void, Error> set_config_value(Config& config, std::string_view key, std::string_view value) {
    const auto setting = std::ranges::find(settings(), key, &Setting::key);
    if (setting == settings().end())
        return std::unexpected(
            Error{Error::Code::ConfigInvalid, "config.unknown_key", {{"key", std::string(key)}}});
    // Dosyanın aksine burada kaydedilecek yeni değer söz konusu: soyup kabul etmek yerine reddedilir ki
    // kullanıcı token'ını `note sync login` ile kaydetsin. Değer hataya yazılmaz.
    if (key == "sync.remote" && without_credentials(value))
        return std::unexpected(Error{Error::Code::ConfigInvalid,
                                     "config.invalid_value",
                                     {{"key", std::string(key)}, {"reason", "remote_has_credentials"}}});

    // Değer, şemadaki türüyle TOML düğümüne çevrilir ve dosya yüklemesiyle AYNI `apply` doğrulamasından
    // geçer. Türe uymayan metin (ör. "abc" tamsayı için) string kalır; apply bunu tür hatası olarak raporlar.
    toml::table root;
    toml::table* target = &root;
    std::string_view leaf = key;
    if (const auto dot = key.find('.'); dot != std::string_view::npos) {
        target = root.insert_or_assign(key.substr(0, dot), toml::table{}).first->second.as_table();
        leaf = key.substr(dot + 1);
    }
    switch (setting->kind) {
    case ValueKind::Bool:
        if (value == "true" || value == "false")
            target->insert_or_assign(leaf, value == "true");
        else
            target->insert_or_assign(leaf, std::string(value));
        break;
    case ValueKind::Integer: {
        std::int64_t number = 0;
        const char* end = value.data() + value.size();
        if (const auto parsed = std::from_chars(value.data(), end, number);
            parsed.ec == std::errc{} && parsed.ptr == end)
            target->insert_or_assign(leaf, number);
        else
            target->insert_or_assign(leaf, std::string(value));
        break;
    }
    case ValueKind::StringList: {
        toml::array items;
        for (std::size_t start = 0;;) {
            const std::size_t comma = value.find(',', start);
            items.push_back(std::string(trim(value.substr(start, comma - start))));
            if (comma == std::string_view::npos)
                break;
            start = comma + 1;
        }
        target->insert_or_assign(leaf, std::move(items));
        break;
    }
    case ValueKind::String:
        target->insert_or_assign(leaf, std::string(value));
        break;
    }

    Config candidate = config;
    std::vector<Error> problems;
    apply(root, candidate, problems);
    if (!problems.empty())
        return std::unexpected(std::move(problems.front()));
    config = std::move(candidate);
    return {};
}

LoadedConfig parse_config(std::string_view user_toml) {
    LoadedConfig loaded{};
    Error error{Error::Code::ConfigInvalid, "", {}};

    const auto defaults_text = embedded_resource("defaults.toml");
    const auto defaults = defaults_text ? parse_table(*defaults_text, error) : std::nullopt;
    if (!defaults) {
        // Derleme hatası sayılır (testler yakalar); yine de çökmeden uyarıyla sıfır değerler döner.
        // Ayrıştırma başarılıysa (resource yok) error boş kalır; ikisi de aynı uyarıya iner.
        error.message_key = "config.defaults_unusable";
        loaded.warnings.push_back(std::move(error));
        return loaded;
    }
    apply(*defaults, loaded.config, loaded.warnings);

    // Kısmi uygulama yok: sözdizimi hatalı dosya bütünüyle yok sayılır (yarım yapılandırma şaşırtır).
    if (auto user = parse_table(user_toml, error))
        apply(*user, loaded.config, loaded.warnings);
    else
        loaded.warnings.push_back(std::move(error));
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
        loaded.warnings.push_back(
            Error{Error::Code::ConfigInvalid, "config.unreadable", {{"path", to_utf8(file)}}});
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

    const Tuning defaults = parse_config("").config.tuning;
    std::string tuning;
    for (const TuningKey& entry : tuning_keys()) {
        if (const std::string value = entry.text(config.tuning); value != entry.text(defaults))
            tuning += std::string(entry.key) + " = " + value + "\n";
    }
    if (!tuning.empty())
        out << "\n[tuning]\n" << tuning;

    const fs::path dir = config_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        return std::unexpected(io_error(dir, ec.message()));
    return write_file_atomically(config_file(), out.str());
}

}  // namespace noteapp::core
