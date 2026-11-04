#include "app/document.hpp"

#include "core/i18n.hpp"
#include "core/text.hpp"

#include <cstdlib>
#include <string>
#include <utility>

namespace noteapp::app {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::string_view kReplacement = "\xEF\xBF\xBD";  // U+FFFD
constexpr std::string_view kCrlf = "\r\n";
constexpr std::uintmax_t kBytesPerMebibyte = 1024 * 1024;

std::size_t sequence_length(unsigned char lead) {
    if (lead >= 0xF0)
        return 4;
    if (lead >= 0xE0)
        return 3;
    return lead >= 0xC0 ? 2 : 1;
}

// Her '\n' CRLF'nin parçası ve en az bir satır sonu var: LF'ye indirip geri açmak aynı baytları verir (tek
// başına CR iki yönde de olduğu gibi kalır). Karışık dosyada bu garanti yok.
bool uniform_crlf(std::string_view text) {
    bool any = false;
    for (std::size_t at = text.find('\n'); at != std::string_view::npos; at = text.find('\n', at + 1)) {
        if (at == 0 || text[at - 1] != '\r')
            return false;
        any = true;
    }
    return any;
}

}  // namespace

ReadOnly classify(std::string_view raw, std::uintmax_t max_bytes) {
    if (raw.size() > max_bytes)
        return ReadOnly::TooLarge;
    if (core::looks_binary(raw))
        return ReadOnly::Binary;
    if (raw.find('\0') != std::string_view::npos)
        return ReadOnly::EmbeddedNul;
    return core::is_valid_utf8(raw) ? ReadOnly::No : ReadOnly::InvalidUtf8;
}

std::string read_only_reason(ReadOnly reason, std::uintmax_t max_bytes) {
    switch (reason) {
    case ReadOnly::Binary:
        return core::tr("document.readonly_binary");
    case ReadOnly::InvalidUtf8:
        return core::tr("document.readonly_utf8");
    case ReadOnly::EmbeddedNul:
        return core::tr("document.readonly_nul");
    case ReadOnly::TooLarge:
        return core::tr("document.readonly_large",
                        {{"limit", std::to_string(max_bytes / kBytesPerMebibyte)}});
    case ReadOnly::No:
        break;
    }
    return {};
}

std::string sanitized_utf8(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t at = 0; at < raw.size();) {
        const auto lead = static_cast<unsigned char>(raw[at]);
        const std::size_t length = std::min(sequence_length(lead), raw.size() - at);
        const std::string_view sequence = raw.substr(at, length);
        if (lead != 0 && core::is_valid_utf8(sequence)) {
            out += sequence;
            at += length;
        } else {
            out += kReplacement;
            ++at;
        }
    }
    return out;
}

DiskChange disk_change(const std::optional<core::Revision>& disk, const core::Revision& mine, bool dirty) {
    if (!disk)
        return dirty ? DiskChange::Deleted : DiskChange::Close;
    if (*disk == mine)
        return DiskChange::None;
    return dirty ? DiskChange::Changed : DiskChange::Reload;
}

std::string new_note_folder(const FolderContext& context) {
    if (context.selected_folder != nullptr)
        return *context.selected_folder;
    if (!context.scope_folder.empty())
        return std::string(context.scope_folder);
    if (context.note != nullptr) {
        if (std::string folder = core::split_note_path(context.note->string()).folder; !folder.empty())
            return folder;
    }
    return std::string(context.default_folder);
}

std::expected<DraftName, core::Error> parse_draft_name(std::string_view input,
                                                       std::string_view default_folder,
                                                       std::chrono::system_clock::time_point now,
                                                       std::size_t slug_max_length) {
    DraftName name;
    name.folder = default_folder;
    std::string_view rest = input;
    if (const std::size_t slash = rest.rfind('/'); slash != std::string_view::npos) {
        name.folder = rest.substr(0, slash);
        rest.remove_prefix(slash + 1);
    }
    // Gizli ad NotePath'ten geçemez: slug ona sessizce başka bir ad verirdi (`.env` -> `env.md`).
    if (rest.starts_with('.'))
        if (auto hidden = core::NotePath::parse(rest); !hidden)
            return std::unexpected(hidden.error());
    const core::TypedName typed = core::split_typed_name(rest);
    name.type = typed.extension;
    rest = typed.stem;
    name.timestamp_name = rest.empty();
    if (!rest.empty())
        name.stem = core::slugify_stem(rest, now, slug_max_length);
    // create() ancak kayıtta denerdi: geçersiz yol istemde görünsün, yazılan metin sonradan reddedilmesin.
    const std::string probe_stem = name.stem.empty() ? "probe" : name.stem;
    const std::string probe_type = name.type.empty() ? "txt" : name.type;
    const std::string prefix = name.folder.empty() ? std::string{} : name.folder + "/";
    if (auto path = core::NotePath::parse(prefix + probe_stem + "." + probe_type); !path)
        return std::unexpected(path.error());
    return name;
}

