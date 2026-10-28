#include "core/error.hpp"
#include "core/i18n.hpp"
#include "core/resources.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <doctest/doctest.h>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <toml++/toml.hpp>
#include <vector>

using namespace noteapp::core;
using namespace noteapp::test;

namespace {

// Eksik anahtar uyarıları log'a yazar; testler gerçek kullanıcı dizinini kirletmesin.
struct IsolatedHome {
    TempDir dir{kTurkishUncreated};
    ScopedEnv env{"NOTEAPP_HOME", dir.path().u8string()};
};

void flatten(const toml::table& table, const std::string& prefix, std::set<std::string>& out) {
    for (const auto& [key, node] : table) {
        const std::string name =
            prefix.empty() ? std::string(key.str()) : prefix + "." + std::string(key.str());
        if (const auto* sub = node.as_table())
            flatten(*sub, name, out);
        else
            out.insert(name);
    }
}

std::set<std::string> catalog_keys(std::string_view resource) {
    const auto text = embedded_resource(resource);
    REQUIRE(text.has_value());
    toml::parse_result parsed = toml::parse(*text);
    REQUIRE(parsed);
    std::set<std::string> keys;
    flatten(parsed.table(), "", keys);
    return keys;
}

constexpr std::string_view kEn = "only_en = \"English only\"\nboth = \"EN {x}\"\n[sec]\nkey = \"deep\"\n";
constexpr std::string_view kTr = "both = \"TR {x}\"\n";

}  // namespace

TEST_CASE("catalog key parity: en and tr define exactly the same keys") {
    const auto en = catalog_keys("i18n/en.toml");
    const auto tr = catalog_keys("i18n/tr.toml");
    CHECK_FALSE(en.empty());
    std::vector<std::string> only_en;
    std::vector<std::string> only_tr;
    std::set_difference(en.begin(), en.end(), tr.begin(), tr.end(), std::back_inserter(only_en));
    std::set_difference(tr.begin(), tr.end(), en.begin(), en.end(), std::back_inserter(only_tr));
    for (const auto& k : only_en)
        MESSAGE("missing in tr: " << k);
    for (const auto& k : only_tr)
        MESSAGE("missing in en: " << k);
    CHECK(only_en.empty());
    CHECK(only_tr.empty());
}

TEST_CASE("catalog placeholders match between en and tr") {
    const auto placeholders = [](const std::string& s) {
        std::set<std::string> out;
        for (std::size_t i = s.find('{'); i != std::string::npos; i = s.find('{', i + 1))
            out.insert(s.substr(i, s.find('}', i) - i + 1));
        return out;
    };
    IsolatedHome home;
    for (const auto& key : catalog_keys("i18n/en.toml")) {
        init_i18n("en");
        const std::string en = tr(key);
        init_i18n("tr");
        const std::string tr_text = tr(key);
        CHECK_MESSAGE(placeholders(en) == placeholders(tr_text), key);
    }
}

TEST_CASE("every Error key and reason produced by core exists in the catalogs") {
    const auto en = catalog_keys("i18n/en.toml");
    for (const char* key : {"error.invalid_path",
                            "error.io",
                            "error.not_found",
                            "error.already_exists",
                            "error.modified_externally",
                            "error.not_empty",
                            "error.config_invalid",
                            "error.editor_not_found",
                            "error.editor_failed",
                            "error.process_not_found",
                            "error.process_spawn_failed",
                            "error.process_unsafe_argument",
                            "error.process_empty_command",
                            "config.unknown_key",
                            "config.invalid_value",
                            "config.syntax_error",
                            "reason.empty",
                            "reason.invalid_utf8",
                            "reason.unc",
                            "reason.backslash",
                            "reason.absolute",
                            "reason.drive_letter",
                            "reason.empty_component",
                            "reason.dot",
                            "reason.dot_dot",
                            "reason.hidden",
                            "reason.control_char",
                            "reason.invalid_char",
                            "reason.reserved_name",
                            "reason.trailing_space",
                            "reason.trailing_dot",
                            "reason.too_long",
                            "reason.outside_root",
                            "reason.invalid_type",
                            "reason.invalid_name"})
        CHECK_MESSAGE(en.count(key) == 1, key);
}

