#include "core/config.hpp"

#include "core/atomic_file.hpp"
#include "core/paths.hpp"
#include "core/resources.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <climits>
#include <cmath>
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
constexpr std::array<std::string_view, 3> kIconModes = {"auto", "nerd", "off"};

bool is_valid_type(std::string_view type) {
    return !type.empty() && type.find('.') == std::string_view::npos;
}

// LSP languageId'leri küçük harf ASCII'dir ("cpp", "shellscript"); bu küme TOML'da çıplak anahtar da olur.
bool is_valid_language_id(std::string_view id) {
    return !id.empty() && id.size() <= LspConfig::kMaxLanguageIdBytes && std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

// Kabuksuz argv: boş olmayan dizgeler, boyut sınırlı. NUL exec'te argümanı keserdi, reddedilir. Boş dizi
// geçerlidir: o dilin (varsayılan) sunucusunu kapatır.
std::optional<std::vector<std::string>> server_command(const toml::node& node) {
    const auto* array = node.as_array();
    if (array == nullptr || array->size() > LspConfig::kMaxArguments)
        return std::nullopt;
    std::vector<std::string> argv;
    for (const toml::node& element : *array) {
        const auto* value = element.as_string();
        if (value == nullptr || value->get().empty() || value->get().size() > LspConfig::kMaxArgumentBytes ||
            value->get().find('\0') != std::string::npos)
            return std::nullopt;
        argv.push_back(value->get());
    }
    return argv;
}

// Tek argv (["clangd"]) ya da argv listesi ([["a"], ["b"]]): dilin bütün sunucuları. Boş dizi tek argv
// yolundan geçer ve o dilin sunucularını kapatır; listedeki her argv boş olmamalıdır.
std::optional<std::vector<std::vector<std::string>>> server_commands(const toml::node& node) {
    const auto* array = node.as_array();
    if (array == nullptr || array->empty() || !array->front().is_array()) {
        auto command = server_command(node);
        if (!command)
            return std::nullopt;
        if (command->empty())
            return std::vector<std::vector<std::string>>{};
        return std::vector<std::vector<std::string>>{std::move(*command)};
    }
    if (array->size() > LspConfig::kMaxServersPerLanguage)
        return std::nullopt;
    std::vector<std::vector<std::string>> commands;
    for (const toml::node& element : *array) {
        auto command = server_command(element);
        if (!command || command->empty())
            return std::nullopt;
        commands.push_back(std::move(*command));
    }
    return commands;
}

// [lsp.settings.<dil>] düğümünün JSON karşılığı. JSON'da karşılığı olmayan (tarih, saat, nan/inf) ya da
// sınırı aşan değer reddedilir: sunucuya yarım ayar gitmesin.
std::optional<Json> settings_json(const toml::node& node, int depth, std::size_t& values) {
    if (depth > LspConfig::kMaxSettingsDepth || ++values > LspConfig::kMaxSettingsValues)
        return std::nullopt;
    if (const auto* table = node.as_table()) {
        Json out{.kind = Json::Kind::Object};
        for (const auto& [key, value] : *table) {
            auto field = settings_json(value, depth + 1, values);
            if (!field)
                return std::nullopt;
            out.fields.emplace(std::string(key.str()), std::move(*field));
        }
        return out;
    }
    if (const auto* array = node.as_array()) {
        Json out{.kind = Json::Kind::Array};
        for (const toml::node& element : *array) {
            auto item = settings_json(element, depth + 1, values);
            if (!item)
                return std::nullopt;
            out.items.push_back(std::move(*item));
        }
        return out;
    }
    if (const auto* text = node.as_string())
        return Json{.kind = Json::Kind::String, .text = text->get()};
    if (const auto* flag = node.as_boolean())
        return Json{.kind = Json::Kind::Bool, .text = flag->get() ? "true" : "false"};
    if (const auto* integer = node.as_integer())
        return Json{.kind = Json::Kind::Number,
                    .text = std::to_string(integer->get()),
                    .number = static_cast<double>(integer->get())};
    if (const auto* real = node.as_floating_point()) {
        const double value = real->get();
        if (!std::isfinite(value))
            return std::nullopt;
        std::array<char, 32> buffer{};
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (error != std::errc{})
            return std::nullopt;
        std::string token(buffer.data(), end);
        // to_chars tamsayı görünümlü double'ı "2" yazar; JSON için yine geçerli sayıdır.
        return Json{.kind = Json::Kind::Number, .text = std::move(token), .number = value};
    }
    return std::nullopt;  // tarih/saat
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

    // [lsp.servers]: languageId -> argv. Bozuk girdi tek tek uyarı olur ve atlanır, geçerliler kalır: tek
    // yazım hatası diğer dillerin sunucusunu kapatmasın.
    std::optional<std::map<std::string, std::vector<std::vector<std::string>>>>
    server_table(std::string_view key) {
        const toml::table* table = table_of(key);
        if (table == nullptr)
            return std::nullopt;
        std::map<std::string, std::vector<std::vector<std::string>>> out;
        for (const auto& [id, value] : *table) {
            const std::string entry = std::string(key) + "." + std::string(id.str());
            if (!is_valid_language_id(id.str()))
                invalid(entry, "must_be_language_id");
            else if (auto commands = server_commands(value); !commands)
                invalid(entry, "must_be_server_command");
            else if (out.size() >= LspConfig::kMaxServers)
                invalid(entry, "too_many_servers");
            else
                out.emplace(id.str(), std::move(*commands));
        }
        return out;
    }

    // [lsp.settings]: languageId -> JSON nesnesi. Bozuk dil girdisi uyarı olur ve atlanır.
    std::optional<std::map<std::string, Json>> settings_table(std::string_view key) {
        const toml::table* table = table_of(key);
        if (table == nullptr)
            return std::nullopt;
        std::map<std::string, Json> out;
        for (const auto& [id, value] : *table) {
            const std::string entry = std::string(key) + "." + std::string(id.str());
            std::size_t values = 0;
            if (!is_valid_language_id(id.str()))
                invalid(entry, "must_be_language_id");
            else if (auto json = value.is_table() ? settings_json(value, 0, values) : std::nullopt; !json)
                invalid(entry, "must_be_settings_table");
            else if (out.size() >= LspConfig::kMaxServers)
                invalid(entry, "too_many_servers");
            else
                out.emplace(id.str(), std::move(*json));
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

    // Anahtar yoksa ya da tablo değilse (uyarıyla) nullptr.
    const toml::table* table_of(std::string_view key) {
        const toml::node* node = find(key);
        if (node == nullptr)
            return nullptr;
        if (const auto* table = node->as_table())
            return table;
        invalid(key, "must_be_table");
        return nullptr;
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

void apply_lsp(Section& s, LspConfig& lsp) {
    if (auto v = s.boolean("enabled"))
        lsp.enabled = *v;
    // Girdi girdi önceki katmanın (varsayılanların) üstüne biner: kullanıcı bir dili ekleyince diğerleri
    // kalır.
    if (auto v = s.server_table("servers"))
        for (auto& [id, commands] : *v) {
            if (commands.empty())
                lsp.servers.erase(id);
            else
                lsp.servers.insert_or_assign(id, std::move(commands));
        }
    if (auto v = s.settings_table("settings"))
        for (auto& [id, json] : *v)
            lsp.settings.insert_or_assign(id, std::move(json));
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
    if (auto v = s.positive_integer("graph_batch_ops", static_cast<std::int64_t>(kMaxGraphBatchOps)))
        tuning.graph_batch_ops = static_cast<std::size_t>(*v);
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
    if (auto v = top.string("icons")) {
        if (std::find(kIconModes.begin(), kIconModes.end(), *v) == kIconModes.end())
            top.invalid("icons", "must_be_icons");
        else
            config.icons = *v == "auto" ? "nerd" : std::move(*v);  // `auto` eski addır, yedek sembol kalktı
    }
    if (auto sync = top.subsection("sync"))
        apply_sync(*sync, config.sync);
    if (auto lsp = top.subsection("lsp"))
        apply_lsp(*lsp, config.lsp);
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
    ValueKind kind{};
    std::string (*get)(const Config&){};
};

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i)
        out += (i == 0 ? "" : ", ") + items[i];
    return out;
}

const std::array<Setting, 13>& settings() {
    static const std::array<Setting, 13> table = {{
        {"language", ValueKind::String, [](const Config& c) { return c.language; }},
        {"notes_dir", ValueKind::String, [](const Config& c) { return to_utf8(c.notes_dir); }},
        {"editor", ValueKind::String, [](const Config& c) { return c.editor; }},
        {"default_type", ValueKind::String, [](const Config& c) { return c.default_type; }},
        {"default_folder", ValueKind::String, [](const Config& c) { return c.default_folder; }},
        {"types", ValueKind::StringList, [](const Config& c) { return join(c.types); }},
        {"icons", ValueKind::String, [](const Config& c) { return c.icons; }},
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
    std::string (*text)(const Tuning&){};
};

std::string fraction_text(double value) {
    std::ostringstream out;
    out << toml::value<double>(value);  // gidiş-dönüş hassasiyetini toml++ korur
    return out.str();
}

const std::array<TuningKey, 11>& tuning_keys() {
    static const std::array<TuningKey, 11> table = {{
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
        {"graph_batch_ops", [](const Tuning& t) { return std::to_string(t.graph_batch_ops); }},
    }};
    return table;
}

// Uygulama [lsp]'yi düzenlemez: kayıt, bölümü dosyada şu an nasılsa öyle (bozuk girdiler dahil) geri yazar.
// Bellekteki açılış kopyası yazılsaydı uygulama açıkken dosyadan kapatılan LSP ilk ayar kaydında geri
// açılırdı. Dosya yoksa, okunamıyorsa ya da bölüm yoksa boş: varsayılan geçerli olur.
std::string lsp_section_on_disk() {
    std::ifstream in(config_file(), std::ios::binary);
    if (!in)
        return {};
    const std::string text(std::istreambuf_iterator<char>(in), {});
    Error ignored{Error::Code::ConfigInvalid, "", {}};
    const auto table = parse_table(text, ignored);
    const toml::table* lsp = table ? table->get_as<toml::table>("lsp") : nullptr;
    if (lsp == nullptr)
        return {};  // tablo olmayan değer yüklemede zaten yok sayılır; yazılırsa her açılışta uyarı olur
    // Kökteki satır içi tablo (`lsp = { … }`) [sync]'ten sonra yazılınca `sync.lsp` olurdu: başlıklı yazılır.
    toml::table copy = *lsp;
    copy.is_inline(false);
    toml::table section;
    section.insert("lsp", std::move(copy));
    std::ostringstream out;
    out << "\n" << section << "\n";  // toml++ [lsp] ve [lsp.servers] başlıklarını kendisi yazar
    return out.str();
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
    // İzole ortam gerçek ~/notes'a yazmasın; kullanıcının açık değeri aşağıda yine kazanır.
    if (auto isolated = isolated_notes_dir())
        loaded.config.notes_dir = std::move(*isolated);

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
    out << "]\n";
    out << "icons = " << quote(config.icons) << "\n";
    out << "\n[sync]\n";
    out << "enabled = " << (config.sync.enabled ? "true" : "false") << "\n";
    out << "remote = " << quote(config.sync.remote) << "\n";
    out << "branch = " << quote(config.sync.branch) << "\n";
    out << "idle_seconds = " << config.sync.idle_seconds << "\n";
    out << "author_name = " << quote(config.sync.author_name) << "\n";
    out << "author_email = " << quote(config.sync.author_email) << "\n";
    out << lsp_section_on_disk();

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