std::string suggested_stem(std::string_view content, std::string_view type,
                           std::chrono::system_clock::time_point now, std::size_t slug_max_length) {
    return core::slugify(core::note_name_source(content, type), now, slug_max_length);
}

bool DraftTypeTracker::edited(std::size_t size_before, std::size_t size_after) {
    constexpr unsigned kRedetectShift = 5;  // metnin 1/32'si
    changes_ += std::max<std::size_t>(
        size_after > size_before ? size_after - size_before : size_before - size_after, 1);
    if (changes_ < std::max<std::size_t>(size_after >> kRedetectShift, 1))
        return false;
    changes_ = 0;
    return true;
}

std::expected<std::string, core::Error> Document::open(const core::NoteStore& store,
                                                       const core::NotePath& path) {
    auto content = store.read(path);
    if (!content)
        return std::unexpected(std::move(content.error()));
    path_ = path;
    return load(std::move(*content));
}

std::string Document::load(core::NoteContent content) {
    std::string raw = std::move(content.text);
    revision_ = content.revision;
    changed_on_disk_ = false;
    deleted_on_disk_ = false;
    bom_ = false;
    crlf_shown_lf_ = false;
    read_only_ = classify(raw, options_.max_bytes);
    if (read_only_ != ReadOnly::No) {
        if (read_only_ == ReadOnly::TooLarge)
            raw.resize(static_cast<std::size_t>(options_.max_bytes));
        return sanitized_utf8(raw);  // yalnız gösterilir, asla yazılmaz
    }
    if (raw.starts_with(kBom)) {
        bom_ = true;
        raw.erase(0, kBom.size());
    }
    if (options_.lf_editor && uniform_crlf(raw)) {
        crlf_shown_lf_ = true;
        raw = core::normalize_line_endings(raw);
    }
    return raw;
}

void Document::reset_draft() {
    *this = Document(options_);
}

std::string Document::encode(std::string_view text) const {
    std::string out = bom_ ? std::string(kBom) : std::string{};
    if (!crlf_shown_lf_)
        return out + std::string(text);
    out.reserve(out.size() + text.size() + text.size() / 16);
    for (const char c : text) {
        if (c == '\n')
            out += kCrlf;
        else
            out += c;
    }
    return out;
}

std::expected<Document::Saved, core::Error> Document::save(core::NoteStore& store, std::string_view text) {
    if (!path_ || !writable())
        std::abort();
    const auto written = store.write(*path_, encode(text), revision_);
    if (written) {
        adopt(*path_, *written);
        return Saved::Written;
    }
    switch (written.error().code) {
    case core::Error::Code::ModifiedExternally:
        return Saved::Conflict;
    case core::Error::Code::NotFound:
        return Saved::Gone;
    default:
        return std::unexpected(written.error());
    }
}

std::expected<void, core::Error> Document::overwrite(core::NoteStore& store, std::string_view text) {
    if (!path_ || !writable())
        std::abort();
    const auto fresh = store.read(*path_);
    if (!fresh)
        return std::unexpected(fresh.error());
    const auto written = store.write(*path_, encode(text), fresh->revision);
    if (!written)
        return std::unexpected(written.error());
    adopt(*path_, *written);
    return {};
}

std::expected<std::string, core::Error> Document::reload(const core::NoteStore& store) {
    if (!path_)
        std::abort();
    auto fresh = store.read(*path_);
    if (!fresh)
        return std::unexpected(std::move(fresh.error()));
    return load(std::move(*fresh));
}

std::expected<core::NotePath, core::Error>
Document::create_beside(core::NoteStore& store, std::string_view text, std::string_view suffix) {
    if (!path_)
        std::abort();
    core::NoteParts parts = core::split_note_path(path_->string());
    return create(
        store,
        core::NewNote{std::move(parts.folder), parts.name + std::string(suffix), std::move(parts.type), {}},
        text);
}

std::expected<core::NotePath, core::Error> Document::create(core::NoteStore& store, core::NewNote note,
                                                            std::string_view text) {
    if (!writable())
        std::abort();
    note.content = encode(text);
    auto created = store.create_exact(note);
    if (!created)
        return std::unexpected(std::move(created.error()));
    adopt(created->path, created->revision);
    return created->path;
}

void Document::moved(core::NotePath to, const core::Revision& revision) {
    adopt(std::move(to), revision);
}

void Document::note_disk_change(DiskChange change) {
    switch (change) {
    case DiskChange::None:
        deleted_on_disk_ = false;  // not yeniden göründü
        break;
    case DiskChange::Changed:
        changed_on_disk_ = true;
        deleted_on_disk_ = false;
        break;
    case DiskChange::Deleted:
        deleted_on_disk_ = true;
        changed_on_disk_ = false;
        break;
    case DiskChange::Reload:
    case DiskChange::Close:
        break;  // load() ve kapanış bayrakları kendisi sıfırlar
    }
}

void Document::adopt(core::NotePath path, const core::Revision& revision) {
    path_ = std::move(path);
    revision_ = revision;
    changed_on_disk_ = false;
    deleted_on_disk_ = false;
}

}  // namespace noteapp::app