TEST_CASE("tr substitutes {name} placeholders and leaves unknown ones intact") {
    IsolatedHome home;
    init_i18n_with_catalogs("en", "a = \"x {p} y {p} {q}\"\n", "");
    CHECK(tr("a", {{"p", "1"}}) == "x 1 y 1 {q}");
    CHECK(tr("a", {{"p", "ı{q}"}, {"q", "Q"}}) == "x ı{q} y ı{q} Q");  // değer içindeki {q} yeniden işlenmez
}

TEST_CASE("fallback chain: selected language, then en, then the key itself") {
    IsolatedHome home;
    init_i18n_with_catalogs("tr", kEn, kTr);
    CHECK(current_language() == "tr");
    CHECK(tr("both", {{"x", "1"}}) == "TR 1");
    CHECK(tr("only_en") == "English only");
    CHECK(tr("sec.key") == "deep");  // tablolar "section.key"e düzleşir
    CHECK(tr("no.such.key") == "no.such.key");
    CHECK(tr("no.such.key") == "no.such.key");  // ikinci çağrı da çökmez (uyarı yalnız bir kez)
    init_i18n_with_catalogs("en", kEn, kTr);
    CHECK(tr("both", {{"x", "1"}}) == "EN 1");
}

TEST_CASE("init_i18n resolves explicit languages and falls back to en for junk") {
    IsolatedHome home;
    init_i18n("tr");
    CHECK(current_language() == "tr");
    init_i18n("en");
    CHECK(current_language() == "en");
    init_i18n("auto");
    CHECK((current_language() == "tr" || current_language() == "en"));
    init_i18n("klingon");
    CHECK(current_language() == "en");
}

TEST_CASE("real catalogs: Turkish uses diacritics") {
    IsolatedHome home;
    init_i18n("tr");
    CHECK(tr("error.not_found", {{"path", "a.md"}}).find("ulunamadı") != std::string::npos);
}

TEST_CASE("format_error translates the reason argument when a reason.<value> key exists") {
    IsolatedHome home;
    init_i18n("en");
    const Error e{Error::Code::InvalidPath, "error.invalid_path", {{"reason", "dot_dot"}, {"path", "../x"}}};
    const std::string text = format_error(e);
    CHECK(text.find("../x") != std::string::npos);
    CHECK(text.find("dot_dot") == std::string::npos);
    CHECK(text == tr("error.invalid_path", {{"reason", tr("reason.dot_dot")}, {"path", "../x"}}));

    // Katalogda karşılığı olmayan reason olduğu gibi kalır.
    const Error unknown{Error::Code::InvalidPath, "error.invalid_path", {{"reason", "zzz"}, {"path", "p"}}};
    CHECK(format_error(unknown).find("zzz") != std::string::npos);
}

TEST_CASE("detect_language follows LC_ALL, LC_MESSAGES, LANG on POSIX") {
#ifdef _WIN32
    const std::string detected = detect_language();
    CHECK((detected == "tr" || detected == "en"));
#else
    const ScopedEnv all("LC_ALL", std::nullopt);
    const ScopedEnv msg("LC_MESSAGES", std::nullopt);
    const ScopedEnv lang("LANG", u8"tr_TR.UTF-8");
    CHECK(detect_language() == "tr");
    const ScopedEnv msg_en("LC_MESSAGES", u8"en_US.UTF-8");
    CHECK(detect_language() == "en");  // LC_MESSAGES, LANG'dan önceliklidir
    const ScopedEnv all_tr("LC_ALL", u8"tr");
    CHECK(detect_language() == "tr");  // LC_ALL en önceliklidir
#endif
}
