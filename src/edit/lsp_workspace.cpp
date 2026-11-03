#include "edit/lsp_workspace.hpp"

#include "core/atomic_file.hpp"
#include "core/file_stat.hpp"
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace noteapp::tui {

namespace fs = std::filesystem;

namespace {

std::string to_utf8(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

// Yansıtılan dosya zaten aynıysa yazılmaz: açılışta binlerce dosyayı yeniden yazmak gereksiz disk yüküdür.
bool has_content(const fs::path& file, std::string_view content) {
    std::error_code ec;
    if (fs::file_size(file, ec) != content.size() || ec)
        return false;
    std::ifstream in(file, std::ios::binary);
    const std::string existing(std::istreambuf_iterator<char>(in), {});
    return in.good() || in.eof() ? existing == content : false;
}

}  // namespace

fs::path lsp_workspace_dir() {
    return core::state_dir() / "lsp" / "workspace";
}

namespace {

struct LanguageName {
    std::string_view name;  // küçük harf uzantı (noktasız) ya da tam dosya adı
    std::string_view id;    // LSP languageId
};

// Notun dil kimliğinin tek kaynağı. Sözdizimi vurgusundan bağımsızdır: dilbilgisi olmayan tip de sunucuya
// gidebilir. env ve properties bilerek yoktur (sır taşırlar); listede olmayan her tip hiçbir sunucuya gitmez.
constexpr LanguageName kFileNames[] = {
    {"Dockerfile", "dockerfile"}, {"Containerfile", "dockerfile"}, {"Makefile", "makefile"},
    {"makefile", "makefile"},     {"GNUmakefile", "makefile"},     {"CMakeLists.txt", "cmake"},
};
constexpr LanguageName kExtensions[] = {
    {"c", "c"},
    {"h", "c"},
    {"cpp", "cpp"},
    {"cc", "cpp"},
    {"cxx", "cpp"},
    {"c++", "cpp"},
    {"hpp", "cpp"},
    {"hh", "cpp"},
    {"hxx", "cpp"},
    {"py", "python"},
    {"pyi", "python"},
    {"sh", "shellscript"},
    {"bash", "shellscript"},
    {"md", "markdown"},
    {"markdown", "markdown"},
    {"lua", "lua"},
    {"json", "json"},
    {"jsonc", "jsonc"},
    {"yaml", "yaml"},
    {"yml", "yaml"},
    {"toml", "toml"},
    {"js", "javascript"},
    {"mjs", "javascript"},
    {"cjs", "javascript"},
    {"jsx", "javascriptreact"},
    {"ts", "typescript"},
    {"mts", "typescript"},
    {"cts", "typescript"},
    {"tsx", "typescriptreact"},
    {"go", "go"},
    {"rs", "rust"},
    {"java", "java"},
    {"cs", "csharp"},
    {"html", "html"},  // Thymeleaf ve htmx şablonları da
    {"htm", "html"},
    {"css", "css"},
    {"scss", "scss"},
    {"less", "less"},
    {"sql", "sql"},
    {"dockerfile", "dockerfile"},
    {"makefile", "makefile"},
    {"mk", "makefile"},
    {"cmake", "cmake"},
    {"rb", "ruby"},
    {"php", "php"},
    {"kt", "kotlin"},
    {"kts", "kotlin"},
    {"swift", "swift"},
    {"zig", "zig"},
    {"xml", "xml"},
    {"ini", "ini"},
};

}  // namespace

std::string_view lsp_language_for(const core::NotePath& path) {
    const std::string name = to_utf8(path.value().filename());
    if (const auto* found = std::ranges::find(kFileNames, name, &LanguageName::name);
        found != std::ranges::end(kFileNames))
        return found->id;
    std::string extension = to_utf8(path.value().extension());
    if (extension.empty())
        return {};
    extension.erase(0, 1);
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto* found = std::ranges::find(kExtensions, extension, &LanguageName::name);
    return found == std::ranges::end(kExtensions) ? std::string_view{} : found->id;
}

namespace {

// Senkronla gelen bir not, sunucunun ayarını ya da çalıştırdığı kodu sağlayamasın: bu adlardaki dosyalar ve
// bu klasörlerin altı hiç yansıtılmaz (T59e). Adlar küçük harf ASCII'dir; not adı folded_name ile
// karşılaştırılır.
constexpr std::string_view kServerConfigNames[] = {
    "rust-analyzer.toml",   // rust-analyzer: check.overrideCommand ve buildScripts istemci ayarını ezer
    "rust-toolchain.toml",  // rustup: araç zinciri indirir ya da yol verilen araç zincirini çalıştırır
    "rust-project.json",    // rust-analyzer: proje modeli (proc-macro kitaplık yolları)
    "taplo.toml",           // taplo: şema URL'leri ve katalog
};
constexpr std::string_view kServerCodeDirectories[] = {
    "node_modules",  // typescript-language-server çalışma alanının typescript/lib/tsserver.js'ini çalıştırır
};

constexpr unsigned kFullwidthFirst = 0xFF01;   // tam genişlik '!'
constexpr unsigned kFullwidthLast = 0xFF5E;    // tam genişlik '~'
constexpr unsigned kFullwidthOffset = 0xFEE0;  // tam genişlik - ASCII

// Büyük/küçük harf duyarsız dosya sistemlerinin (APFS, NTFS) bir adı ASCII'ye denk sayması: ASCII küçük
// harfe, Unicode CaseFolding.txt'te ASCII'ye inen dört harf (ſ U+017F -> s, ı U+0131 ve İ U+0130 -> i, K
// U+212A -> k) ve tam genişlik ASCII (U+FF01-U+FF5E, uyumluluk eşdeğeri) karşılığına iner. Kalan baytlar
// olduğu gibi kalır: "kılavuz" "kilavuz" olur, engellenen bir adla karışmaz. Tablo eksik kalırsa dosya
// sisteminin kendi karşılaştırması (resolves_to_denied) yakalar.
std::string folded_name(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (std::size_t i = 0; i < name.size();) {
        const std::string_view rest = name.substr(i);
        if (rest.starts_with("\xC5\xBF")) {
            out += 's';
            i += 2;
        } else if (rest.starts_with("\xC4\xB1") || rest.starts_with("\xC4\xB0")) {
            out += 'i';
            i += 2;
        } else if (rest.starts_with("\xE2\x84\xAA")) {
            out += 'k';
            i += 3;
        } else if (rest.size() >= 3 && static_cast<unsigned char>(rest[0]) == 0xEF) {
            const unsigned code = ((static_cast<unsigned char>(rest[0]) & 0x0FU) << 12U) |
                                  ((static_cast<unsigned char>(rest[1]) & 0x3FU) << 6U) |
                                  (static_cast<unsigned char>(rest[2]) & 0x3FU);
            if (code >= kFullwidthFirst && code <= kFullwidthLast) {
                out += static_cast<char>(std::tolower(static_cast<int>(code - kFullwidthOffset)));
                i += 3;
            } else {
                out += rest[0];
                ++i;
            }
        } else {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(rest[0])));
            ++i;
        }
    }
    return out;
}

bool configures_a_server(const core::NotePath& note) {
    const auto matches = [](const fs::path& part, std::span<const std::string_view> list) {
        const std::string name = folded_name(to_utf8(part));
        return std::ranges::find(list, name) != list.end();
    };
    if (matches(note.value().filename(), kServerConfigNames))
        return true;
    return std::ranges::any_of(note.value().parent_path(),
                               [&](const fs::path& part) { return matches(part, kServerCodeDirectories); });
}

bool is_slash(char c) {
    return c == '/' || c == '\\';
}

bool equals_ascii_ignoring_case(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

constexpr std::string_view kFileScheme = "file:";
constexpr std::size_t kUncSlashes = 4;  // file://// ve fazlası: yol "//host/..." olur
constexpr std::size_t kAuthoritySlashes = 2;

// `rest` "file:"tan sonrası. İki eğik çizgiden sonra boş olmayan, localhost olmayan host ya da dört ve daha
// çok eğik çizgi (UNC). Ters eğik çizgi de sayılır: Windows ikisini aynı okur.
bool is_remote_file_uri(std::string_view rest) {
    std::size_t slashes = 0;
    while (slashes < rest.size() && is_slash(rest[slashes]))
        ++slashes;
    if (slashes >= kUncSlashes)
        return true;
    if (slashes != kAuthoritySlashes)
        return false;
    const std::string_view after = rest.substr(slashes);
    const std::string_view host = after.substr(0, std::min(after.find_first_of("/\\"), after.size()));
    return !host.empty() && !equals_ascii_ignoring_case(host, "localhost");
}

constexpr unsigned kFirstNonAscii = 0x80;
constexpr std::size_t kUnicodeEscapeDigits = 4;  // \uXXXX
constexpr std::size_t kPercentDigits = 2;        // %XX
constexpr int kHexBase = 16;

// Host adı karakteri: ASCII harf, rakam, '-', '.', '_' ve (uluslararası adlar için) her ASCII dışı bayt.
bool is_host_char(char c) {
    const auto byte = static_cast<unsigned char>(c);
    return std::isalnum(byte) != 0 || c == '-' || c == '.' || c == '_' || byte >= kFirstNonAscii;
}

// `at`ta "\\host\paylaşım" ya da "//host/paylaşım" başlıyor mu (iki ayırıcı karışık olabilir: Windows ikisini
// aynı okur). Önünde ':' olan (https://, file://) URL'dir, önünde ayırıcı olan daha uzun bir ayırıcı
// dizisinin parçasıdır. Host'tan sonra tek ayırıcı ve ayırıcı olmayan bir karakter gelmeli: "n//2",
// "//TODO:",
// "\\d+" ve JSON'da kaçışlı "C:\\\\a\\\\b" (ham metinde host'tan sonra iki ayırıcı) yol değildir.
bool unc_path_at(std::string_view text, std::size_t at) {
    if (at + 1 >= text.size() || !is_slash(text[at]) || !is_slash(text[at + 1]))
        return false;
    if (at > 0 && (text[at - 1] == ':' || is_slash(text[at - 1])))
        return false;
    std::size_t end = at + 2;
    while (end < text.size() && is_host_char(text[end]))
        ++end;
    if (end == at + 2 || end + 1 >= text.size() || !is_slash(text[end]))
        return false;
    const char share = text[end + 1];
    return !is_slash(share) && std::isspace(static_cast<unsigned char>(share)) == 0 && share != '"' &&
           share != '\'';
}

std::optional<unsigned> hex_value(std::string_view digits) {
    unsigned value = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value, kHexBase);
    if (error != std::errc{} || end != digits.data() + digits.size())
        return std::nullopt;
    return value;
}

// JSON kaçışları (\\ \/ ve ASCII'ye inen \uXXXX) ve yüzde kodlaması çözülmüş kopya: "\/\/host",
// "\u002f\u002fhost", "%5C%5Chost" düz metindeki biçimine iner. Başka kaçışlar olduğu gibi kalır.
std::string decoded_references(std::string_view text) {
    std::string json;
    json.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            json += text[i];
            continue;
        }
        const char next = text[i + 1];
        if (next == '\\' || next == '/') {
            json += next;
            ++i;
        } else if (next == 'u' && i + 2 + kUnicodeEscapeDigits <= text.size()) {
            const auto code = hex_value(text.substr(i + 2, kUnicodeEscapeDigits));
            if (code && *code < kFirstNonAscii) {
                json += static_cast<char>(*code);
                i += 1 + kUnicodeEscapeDigits;
            } else {
                json += text[i];
            }
        } else {
            json += text[i];
        }
    }
    std::string out;
    out.reserve(json.size());
    for (std::size_t i = 0; i < json.size(); ++i) {
        if (json[i] == '%' && i + kPercentDigits < json.size()) {
            if (const auto byte = hex_value(std::string_view(json).substr(i + 1, kPercentDigits))) {
                out += static_cast<char>(*byte);
                i += kPercentDigits;
                continue;
            }
        }
        out += json[i];
    }
    return out;
}

