#include "tui/server_settings.hpp"

#include "core/json_text.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"

#include <algorithm>
#include <span>
#include <system_error>
#include <utility>

namespace noteapp::tui {

namespace fs = std::filesystem;
using core::Json;

namespace {

// Gömülü JSON'lar derinliğe izin verir: kullanıcı tablosu 16 düzeye, sarmalayan birkaç düzeye çıkar.
constexpr core::JsonLimits kBuiltinLimits{.max_depth = 32};

// Ayar ağacından bir mesaja giden parça.
struct Projection {
    bool enabled = false;
    std::string_view section;  // "" ağacın tamamı, değilse bu anahtarın değeri
    std::string_view wrap;     // "" parça doğrudan gider, değilse {wrap: parça}
};

// Bir sunucunun gömülü ayarı. Her anahtar sunucunun güncel kaynağından doğrulandı (2026-11-03); kaynaklar
// satır yorumlarında. Amaç: senkronla gelen bir not sunucuya kod çalıştıramasın, ağdan bir şey indirtemesin.
struct Builtin {
    std::string_view program;
    std::string_view settings;                // JSON nesnesi: workspace/configuration cevaplarının kaynağı
    std::string_view initialization_options;  // JSON nesnesi ya da boş
    Projection initialization;                // ayar ağacından initializationOptions'a (sabitin üstüne biner)
    Projection change;                        // didChangeConfiguration `settings`ı
    std::span<const std::string_view> environment;
    std::span<const std::string_view> notifications;
    std::string_view data_flag;  // veri dizini bayrağı; argv'ye `<bayrak> <lsp_dir>/<program>-data` eklenir
};

// rust-analyzer: crates/rust-analyzer/src/config.rs (cargo.buildScripts.enable, procMacro.enable,
// checkOnSave, cargo.noDeps); session.rs initializationOptions'ı istemci ayarı sayar;
// handlers/notification.rs ayarı "rust-analyzer" bölümüyle yeniden ister. Ortam: CARGO_NET_OFFLINE
// (doc.rust-lang.org/cargo/reference/ config.html#netoffline), RUSTUP_AUTO_INSTALL
// (rust-lang.github.io/rustup/environment-variables.html). checkOnSave nesne biçiminde (enable): 2023-01-09
// öncesi sürümler yalnız checkOnSave.enable'ı tanır, bool'u yok sayıp cargo check (build.rs) çalıştırırdı;
// sonrakiler onu takma ad olarak önce okur (commit 87d57f51bc, PR #13799; config.rs `checkOnSave |
// checkOnSave_enable`, get_field_json takma adı önce dener; config/patch_old_style.rs nesneyi bool'a
// çevirir).
constexpr std::string_view kRustAnalyzerEnv[] = {"CARGO_NET_OFFLINE=true", "RUSTUP_AUTO_INSTALL=0"};
// gopls: gopls/doc/settings.md (codelenses, vulncheck), internal/server/general.go (initializationOptions,
// "gopls" bölümü). Ortam: GOTOOLCHAIN=local (go.dev/doc/toolchain), GOPROXY=off ve -mod=readonly
// (go.dev/ref/mod), GOVCS=*:off ve CGO_ENABLED (pkg.go.dev/cmd/go). cgo kapalı: `go list -compiled` C
// derleyicisini nottaki #cgo bayraklarıyla çalıştırmasın.
constexpr std::string_view kGoplsEnv[] = {"GOTOOLCHAIN=local", "GOPROXY=off", "GOVCS=*:off",
                                          "GOFLAGS=-mod=readonly", "CGO_ENABLED=0"};
// yaml-language-server: src/requestTypes.ts. Bildirimden sonra http(s) şemaları istemciye vscode/content ile
// sorulur (schemaRequestHandler.ts); istemci -32601 döner.
constexpr std::string_view kYamlNotifications[] = {"yaml/registerContentRequest"};

constexpr Builtin kBuiltins[] = {
    {.program = "rust-analyzer",
     .settings = R"({"rust-analyzer":{"cargo":{"buildScripts":{"enable":false},"noDeps":true},)"
                 R"("procMacro":{"enable":false},"checkOnSave":{"enable":false}}})",
     .initialization_options = {},
     .initialization = {.enabled = true, .section = "rust-analyzer", .wrap = {}},
     .change = {.enabled = true, .section = {}, .wrap = {}},
     .environment = kRustAnalyzerEnv,
     .notifications = {},
     .data_flag = {}},
    {.program = "gopls",
     .settings = R"({"gopls":{"codelenses":{"generate":false,"regenerate_cgo":false,"run_govulncheck":false,)"
                 R"("tidy":false,"upgrade_dependency":false,"vendor":false},"vulncheck":"Off"}})",
     .initialization_options = {},
     .initialization = {.enabled = true, .section = "gopls", .wrap = {}},
     .change = {.enabled = true, .section = {}, .wrap = {}},
     .environment = kGoplsEnv,
     .notifications = {},
     .data_flag = {}},
    // jdtls: BaseInitHandler.java (initializationOptions.settings), Preferences.java anahtarları,
    // JDTLanguageServer.didChangeConfiguration; değerler vscode-java package.json. Varsayılan -data
    // geçici dizindedir (bin/jdtls.py): noteapp'e ait özel dizin verilir.
    {.program = "jdtls",
     .settings = R"({"java":{"import":{"maven":{"enabled":false,"offline":{"enabled":true}},)"
                 R"("gradle":{"enabled":false,"wrapper":{"enabled":false},"offline":{"enabled":true},)"
                 R"("annotationProcessing":{"enabled":false}}},)"
                 R"("maven":{"downloadSources":false},"eclipse":{"downloadSources":false},)"
                 R"("configuration":{"updateBuildConfiguration":"disabled"},)"
                 R"("project":{"referencedLibraries":[]}}})",
     .initialization_options = {},
     .initialization = {.enabled = true, .section = {}, .wrap = "settings"},
     .change = {.enabled = true, .section = {}, .wrap = {}},
     .environment = {},
     .notifications = {},
     .data_flag = "-data"},
    // typescript-language-server: docs/configuration.md (disableAutomaticTypingAcquisition). Yerel tsserver
    // eklentisi zaten yüklenmez: sunucu --allowLocalPluginLoads vermez (src/tsServer/spawner.ts), tsserver'da
    // varsayılan kapalı. Çalışma alanının node_modules/typescript'i yansıtılmaz (lsp_workspace.cpp).
    {.program = "typescript-language-server",
     .settings = {},
     .initialization_options = R"({"disableAutomaticTypingAcquisition":true})",
     .initialization = {},
     .change = {},
     .environment = {},
     .notifications = {},
     .data_flag = {}},
    // yaml-language-server: README ayarları; settingsHandlers.ts "yaml", "http" bölümlerini çeker.
    {.program = "yaml-language-server",
     .settings =
         R"({"yaml":{"schemaStore":{"enable":false},"kubernetesCRDStore":{"enable":false},"schemas":{}}})",
     .initialization_options = {},
     .initialization = {},
     .change = {.enabled = true, .section = {}, .wrap = {}},
     .environment = {},
     .notifications = kYamlNotifications,
     .data_flag = {}},
    // vscode-json-language-server: extensions/json-language-features/server/src/jsonServer.ts. Elle tutulan
    // protokol dışındakiler istemciye vscode/content ile sorulur, istemci -32601 döner. Bildirimdeki
    // json.validate.enable yoksa false sayılır: açık verilir.
    {.program = "vscode-json-language-server",
     .settings = R"({"json":{"validate":{"enable":true},"schemas":[]}})",
     .initialization_options = R"({"handledSchemaProtocols":["file"],"provideFormatter":false})",
     .initialization = {},
     .change = {.enabled = true, .section = {}, .wrap = {}},
     .environment = {},
     .notifications = {},
     .data_flag = {}},
    // taplo: crates/taplo-lsp/src/config.rs (schema.enabled, schema.catalogs, taplo.configFile.enabled);
    // handlers/configuration.rs bildirimde bölümün kendisini, çekmede "evenBetterToml" bölümünü okur.
    {.program = "taplo",
     .settings = R"({"evenBetterToml":{"schema":{"enabled":false,"catalogs":[]},)"
                 R"("taplo":{"configFile":{"enabled":false}}}})",
     .initialization_options = R"({"configurationSection":"evenBetterToml"})",
     .initialization = {},
     .change = {.enabled = true, .section = "evenBetterToml", .wrap = {}},
     .environment = {},
     .notifications = {},
     .data_flag = {}},
    // htmx-lsp: ayar okumaz, süreç başlatmaz, ağa çıkmaz (lsp/src/lib.rs, handle.rs). Tabloda: varsayılanın
    // incelendiğini belgeler.
    {.program = "htmx-lsp",
     .settings = {},
     .initialization_options = {},
     .initialization = {},
     .change = {},
     .environment = {},
     .notifications = {},
     .data_flag = {}},
};

const Builtin* builtin_for(std::string_view program) {
    const auto* found = std::ranges::find(kBuiltins, program, &Builtin::program);
    return found == std::ranges::end(kBuiltins) ? nullptr : found;
}

// Gömülü sabitler testle doğrulanır; bozuksa programlama hatasıdır.
Json builtin_json(std::string_view text) {
    if (text.empty())
        return Json{.kind = Json::Kind::Object};
    return core::JsonParser(text, kBuiltinLimits).parse().value();
}

// `over` nesneleri anahtar anahtar `base`e biner; nesne olmayan değer olduğu gibi değişir.
void merge_into(Json& base, const Json& over) {
    if (base.kind != Json::Kind::Object || over.kind != Json::Kind::Object) {
        base = over;
        return;
    }
    for (const auto& [key, value] : over.fields) {
        const auto found = base.fields.find(key);
        if (found == base.fields.end())
            base.fields.emplace(key, value);
        else
            merge_into(found->second, value);
    }
}

std::optional<Json> project(const Json& settings, const Projection& projection) {
    if (!projection.enabled)
        return std::nullopt;
    Json part = settings;
    if (!projection.section.empty()) {
        const auto found = settings.fields.find(std::string(projection.section));
        part = found == settings.fields.end() ? Json{.kind = Json::Kind::Object} : found->second;
    }
    if (projection.wrap.empty())
        return part;
    Json wrapped{.kind = Json::Kind::Object};
    wrapped.fields.emplace(std::string(projection.wrap), std::move(part));
    return wrapped;
}

}  // namespace

std::string server_program(std::string_view argv0) {
    const std::size_t slash = argv0.find_last_of("/\\");
    std::string_view name = slash == std::string_view::npos ? argv0 : argv0.substr(slash + 1);
    if (const std::size_t dot = name.rfind('.'); dot != std::string_view::npos && dot != 0)
        name = name.substr(0, dot);
    std::string out(name);
    std::ranges::transform(
        out, out.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return out;
}

bool has_hardening(std::string_view program) {
    return builtin_for(program) != nullptr;
}

std::optional<ServerLaunch> server_launch(std::vector<std::string> command, const Json* user,
                                          const fs::path& lsp_dir) {
    ServerLaunch out{.command = std::move(command)};
    const std::string program = server_program(out.command.front());
    const Builtin* builtin = builtin_for(program);
    if (builtin == nullptr) {
        // Tanınmayan sunucu: yalnız kullanıcının ayarı, o da bildirim ve çekme yoluyla.
        if (user != nullptr) {
            out.settings = *user;
            out.configuration_change = core::json_text(*user);
        }
        return out;
    }
    Json settings = builtin_json(builtin->settings);
    if (user != nullptr)
        merge_into(settings, *user);
    if (!builtin->initialization_options.empty() || builtin->initialization.enabled) {
        Json options = builtin_json(builtin->initialization_options);
        if (auto part = project(settings, builtin->initialization))
            merge_into(options, *part);
        out.initialization_options = core::json_text(options);
    }
    if (auto change = project(settings, builtin->change))
        out.configuration_change = core::json_text(*change);
    out.environment.assign(builtin->environment.begin(), builtin->environment.end());
    out.notifications.assign(builtin->notifications.begin(), builtin->notifications.end());
    if (!builtin->data_flag.empty() &&
        std::ranges::find(out.command, builtin->data_flag) == out.command.end()) {
        const fs::path data = lsp_dir / (program + "-data");
        if (const std::error_code ec = core::create_private_directories(data)) {
            core::log_warn("event=lsp_data_dir_failed program=" + program + " detail=" + ec.message());
            return std::nullopt;
        }
        const std::u8string text = data.u8string();
        out.command.emplace_back(builtin->data_flag);
        out.command.emplace_back(text.begin(), text.end());
    }
    out.settings = std::move(settings);
    return out;
}

}  // namespace noteapp::tui
