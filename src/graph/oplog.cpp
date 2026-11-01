#include "graph/oplog.hpp"

#include "graph/id.hpp"
#include "graph/store.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace noteapp::graph::detail {

namespace {

constexpr std::string_view kLogMagic = "NGOL";
constexpr std::size_t kMaxSetAsideLogs = 3;

Error bad_log(std::string reason) {
    return Error{Error::Code::BadFrame, "graph.bad_frame", {{"reason", std::move(reason)}}};
}

Error io_error(std::string what) {
    return Error{Error::Code::Io, "graph.log_io", {{"what", std::move(what)}}};
}

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::uint32_t read_u32_le(std::span<const std::byte> data) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < sizeof value; ++i)
        value |= std::to_integer<std::uint32_t>(data[i]) << (i * 8U);
    return value;
}

// Var olan kısım başlıkla çelişiyorsa bozuktur; yalnız kısaysa henüz gelmemiştir. nullopt: buraya kadar
// geçerli.
std::optional<Error> header_problem(std::span<const std::byte> file) {
    const std::size_t magic_part = std::min(file.size(), kLogMagic.size());
    if (!std::ranges::equal(file.first(magic_part), bytes_of(kLogMagic.substr(0, magic_part))))
        return bad_log("bad_magic");
    if (file.size() <= kLogMagic.size())
        return std::nullopt;
    const auto version = std::to_integer<std::uint8_t>(file[kLogMagic.size()]);
    if (version > kLogVersion)
        return Error{Error::Code::TooNew,
                     "graph.log_too_new",
                     {{"found", std::to_string(version)}, {"supported", std::to_string(kLogVersion)}}};
    if (version == 0)
        return bad_log("bad_version");
    return std::nullopt;
}

bool digest_matches(std::span<const std::byte> payload, std::span<const std::byte> digest) {
    const auto full = sha256(payload);
    return std::ranges::equal(std::span<const std::byte>(full).first(kFrameDigestBytes), digest);
}

}  // namespace

std::string encode_log_header(const DeviceId& device) {
    std::string out(kLogMagic);
    out.push_back(static_cast<char>(kLogVersion));
    out.append(reinterpret_cast<const char*>(device.data()), device.size());
    return out;
}

std::string encode_log_frame(std::string_view payload) {
    const auto size = static_cast<std::uint32_t>(payload.size());
    const auto digest = sha256(bytes_of(payload));
    std::string out;
    out.reserve(kFrameEnvelopeBytes + payload.size());
    for (std::size_t i = 0; i < sizeof size; ++i)
        out.push_back(static_cast<char>(size >> (i * 8U)));
    out.append(reinterpret_cast<const char*>(digest.data()), kFrameDigestBytes);
    out.append(payload);
    return out;
}

std::string log_file_name(const DeviceId& device) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string name;
    name.reserve(device.size() * 2 + 6);
    for (const std::byte value : device) {
        name.push_back(kHex[std::to_integer<std::size_t>(value >> 4U)]);
        name.push_back(kHex[std::to_integer<std::size_t>(value & std::byte{0x0F})]);
    }
    name += ".oplog";
    return name;
}

Result<std::optional<LogReader>> LogReader::open(const std::filesystem::path& path) {
    Result<std::optional<InputFile>> file = InputFile::open(path);
    if (!file)
        return std::unexpected(file.error());
    if (!file->has_value())
        return std::optional<LogReader>{};
    return std::optional<LogReader>{LogReader(std::move(**file))};
}

LogReader::Status LogReader::damaged(Error error) {
    error_ = std::move(error);
    return Status::Damaged;
}

LogReader::Status LogReader::read_header() {
    std::array<std::byte, kLogHeaderBytes> header{};
    const std::size_t available =
        static_cast<std::size_t>(std::min<std::uint64_t>(file_.size(), header.size()));
    const std::span<std::byte> part = std::span(header).first(available);
    if (const Result<void> read = file_.read_at(0, part); !read)
        return damaged(read.error());
    if (std::optional<Error> problem = header_problem(part))
        return damaged(std::move(*problem));
    if (available < header.size())
        return Status::PartialTail;
    std::ranges::copy(part.subspan(kLogMagic.size() + 1), device_.begin());
    offset_ = kLogHeaderBytes;
    return Status::Frame;
}