// `file:` (sözcük sonu değil) ve ardından uzak sayılan biçim.
bool has_remote_file_uri(std::string_view text) {
    // 2 MiB'lık metinde bayt bayt karşılaştırma tuş gecikmesini aşardı: ':' memchr ile aranır, önündeki dört
    // harf "file" mi diye bakılır.
    constexpr std::size_t kSchemeLetters = kFileScheme.size() - 1;
    for (std::size_t colon = text.find(':'); colon != std::string_view::npos;
         colon = text.find(':', colon + 1)) {
        if (colon < kSchemeLetters)
            continue;
        const std::size_t at = colon - kSchemeLetters;
        // "profile:" gibi bir sözcüğün sonu değil: önünde harf ya da rakam olmamalı.
        if (!equals_ascii_ignoring_case(text.substr(at, kFileScheme.size()), kFileScheme) ||
            (at > 0 && std::isalnum(static_cast<unsigned char>(text[at - 1])) != 0))
            continue;
        if (is_remote_file_uri(text.substr(colon + 1)))
            return true;
    }
    return false;
}

bool has_unc_path(std::string_view text) {
    for (std::size_t at = text.find_first_of("/\\"); at != std::string_view::npos;
         at = text.find_first_of("/\\", at + 1))
        if (unc_path_at(text, at))
            return true;
    return false;
}

