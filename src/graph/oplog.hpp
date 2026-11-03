#pragma once

// İç başlık. Oplog dosyasının zarfı: başlık `"NGOL" ver dev[16]`, çerçeve `u32 len | SHA-256 ilk 8 bayt |
// payload`. Payload'ın kendisi op.*'dadır. Tarama saf bayt işidir; dosya I/O'su yalnız read_log_file ve
// export_log'dadır.
#include "graph/error.hpp"
#include "graph/graph.hpp"
#include "graph/op.hpp"
#include "graph/secure_file.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace noteapp::graph::detail {

class Store;

// Dosya sürümü (SPEC-graph "Parçalı çerçeve"): v1'de her kayıt tam bir çerçevedir, v2'de her kayıt bir
// parçadır. Yazan v1'i bölünmesi gereken ilk çerçeveye dek korur; okuyan ikisini de okur, daha yenisini
// log_too_new ile reddeder.
inline constexpr std::uint8_t kLogVersionWhole = 1;
inline constexpr std::uint8_t kLogVersionParts = 2;
inline constexpr std::uint8_t kLogVersion = kLogVersionParts;  // okunabilen en yeni sürüm
inline constexpr std::size_t kLogHeaderBytes = 4 + 1 + std::tuple_size_v<DeviceId>;
inline constexpr std::size_t kFrameDigestBytes = 8;
inline constexpr std::size_t kFrameEnvelopeBytes = sizeof(std::uint32_t) + kFrameDigestBytes;
// v2 kaydının yükü: tür u8 | seq u64 | index u32 | final u8 | baytlar. Bilinmeyen tür log_too_new'dir.
inline constexpr std::uint8_t kPartKind = 1;
inline constexpr std::size_t kPartHeaderBytes = 1 + sizeof(std::uint64_t) + sizeof(std::uint32_t) + 1;

std::string encode_log_header(const DeviceId& device, std::uint8_t version = kLogVersionWhole);
// Kaydın zarfı (uzunluk + özet). v1'de yük çerçevedir, v2'de parçadır (encode_log_part).
std::string encode_log_frame(std::string_view payload);
std::string encode_log_part(std::uint64_t seq, std::uint32_t index, bool final, std::string_view chunk);
// Yazanın parça sayısı: her parça kayıt sınırına sığar, işlem yoğunluğu parça başına limits.ops'u geçmez.
std::size_t log_parts_for(std::size_t payload_bytes, std::size_t ops, const FrameLimits& limits);
// `<32 küçük hex>.oplog`: başlıktaki dev ile karşılaştırılan ad.
std::string log_file_name(const DeviceId& device);

// Güvenilmez dosyayı mantıksal çerçeve çerçeve okur. Bellekte en çok bir kayıt ve bir (yarım) grup tutulur;
// uzunluk ayırmadan önce hem sınırla hem dosyadaki karşılığıyla denetlenir. Sondaki eksik kayıt ya da son
// parçası gelmemiş grup hata değildir (PartialTail): sonraki eşitlemede tamamlanır, bu arada görünmez.
class LogReader {
public:
    enum class Status { Frame, End, PartialTail, Damaged };

    // Dosya yoksa nullopt. Sembolik bağ ve normal olmayan dosya hatadır.
    // max_frame_bytes: bu cihazın kayıt sınırı (Store::frame_limits); kendi günlüğü de dışarıdan gelebilir.
    // max_group_bytes: birleştirilen mantıksal çerçevenin sınırı (limits.group_bytes, depolama sınırı).
    static Result<std::optional<LogReader>> open(const std::filesystem::path& path,
                                                 std::size_t max_frame_bytes,
                                                 std::size_t max_group_bytes = SIZE_MAX);

    // Başlığı okur: Frame geçerli, PartialTail kısa, Damaged bozuk. Sonrasında offset ilk çerçevededir.
    Status read_header();
    // Frame ise payload() mantıksal çerçevedir, sonraki next()'e kadar geçerli. Damaged ise error() nedenini
    // verir; daha yeni biçim TooNew'dur.
    Status next();
    void seek(std::uint64_t offset) { offset_ = offset; }

    std::span<const std::byte> payload() const {
        return std::as_bytes(std::span(payload_.data(), payload_.size()));
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- Damaged yalnız damaged() ile döner, o doldurur
    const Error& error() const { return *error_; }
    const DeviceId& device() const { return device_; }
    std::uint8_t version() const { return version_; }
    // Son çerçevenin kayıt sayısı (v1'de 1): işlem sınırı parça başınadır (logical_frame_limits).
    std::size_t parts() const { return parts_; }
    std::uint64_t offset() const { return offset_; }
    std::uint64_t file_size() const { return file_.size(); }

private:
    LogReader(InputFile file, std::size_t max_frame_bytes, std::size_t max_group_bytes)
        : file_(std::move(file)), max_frame_bytes_(max_frame_bytes), max_group_bytes_(max_group_bytes) {}
    Status damaged(Error error);
    // `at`taki kaydı record_'a okur, özetini doğrular; Frame ise end kaydın sonudur.
    Status read_record(std::uint64_t at, std::uint64_t& end);
    Status next_whole();
    Status next_group();

    InputFile file_;
    std::size_t max_frame_bytes_;
    std::size_t max_group_bytes_;
    DeviceId device_{};
    std::uint8_t version_ = kLogVersionWhole;
    std::uint64_t offset_ = 0;
    std::size_t parts_ = 0;
    std::string record_;
    std::string payload_;
    std::optional<Error> error_;
};

// Bu cihazın ops'unu dosyaya yazar. Dosya ops'un öneki değilse kenara alır, silmez ve yerine ops'tan üretir.
Result<ExportReport> export_log(Store& store, const std::filesystem::path& directory);

}  // namespace noteapp::graph::detail
