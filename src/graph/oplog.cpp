#include "graph/oplog.hpp"

#include "graph/id.hpp"
#include "graph/store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace noteapp::graph::detail {

namespace {

constexpr std::string_view kLogMagic = "NGOL";

Error bad_log(std::string reason) {
    return Error{Error::Code::BadFrame, "graph.bad_frame", {{"reason", std::move(reason)}}};
}

Error io_error(std::string what) {
    return Error{Error::Code::Io, "graph.log_io", {{"what", std::move(what)}}};
}

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

template <class Unsigned> Unsigned read_le(std::span<const std::byte> data) {
    Unsigned value = 0;
    for (std::size_t i = 0; i < sizeof value; ++i)
        value = static_cast<Unsigned>(value | (std::to_integer<Unsigned>(data[i]) << (i * 8U)));
    return value;
}

template <class Unsigned> void put_le(std::string& out, Unsigned value) {
    for (std::size_t i = 0; i < sizeof(Unsigned); ++i)
        out.push_back(static_cast<char>(value >> (i * 8U)));
}

// Daha yeni sürümün biçimi: okunmaz, yanlış da okunmaz. Neden tek: kullanıcıya "uygulamayı güncelle" denir.
Error too_new(std::vector<std::pair<std::string, std::string>> detail) {
    detail.insert(detail.begin(), {"reason", "log_too_new"});
    return Error{Error::Code::TooNew, "graph.log_too_new", std::move(detail)};
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
        return too_new({{"found", std::to_string(version)}, {"supported", std::to_string(kLogVersion)}});
    if (version == 0)
        return bad_log("bad_version");
    return std::nullopt;
}

bool digest_matches(std::span<const std::byte> payload, std::span<const std::byte> digest) {
    const auto full = sha256(payload);
    return std::ranges::equal(std::span<const std::byte>(full).first(kFrameDigestBytes), digest);
}

}  // namespace

std::string encode_log_header(const DeviceId& device, std::uint8_t version) {
    std::string out(kLogMagic);
    out.push_back(static_cast<char>(version));
    out.append(reinterpret_cast<const char*>(device.data()), device.size());
    return out;
}

std::string encode_log_frame(std::string_view payload) {
    const auto digest = sha256(bytes_of(payload));
    std::string out;
    out.reserve(kFrameEnvelopeBytes + payload.size());
    put_le(out, static_cast<std::uint32_t>(payload.size()));
    out.append(reinterpret_cast<const char*>(digest.data()), kFrameDigestBytes);
    out.append(payload);
    return out;
}

std::string encode_log_part(std::uint64_t seq, std::uint32_t index, bool final, std::string_view chunk) {
    std::string payload;
    payload.reserve(kPartHeaderBytes + chunk.size());
    payload.push_back(static_cast<char>(kPartKind));
    put_le(payload, seq);
    put_le(payload, index);
    payload.push_back(final ? '\1' : '\0');
    payload.append(chunk);
    return encode_log_frame(payload);
}