bool has_network_reference(std::string_view text) {
    return has_remote_file_uri(text) || has_unc_path(text);
}

bool is_link(const fs::path& path) {
    const auto stat = core::stat_file(path);
    return stat && stat->symlink;
}

bool is_linked_root(const fs::path& root) {
    return is_link(root) || is_link(root.parent_path());
}

}  // namespace

std::optional<fs::path> mirror_file(const fs::path& root, const core::NotePath& note) {
    if (is_linked_root(root))
        return std::nullopt;
    std::error_code ec;
    fs::path file = fs::weakly_canonical(root, ec);
    if (ec)
        return std::nullopt;
    // Her bileşen bağsız olmalı. weakly_canonical/symlink_status MinGW'de junction'ı görmeyebilir; stat_file
    // (scan_files'ın da kullandığı) Windows'ta ad vekili reparse point'i bağ sayar.
    for (const fs::path& part : note.value()) {
        file /= part;
        if (is_link(file))
            return std::nullopt;
    }
    return file;
}

bool is_server_owned_path(const core::NotePath& note) {
    return configures_a_server(note);
}

bool references_network_path(std::string_view text) {
    if (has_network_reference(text))
        return true;
    // Kaçış yoksa çözülmüş kopya aynıdır: 2 MiB'lık metni boşuna kopyalamayalım.
    if (text.find_first_of("\\%") == std::string_view::npos)
        return false;
    return has_network_reference(decoded_references(text));
}