LogReader::Status LogReader::next() {
    const std::uint64_t remaining = file_.size() > offset_ ? file_.size() - offset_ : 0;
    if (remaining == 0)
        return Status::End;
    if (remaining < kFrameEnvelopeBytes)
        return Status::PartialTail;
    std::array<std::byte, kFrameEnvelopeBytes> envelope{};
    if (const Result<void> read = file_.read_at(offset_, envelope); !read)
        return damaged(read.error());
    const std::uint32_t size = read_u32_le(envelope);
    if (size > kMaxFrameBytes)
        return damaged(bad_log("frame_too_large"));
    if (size > remaining - kFrameEnvelopeBytes)
        return Status::PartialTail;
    payload_.resize(size);
    const std::span<std::byte> body = std::as_writable_bytes(std::span(payload_.data(), payload_.size()));
    if (const Result<void> read = file_.read_at(offset_ + kFrameEnvelopeBytes, body); !read)
        return damaged(read.error());
    if (!digest_matches(body, std::span(envelope).subspan(sizeof(std::uint32_t))))
        return damaged(bad_log("bad_digest"));
    offset_ += kFrameEnvelopeBytes + size;
    return Status::Frame;
}

namespace {

// export işlemi süresince tek anlık görüntü; çıkışta kapanır.
class ReadScope {
public:
    explicit ReadScope(Store& store) : store_(store) {}
    ReadScope(const ReadScope&) = delete;
    ReadScope& operator=(const ReadScope&) = delete;
    ~ReadScope() { store_.end_txn(); }

private:
    Store& store_;
};

// Kovulan dosyanın ve geçici dosyanın adı: tahmin edilemez, depoda önceden hazırlanamaz. UUIDv7'nin sonu
// rastgeledir.
std::string random_suffix() {
    const NodeId random = new_node_id();
    std::string name = log_file_name(random.bytes);
    name.resize(name.size() - std::string_view(".oplog").size());
    return name;
}

std::filesystem::path with_suffix(const std::filesystem::path& path, std::string_view infix) {
    std::filesystem::path result = path;
    result += infix;
    result += random_suffix();
    return result;
}

Result<void> write_string(OutputFile& out, std::string_view text) {
    return out.write(bytes_of(text));
}

Result<void> write_frames_after(Store& store, std::uint64_t after, OutputFile& out) {
    Result<Statement> rows = store.statement(StatementId::OpsOfDevice);
    if (!rows)
        return std::unexpected(rows.error());
    rows->bind_blob(1, store.device()).bind_int(2, static_cast<std::int64_t>(after));
    while (true) {
        const Result<bool> row = rows->step();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            return {};
        const std::span<const std::byte> payload = rows->column_blob(1);
        const std::string frame =
            encode_log_frame(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
        if (const Result<void> written = write_string(out, frame); !written)
            return written;
    }
}

// Sondaki yarım çerçeve hata değildir. ponytail: fsync yok. Dosya ops'tan yeniden üretilebilen taşıyıcıdır,
// çökmede sonraki export onu onarır.
Result<void> append_frames(Store& store, const std::filesystem::path& path, std::uint64_t after) {
    Result<OutputFile> out = OutputFile::open_append(path);
    if (!out)
        return std::unexpected(out.error());
    if (const Result<void> written = write_frames_after(store, after, *out); !written)
        return written;
    return out->close();
}

// Başarısızsa geçici dosya temizlenir: dizinde kalıntı birikmez.
class TemporaryFile {
public:
    explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    ~TemporaryFile() {
        if (!path_.empty()) {
            std::error_code ignored;  // temizlik asıl hatayı maskelemesin
            std::filesystem::remove(path_, ignored);
        }
    }
    const std::filesystem::path& path() const { return path_; }
    void keep() { path_.clear(); }

private:
    std::filesystem::path path_;
};

// Özel geçici dosya + rename: okuyan ya eski ya yeni dosyayı görür. Rename hedefteki bağı izlemez, onu
// değiştirir.
Result<void> rewrite_log(Store& store, const std::filesystem::path& path) {
    TemporaryFile temporary(with_suffix(path, "."));
    Result<OutputFile> out = OutputFile::create_new(temporary.path());
    if (!out)
        return std::unexpected(out.error());
    if (const Result<void> header = write_string(*out, encode_log_header(store.device())); !header)
        return header;
    if (const Result<void> written = write_frames_after(store, 0, *out); !written)
        return written;
    if (const Result<void> closed = out->close(); !closed)
        return closed;
    std::error_code code;
    std::filesystem::rename(temporary.path(), path, code);
    if (code)
        return std::unexpected(io_error("rename_failed"));
    temporary.keep();
    return {};
}

// Dosya ile bu veritabanının ilişkisi. Yalnız ops'un öneki olan dosya eklenebilir; yarım kuyruk da önektir.
struct LogState {
    enum class Kind { Missing, Prefix, PrefixWithTail, Alien } kind = Kind::Missing;
    std::uint64_t last_seq = 0;
    std::string reason;
};

LogState alien(std::string reason) {
    return LogState{LogState::Kind::Alien, 0, std::move(reason)};
}

std::string reason_of(const Error& error) {
    for (const auto& [name, value] : error.args)
        if (name == "reason" || name == "what")
            return value;
    return error.message_key;
}

// Her çerçeve karşılaştırılır: yalnız sonuncuya bakmak, ortadaki sahteciliği dosyada bırakırdı.
Result<LogState> frames_against_ops(Store& store, LogReader& reader) {
    std::uint64_t seq = 0;
    while (true) {
        switch (reader.next()) {
        case LogReader::Status::End:
            return LogState{LogState::Kind::Prefix, seq, {}};
        case LogReader::Status::PartialTail:
            return LogState{LogState::Kind::PrefixWithTail, seq, {}};
        case LogReader::Status::Damaged:
            return alien(reason_of(reader.error()));
        case LogReader::Status::Frame:
            break;
        }
        Result<Statement> stored = store.statement(StatementId::OpGet);
        if (!stored)
            return std::unexpected(stored.error());
        stored->bind_blob(1, store.device()).bind_int(2, static_cast<std::int64_t>(seq + 1));
        const Result<bool> row = stored->step();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            return alien("log_ahead_of_store");
        if (!std::ranges::equal(stored->column_blob(0), reader.payload()))
            return alien("device_forked");
        ++seq;
    }
}

Result<LogState> inspect_log(Store& store, const std::filesystem::path& path) {
    Result<std::optional<LogReader>> opened = LogReader::open(path);
    if (!opened)
        return opened.error().code == Error::Code::Io ? Result<LogState>{alien(reason_of(opened.error()))}
                                                      : std::unexpected(opened.error());
    if (!opened->has_value())
        return LogState{};
    LogReader& reader = **opened;
    switch (reader.read_header()) {
    case LogReader::Status::PartialTail:
        return LogState{LogState::Kind::PrefixWithTail, 0, {}};
    case LogReader::Status::Damaged:
        return alien(reason_of(reader.error()));
    default:
        break;
    }
    if (reader.device() != store.device())
        return alien("foreign_device");
    return frames_against_ops(store, reader);
}

// Kenara alınan kopyalar kendi ürettiğimiz yerel dosyalardır (git'e girmez). En iyi çaba: silinemeyen kopya
// export'u durdurmaz.
void prune_set_aside(const std::filesystem::path& path, const std::filesystem::path& newest) {
    const std::string prefix = path.filename().string() + ".aside-";
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> older;
    std::error_code code;
    for (std::filesystem::directory_iterator it(path.parent_path(), code), end; !code && it != end;
         it.increment(code)) {
        if (!it->path().filename().string().starts_with(prefix) || it->path() == newest)
            continue;
        std::error_code ignored;
        older.emplace_back(std::filesystem::last_write_time(it->path(), ignored), it->path());
    }
    std::ranges::sort(older, std::greater<>{});
    for (std::size_t i = kMaxSetAsideLogs - 1; i < older.size(); ++i) {
        std::error_code ignored;
        std::filesystem::remove(older[i].second, ignored);
    }
}

Result<std::filesystem::path> set_aside(const std::filesystem::path& path) {
    const std::filesystem::path aside = with_suffix(path, ".aside-");
    std::error_code code;
    std::filesystem::rename(path, aside, code);
    if (code)
        return std::unexpected(io_error("set_aside_failed"));
    prune_set_aside(path, aside);
    return aside;
}

}  // namespace

Result<ExportReport> export_log(Store& store, const std::filesystem::path& directory) {
    if (is_symbolic_link(directory))
        return std::unexpected(io_error("log_directory_symlink"));
    std::error_code code;
    std::filesystem::create_directories(directory, code);
    if (code)
        return std::unexpected(io_error("create_directory_failed"));
    if (const Result<void> begun = store.begin_read(); !begun)
        return std::unexpected(begun.error());
    const ReadScope scope(store);

    const std::filesystem::path path = directory / log_file_name(store.device());
    const Result<std::uint64_t> own = store.applied_seq(store.device());
    if (!own)
        return std::unexpected(own.error());
    const Result<LogState> state = inspect_log(store, path);
    if (!state)
        return std::unexpected(state.error());

    ExportReport report;
    report.last_seq = *own;
    if (state->kind == LogState::Kind::Alien) {
        Result<std::filesystem::path> aside = set_aside(path);
        if (!aside)
            return std::unexpected(aside.error());
        report.set_aside = std::move(*aside);
        report.reason = state->reason;
    }
    const bool nothing_to_write = *own == 0 && state->kind != LogState::Kind::PrefixWithTail;
    if (nothing_to_write || (state->kind == LogState::Kind::Prefix && state->last_seq == *own))
        return report;
    const Result<void> written = state->kind == LogState::Kind::Prefix
                                     ? append_frames(store, path, state->last_seq)
                                     : rewrite_log(store, path);
    if (!written)
        return std::unexpected(written.error());
    return report;
}

}  // namespace noteapp::graph::detail
