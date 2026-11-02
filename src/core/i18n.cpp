#include "core/i18n.hpp"

#include "core/log.hpp"
#include "core/resources.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <set>
#include <toml++/toml.hpp>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace noteapp::core {

namespace {

using Catalog = std::unordered_map<std::string, std::string>;

// TOML tabloları "bölüm.anahtar" olarak düzleştirilir; dize olmayan değerler yok sayılır.
void flatten(const toml::table& table, const std::string& prefix, Catalog& out) {
    for (const auto& [key, node] : table) {
        std::string name = prefix.empty() ? std::string(key.str()) : prefix + "." + std::string(key.str());
        if (const auto* sub = node.as_table())
            flatten(*sub, name, out);
        else if (const auto* value = node.as_string())
            out.emplace(std::move(name), value->get());
    }
}

Catalog parse_catalog(std::string_view text, std::string_view label) {
    Catalog catalog;
    toml::parse_result parsed = toml::parse(text);
    if (parsed)
        flatten(parsed.table(), "", catalog);
    else
        log_error("i18n: catalog '" + std::string(label) +
                  "' is unusable: " + std::string(parsed.error().description()));
    return catalog;
}

// Tek süreç = tek dil; tüm erişim tek mutex altında (ponytail: okuma başına kilit, katalog küçük).
struct State {
    std::mutex mutex;
    std::string language = "en";
    Catalog en;
    Catalog tr;
    bool loaded = false;
    std::set<std::string> warned;
};

State& state() {
    static State s;
    return s;
}

void load_embedded_locked(State& s) {
    if (s.loaded)
        return;
    s.en = parse_catalog(embedded_resource("i18n/en.toml").value_or(""), "en");
    s.tr = parse_catalog(embedded_resource("i18n/tr.toml").value_or(""), "tr");
    s.loaded = true;
}

std::string substitute(const std::string& text, const Args& args) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t open = text.find('{', i);
        if (open == std::string::npos)
            break;
        const std::size_t close = text.find('}', open);
        if (close == std::string::npos)
            break;
        const std::string_view name(text.data() + open + 1, close - open - 1);
        const auto arg =
            std::find_if(args.begin(), args.end(), [&](const auto& a) { return a.first == name; });
        out.append(text, i, open - i);
        if (arg != args.end())
            out += arg->second;
        else
            out.append(text, open, close - open + 1);  // bilinmeyen yer tutucu olduğu gibi kalır
        i = close + 1;
    }
    out.append(text, i, std::string::npos);
    return out;
}

std::string resolve_language(std::string_view setting) {
    if (setting == "auto")
        return detect_language();
    return setting == "tr" ? "tr" : "en";
}

}  // namespace

std::string detect_language() {
#ifdef _WIN32
    // Birincil dil kimliği alt 10 bit; Türkçe LANG_TURKISH (0x1F).
    return (GetUserDefaultUILanguage() & 0x3FF) == LANG_TURKISH ? "tr" : "en";
#else
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe) -- üründe setenv/putenv yok
        if (value != nullptr && *value != '\0')
            return std::string_view(value).starts_with("tr") ? "tr" : "en";
    }
    return "en";
#endif
}

void init_i18n(std::string_view language_setting) {
    State& s = state();
    const std::string language = resolve_language(language_setting);
    const std::scoped_lock lock(s.mutex);
    s.loaded = false;  // test katalogları (init_i18n_with_catalogs) varsa gömülülerle değiştirilir
    load_embedded_locked(s);
    s.language = language;
    s.warned.clear();
}

void init_i18n_with_catalogs(std::string_view language_setting, std::string_view en_toml,
                             std::string_view tr_toml) {
    State& s = state();
    const std::string language = resolve_language(language_setting);
    const std::scoped_lock lock(s.mutex);
    s.en = parse_catalog(en_toml, "en");
    s.tr = parse_catalog(tr_toml, "tr");
    s.loaded = true;
    s.language = language;
    s.warned.clear();
}

std::string current_language() {
    State& s = state();
    const std::scoped_lock lock(s.mutex);
    return s.language;
}

std::string tr(std::string_view key, const Args& args) {
    State& s = state();
    std::string warning;
    std::string result;
    {
        const std::scoped_lock lock(s.mutex);
        load_embedded_locked(s);
        const std::string k(key);
        const Catalog& selected = s.language == "tr" ? s.tr : s.en;
        if (const auto it = selected.find(k); it != selected.end()) {
            result = substitute(it->second, args);
        } else {
            if (s.warned.insert(k).second)
                warning = "i18n: missing key '" + k + "' for language '" + s.language + "'";
            const auto en = s.en.find(k);
            result = en != s.en.end() ? substitute(en->second, args) : k;
        }
    }
    // Kilidin dışında: log kendi mutex'ini alır.
    if (!warning.empty())
        log_warn(warning);
    return result;
}

std::string format_error(const Error& error) {
    Args args = error.args;
    for (auto& [name, value] : args) {
        if (name != "reason")
            continue;
        const std::string reason_key = "reason." + value;
        // Çeviri anahtarın kendisine düştüyse reason bir katalog değeri değildir: olduğu gibi bırak.
        std::string translated = tr(reason_key);
        if (translated != reason_key)
            value = std::move(translated);
    }
    return tr(error.message_key, args);
}

}  // namespace noteapp::core