WorkspaceMirror::WorkspaceMirror(fs::path root, std::set<std::string> languages, SameEntry same_entry,
                                 bool block_network_paths, std::uintmax_t max_note_bytes)
    : root_(std::move(root)), languages_(std::move(languages)), same_entry_(std::move(same_entry)),
      block_network_paths_(block_network_paths), max_note_bytes_(max_note_bytes) {}

// Kökün altındaki her klasör node_modules'la, dosya engellenen her adla dosya sisteminin gözünde aynı mı.
bool WorkspaceMirror::resolves_to_denied(const fs::path& root, const fs::path& file) const {
    const auto same = [this](const fs::path& entry, std::string_view denied) {
        const fs::path twin = entry.parent_path() / fs::path(std::u8string(denied.begin(), denied.end()));
        if (same_entry_)
            return same_entry_(entry, twin);
        std::error_code ec;
        return fs::equivalent(entry, twin, ec);
    };
    if (std::ranges::any_of(kServerConfigNames, [&](std::string_view name) { return same(file, name); }))
        return true;
    for (fs::path dir = file.parent_path(); dir != root && dir.native().size() > root.native().size();
         dir = dir.parent_path())
        if (std::ranges::any_of(kServerCodeDirectories,
                                [&](std::string_view name) { return same(dir, name); }))
            return true;
    return false;
}

namespace {

// Atlanan yazmanın izi kalmasın: dosya ve boşalan klasörler köke dek silinir.
void remove_up_to(const fs::path& root, const fs::path& file) {
    std::error_code ec;
    fs::remove(file, ec);
    for (fs::path dir = file.parent_path(); dir != root && dir.native().size() > root.native().size();
         dir = dir.parent_path())
        if (!fs::remove(dir, ec))
            break;
}

}  // namespace