std::size_t log_parts_for(std::size_t payload_bytes, std::size_t ops, const FrameLimits& limits) {
    // Saçma küçük sınırda (başlıktan küçük kayıt) da ilerler: kayıt sınırı aşılır, okuyan o günlüğü bekletir.
    const std::size_t room = limits.bytes > kPartHeaderBytes ? limits.bytes - kPartHeaderBytes : 1;
    const std::size_t ops_room = std::max<std::size_t>(limits.ops, 1);
    const std::size_t by_bytes = payload_bytes / room + (payload_bytes % room != 0 ? 1 : 0);
    const std::size_t by_ops = ops / ops_room + (ops % ops_room != 0 ? 1 : 0);
    return std::max({std::size_t{1}, by_bytes, by_ops});
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

Result<std::optional<LogReader>> LogReader::open(const std::filesystem::path& path,
                                                 std::size_t max_frame_bytes, std::size_t max_group_bytes) {
    Result<std::optional<InputFile>> file = InputFile::open(path);
    if (!file)
        return std::unexpected(file.error());
    if (!file->has_value())
        return std::optional<LogReader>{};
    return std::optional<LogReader>{LogReader(std::move(**file), max_frame_bytes, max_group_bytes)};
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
    // Cihaz, reddedilen (daha yeni) başlıkta da okunur: kullanıcıya hangi cihazın güncellenmesi gerektiği söylenir.
    if (available == header.size())
        std::ranges::copy(part.subspan(kLogMagic.size() + 1), device_.begin());
    if (std::optional<Error> problem = header_problem(part))
        return damaged(std::move(*problem));
    if (available < header.size())
        return Status::PartialTail;
    version_ = std::to_integer<std::uint8_t>(part[kLogMagic.size()]);
    offset_ = kLogHeaderBytes;
    return Status::Frame;
}

LogReader::Status LogReader::read_record(std::uint64_t at, std::uint64_t& end) {
    const std::uint64_t remaining = file_.size() > at ? file_.size() - at : 0;
    if (remaining == 0)
        return Status::End;
    if (remaining < kFrameEnvelopeBytes)
        return Status::PartialTail;
    std::array<std::byte, kFrameEnvelopeBytes> envelope{};
    if (const Result<void> read = file_.read_at(at, envelope); !read)
        return damaged(read.error());
    const auto size = read_le<std::uint32_t>(envelope);
    if (size > max_frame_bytes_)
        return damaged(bad_log("frame_too_large"));
    if (size > remaining - kFrameEnvelopeBytes)
        return Status::PartialTail;
    record_.resize(size);
    const std::span<std::byte> body = std::as_writable_bytes(std::span(record_.data(), record_.size()));
    if (const Result<void> read = file_.read_at(at + kFrameEnvelopeBytes, body); !read)
        return damaged(read.error());
    if (!digest_matches(body, std::span(envelope).subspan(sizeof(std::uint32_t))))
        return damaged(bad_log("bad_digest"));
    end = at + kFrameEnvelopeBytes + size;
    return Status::Frame;
}

LogReader::Status LogReader::next() {
    return version_ == kLogVersionWhole ? next_whole() : next_group();
}

LogReader::Status LogReader::next_whole() {
    std::uint64_t end = 0;
    if (const Status status = read_record(offset_, end); status != Status::Frame)
        return status;
    if (record_.size() > max_group_bytes_)
        return damaged(bad_log("group_too_large"));
    payload_.swap(record_);
    parts_ = 1;
    offset_ = end;
    return Status::Frame;
}

// Grup, ardışık kayıtlardır: aynı seq, 0'dan artan index, sonuncusunda final. Son parça gelmeden hiçbir şey
// verilmez; offset grubun başında kalır, sonraki çağrı (sonraki eşitleme) baştan okur.
LogReader::Status LogReader::next_group() {
    payload_.clear();
    std::uint64_t at = offset_;
    std::uint64_t group = 0;
    for (std::uint32_t index = 0;; ++index) {
        std::uint64_t end = 0;
        const Status status = read_record(at, end);
        if (status == Status::End)
            return index == 0 ? Status::End : Status::PartialTail;
        if (status != Status::Frame)
            return status;
        const std::span<const std::byte> record = std::as_bytes(std::span(record_.data(), record_.size()));
        if (record.size() < kPartHeaderBytes)
            return damaged(bad_log("truncated_part"));
        if (std::to_integer<std::uint8_t>(record[0]) != kPartKind)
            return damaged(too_new({{"detail", "unknown_record_kind"}}));
        const auto seq = read_le<std::uint64_t>(record.subspan(1));
        const auto part_index = read_le<std::uint32_t>(record.subspan(1 + sizeof seq));
        const auto final = std::to_integer<std::uint8_t>(record[kPartHeaderBytes - 1]);
        if (final > 1)
            return damaged(bad_log("bad_part"));
        if (index == 0)
            group = seq;
        if (seq != group || part_index != index)
            return damaged(bad_log("broken_group"));
        if (record.size() - kPartHeaderBytes > max_group_bytes_ - payload_.size())
            return damaged(bad_log("group_too_large"));
        payload_.append(record_, kPartHeaderBytes);
        at = end;
        if (final == 1) {
            parts_ = std::size_t{index} + 1;
            break;
        }
    }
    // Grup kimliği çerçevenin kendi seq'idir: başka grubun baytları eklenmiş olamaz.
    if (payload_.size() < sizeof group || read_le<std::uint64_t>(payload()) != group)
        return damaged(bad_log("broken_group"));
    offset_ = at;
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

std::string_view text_of(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// v1 kaydına sığmayan çerçeve: okuyan aynı sınırla bad_frame verirdi, parçalara bölünmelidir.
bool needs_parts(std::span<const std::byte> payload, const FrameLimits& limits) {
    return payload.size() > limits.bytes || peek_op_count(payload).value_or(0) > limits.ops;
}

// Bu cihazın `after`'dan sonraki çerçevelerinden biri bölünmeli mi: dosya o zaman v2 olarak yazılır.
Result<bool> needs_parts_after(Store& store, std::uint64_t after) {
    Result<Statement> rows = store.statement(StatementId::OpsOfDevice);
    if (!rows)
        return std::unexpected(rows.error());
    rows->bind_blob(1, store.device()).bind_int(2, static_cast<std::int64_t>(after));
    const FrameLimits limits = store.frame_limits();
    while (true) {
        const Result<bool> row = rows->step();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            return false;
        if (needs_parts(rows->column_blob(1), limits))
            return true;
    }
}

// Parçalar eşit boydadır; her biri ayrı kayıt olarak yazılır, bellekte çerçevenin kopyası tutulmaz.
Result<void> write_parts(OutputFile& out, std::uint64_t seq, std::string_view payload,
                         const FrameLimits& limits) {
    const std::size_t parts =
        log_parts_for(payload.size(), peek_op_count(std::as_bytes(std::span(payload))).value_or(0), limits);
    std::size_t offset = 0;
    for (std::size_t index = 0; index < parts; ++index) {
        const std::size_t size = payload.size() / parts + (index < payload.size() % parts ? 1 : 0);
        const std::string record = encode_log_part(seq, static_cast<std::uint32_t>(index), index + 1 == parts,
                                                   payload.substr(offset, size));
        if (const Result<void> written = write_string(out, record); !written)
            return written;
        offset += size;
    }
    return {};
}

Result<void> write_frames_after(Store& store, std::uint64_t after, std::uint8_t version, OutputFile& out) {
    Result<Statement> rows = store.statement(StatementId::OpsOfDevice);
    if (!rows)
        return std::unexpected(rows.error());
    rows->bind_blob(1, store.device()).bind_int(2, static_cast<std::int64_t>(after));
    const FrameLimits limits = store.frame_limits();
    while (true) {
        const Result<bool> row = rows->step();
        if (!row)
            return std::unexpected(row.error());
        if (!*row)
            return {};
        const auto seq = static_cast<std::uint64_t>(rows->column_int(0));
        const std::string_view payload = text_of(rows->column_blob(1));
        const Result<void> written = version == kLogVersionWhole
                                         ? write_string(out, encode_log_frame(payload))
                                         : write_parts(out, seq, payload, limits);
        if (!written)
            return written;
    }
}

// Sondaki yarım çerçeve hata değildir. ponytail: fsync yok. Dosya ops'tan yeniden üretilebilen taşıyıcıdır,
// çökmede sonraki export onu onarır.
Result<void> append_frames(Store& store, const std::filesystem::path& path, std::uint64_t after,
                           std::uint8_t version) {
    Result<OutputFile> out = OutputFile::open_append(path);
    if (!out)
        return std::unexpected(out.error());
    if (const Result<void> written = write_frames_after(store, after, version, *out); !written)
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
// değiştirir. Sürüm, bölünmesi gereken çerçeve varsa v2'dir; yoksa eski sürümler okuyabilsin diye v1.
Result<void> rewrite_log(Store& store, const std::filesystem::path& path) {
    const Result<bool> parts = needs_parts_after(store, 0);
    if (!parts)
        return std::unexpected(parts.error());
    const std::uint8_t version = *parts ? kLogVersionParts : kLogVersionWhole;
    TemporaryFile temporary(with_suffix(path, "."));
    Result<OutputFile> out = OutputFile::create_new(temporary.path());
    if (!out)
        return std::unexpected(out.error());
    if (const Result<void> header = write_string(*out, encode_log_header(store.device(), version)); !header)
        return header;
    if (const Result<void> written = write_frames_after(store, 0, version, *out); !written)
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
    std::uint8_t version = kLogVersionWhole;  // önek dosyanın sürümü: ekleme aynı biçimle yapılır
};

LogState alien(std::string reason) {
    return LogState{LogState::Kind::Alien, 0, std::move(reason), kLogVersionWhole};
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
            return LogState{LogState::Kind::Prefix, seq, {}, reader.version()};
        case LogReader::Status::PartialTail:
            return LogState{LogState::Kind::PrefixWithTail, seq, {}, reader.version()};
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
    // Kendi günlüğümüz de git'ten gelebilir (başkası değiştirmiş olabilir): uzak günlükle aynı sınır.
    Result<std::optional<LogReader>> opened = LogReader::open(
        path, store.frame_limits().bytes, std::min(store.limits().group_bytes, store.max_blob_bytes()));
    if (!opened)
        return opened.error().code == Error::Code::Io ? Result<LogState>{alien(reason_of(opened.error()))}
                                                      : std::unexpected(opened.error());
    if (!opened->has_value())
        return LogState{};
    LogReader& reader = **opened;
    switch (reader.read_header()) {
    case LogReader::Status::PartialTail:
        return LogState{LogState::Kind::PrefixWithTail, 0, {}, kLogVersionWhole};
    case LogReader::Status::Damaged:
        return alien(reason_of(reader.error()));
    default:
        break;
    }
    if (reader.device() != store.device())
        return alien("foreign_device");
    return frames_against_ops(store, reader);
}


// v1 dosyasına bölünmesi gereken çerçeve eklenemez: dosya v2 olarak baştan yazılır, kenara alınmaz.
Result<void> append_or_upgrade(Store& store, const std::filesystem::path& path, const LogState& state) {
    if (state.version == kLogVersionWhole) {
        const Result<bool> parts = needs_parts_after(store, state.last_seq);
        if (!parts)
            return std::unexpected(parts.error());
        if (*parts)
            return rewrite_log(store, path);
    }
    return append_frames(store, path, state.last_seq, state.version);
}

Result<std::filesystem::path> set_aside(const std::filesystem::path& path) {
    std::filesystem::path aside = with_suffix(path, ".aside-");
    std::error_code code;
    std::filesystem::rename(path, aside, code);
    if (code)
        return std::unexpected(io_error("set_aside_failed"));
    return aside;
}

constexpr std::string_view kSetAsideKey = "set_aside_logs";

std::string utf8_of(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path path_of_utf8(std::string_view text) {
    return std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size());
}

Result<std::vector<std::string>> set_aside_names(Store& store) {
    Result<Statement> stored = store.statement(StatementId::MetaGet);
    if (!stored)
        return std::unexpected(stored.error());
    stored->bind_text(1, kSetAsideKey);
    const Result<bool> row = stored->step();
    if (!row)
        return std::unexpected(row.error());
    std::vector<std::string> names;
    if (!*row)
        return names;
    const std::span<const std::byte> blob = stored->column_blob(0);
    std::string_view rest(reinterpret_cast<const char*>(blob.data()), blob.size());
    while (!rest.empty()) {
        const std::size_t end = std::min(rest.find('\n'), rest.size());
        if (end > 0)
            names.emplace_back(rest.substr(0, end));
        rest.remove_prefix(std::min(end + 1, rest.size()));
    }
    return names;
}

// Kenara alınan kopyaların listesi (eskiden yeniye) bu cihazın yerel durumudur (meta): budama yalnız onları
// siler. Günlük dizini depodur; oradan gelen sahte `<cihaz>.oplog.aside-*` dosyaları (checkout'la yeni mtime
// alır) ne sayılır ne silinir (L4c). Silme commit'ten sonradır: liste güncellenemezse hiçbir kopya gitmez.
Result<void> remember_set_aside(Store& store, const std::filesystem::path& aside, std::size_t keep) {
    if (const Result<void> begun = store.begin_write(); !begun)
        return begun;
    Result<std::vector<std::string>> names = set_aside_names(store);
    if (!names) {
        store.end_txn();
        return std::unexpected(names.error());
    }
    // Ad günlük dizinine göredir: notes_dir taşınsa da budama aynı kopyaları bulur.
    names->push_back(utf8_of(aside.filename()));
    std::vector<std::string> doomed;
    // keep en yenisi dahil kopya sayısıdır; SIZE_MAX (config 0) hiçbirini silmez.
    if (names->size() > keep) {
        const auto first_kept = names->end() - static_cast<std::ptrdiff_t>(keep);
        doomed.assign(names->begin(), first_kept);
        names->erase(names->begin(), first_kept);
    }
    std::string value;
    for (const std::string& name : *names)
        value += name + '\n';
    Result<Statement> put = store.statement(StatementId::MetaPut);
    if (!put) {
        store.end_txn();
        return std::unexpected(put.error());
    }
    put->bind_text(1, kSetAsideKey).bind_blob(2, bytes_of(value));
    const Result<void> stored = put->run();
    if (!stored) {
        store.end_txn();
        return stored;
    }
    if (const Result<void> committed = store.commit_write(); !committed)
        return committed;
    for (const std::string& name : doomed) {
        std::error_code ignored;  // zaten yoksa (elle silinmiş) iş bitmiştir
        std::filesystem::remove(aside.parent_path() / path_of_utf8(name).filename(), ignored);
    }
    return {};
}


Result<ExportReport> write_log(Store& store, const std::filesystem::path& directory) {
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
                                     ? append_or_upgrade(store, path, *state)
                                     : rewrite_log(store, path);
    if (!written)
        return std::unexpected(written.error());
    return report;
}

}  // namespace

Result<ExportReport> export_log(Store& store, const std::filesystem::path& directory) {
    Result<ExportReport> report = write_log(store, directory);  // okuma işlemi içinde; burada kapalı
    if (!report || !report->set_aside)
        return report;
    if (const Result<void> remembered =
            remember_set_aside(store, *report->set_aside, store.limits().set_aside_logs);
        !remembered)
        return std::unexpected(remembered.error());
    return report;
}

}  // namespace noteapp::graph::detail