bool WorkspaceMirror::sync(const core::NoteStore& store, const std::stop_token& stop) {
    if (refused_)
        return false;
    // Kök ya da lsp/ bağsa gösterdiği yer bizim değildir: 0700'e çekilir, oradaki dosyalar "eski" sayılıp
    // silinirdi. Oturum boyunca yansıtılmaz.
    if (is_linked_root(root_)) {
        refused_ = true;
        core::log_warn("event=lsp_mirror_refused reason=workspace_is_link");
        return false;
    }
    if (const std::error_code ec = core::create_private_directories(root_)) {
        core::log_warn("event=lsp_mirror_failed detail=" + ec.message());
        return false;
    }
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(root_, ec);
    if (ec)
        return false;
    std::set<std::string> expected;
    for (const core::NoteInfo& info : store.list()) {
        if (stop.stop_requested())
            return false;  // yarım eşitlemede silme yapılmaz: listede olmayan dosya henüz görülmemiş olabilir
        if (info.size > max_note_bytes_ || !languages_.contains(std::string(lsp_language_for(info.path))) ||
            configures_a_server(info.path))
            continue;
        const auto file = mirror_file(root, info.path);
        if (!file) {
            core::log_warn("event=lsp_mirror_outside_workspace");
            continue;
        }
        const std::string key = info.path.string();
        const auto written = written_.find(key);
        if (written != written_.end() && written->second == info.revision && fs::exists(*file, ec)) {
            expected.insert(key);
            continue;
        }
        const auto content = store.read(info.path);
        // Ağ yolu taşıyan not sunucuya SMB bağlantısı açtırabilir (yalnız Windows): diske hiç yazılmaz.
        if (!content || content->text.size() > max_note_bytes_ ||
            (block_network_paths_ && references_network_path(content->text)))
            continue;
        if (!has_content(*file, content->text)) {
            if (const std::error_code dir_error = core::create_private_directories(file->parent_path())) {
                core::log_warn("event=lsp_mirror_failed detail=" + dir_error.message());
                continue;
            }
            if (const auto saved = core::write_file_atomically(*file, content->text, true); !saved) {
                core::log_warn("event=lsp_mirror_failed detail=" + core::format_error(saved.error()));
                continue;
            }
        }
        // Katlama tablosunun bilmediği bir eşdeğerliği dosya sistemi bilir (NTFS büyük harf tablosu, APFS):
        // yazılan engellenen bir adın kendisi çıktıysa silinir. Pencere: yazma ile silme arası.
        if (resolves_to_denied(root, *file)) {
            remove_up_to(root, *file);
            core::log_warn("event=lsp_mirror_denied_by_filesystem");
            continue;
        }
        written_.insert_or_assign(key, content->revision);
        expected.insert(key);
    }
    remove_stale(root, expected);
    return true;
}

// core::scan_files bağlara ve '.' ile başlayan adlara inmez, onları vermez (Windows'ta junction dahil): bağın
// gösterdiği yerden hiçbir şey silinmez, bağın kendisi de yerinde kalır. Not adı '.' ile başlayamaz; böyle
// adlar sunucunun önbelleğidir (clangd `.cache/`), her eşitlemede silinmesi yeniden indekslemeye yol açardı.
void WorkspaceMirror::remove_stale(const fs::path& root, const std::set<std::string>& expected) {
    std::error_code ec;
    for (const core::ScannedFile& file : core::scan_files(root)) {
        if (expected.contains(file.relative))
            continue;
        written_.erase(file.relative);
        const fs::path path = root / fs::path(std::u8string(file.relative.begin(), file.relative.end()));
        fs::remove(path, ec);
        // Boşalan klasörler köke dek gider; dolu olanda remove hata verip yerinde bırakır.
        for (fs::path dir = path.parent_path(); dir != root && dir.native().size() > root.native().size();
             dir = dir.parent_path())
            if (!fs::remove(dir, ec))
                break;
    }
}

LspWorkspace::LspWorkspace(WorkspaceMirror mirror, core::NoteStore store, std::chrono::milliseconds idle,
                           std::function<void()> on_ready)
    : mirror_(std::move(mirror)), idle_(idle), on_ready_(std::move(on_ready)), store_(std::move(store)),
      worker_([this](const std::stop_token& stop) { run(stop); }) {}

LspWorkspace::~LspWorkspace() = default;

void LspWorkspace::set_store(core::NoteStore store) {
    {
        const std::scoped_lock lock(mutex_);
        store_ = std::move(store);
    }
    request();
}

void LspWorkspace::request() {
    {
        const std::scoped_lock lock(mutex_);
        pending_ = true;
        requested_at_ = std::chrono::steady_clock::now();
    }
    wake_.notify_all();
}

// İstisna jthread'den kaçarsa std::terminate olurdu: loglanır, sonraki istek yeniden dener.
void LspWorkspace::run(const std::stop_token& stop) {
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        wake_.wait(lock, stop, [this] { return pending_; });
        if (stop.stop_requested())
            return;
        const auto ready_at = requested_at_ + idle_;
        if (std::chrono::steady_clock::now() < ready_at) {
            wake_.wait_until(lock, stop, ready_at, [] { return false; });
            continue;
        }
        pending_ = false;
        const core::NoteStore store = store_;
        lock.unlock();
        try {
            if (mirror_.sync(store, stop) && !ready_.exchange(true) && on_ready_)
                on_ready_();
        } catch (const std::exception& error) {
            core::log_warn(std::string("event=lsp_mirror_failed detail=") + error.what());
        }
        lock.lock();
    }
}

}  // namespace noteapp::tui
