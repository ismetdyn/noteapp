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

inline constexpr std::uint8_t kLogVersion = 1;
inline constexpr std::size_t kLogHeaderBytes = 4 + 1 + std::tuple_size_v<DeviceId>;
inline constexpr std::size_t kFrameDigestBytes = 8;
inline constexpr std::size_t kFrameEnvelopeBytes = sizeof(std::uint32_t) + kFrameDigestBytes;

std::string encode_log_header(const DeviceId& device);
// Payload kMaxFrameBytes içinde olmalıdır; store_frame bunu yazarken dayatır.
std::string encode_log_frame(std::string_view payload);
// `<32 küçük hex>.oplog`: başlıktaki dev ile karşılaştırılan ad.
std::string log_file_name(const DeviceId& device);

// Güvenilmez dosyayı çerçeve çerçeve okur. Bellek en fazla bir çerçevedir ve uzunluk, ayırmadan önce hem
// sınırla hem dosyadaki karşılığıyla denetlenir. Sondaki eksik çerçeve hata değildir: sonraki eşitlemede
// tamamlanır.
class LogReader {
public:
    enum class Status { Frame, End, PartialTail, Damaged };

    // Dosya yoksa nullopt. Sembolik bağ ve normal olmayan dosya hatadır.
    static Result<std::optional<LogReader>> open(const std::filesystem::path& path);

    // Başlığı okur: Frame geçerli, PartialTail kısa, Damaged bozuk. Sonrasında offset ilk çerçevededir.
    Status read_header();
    // Frame ise payload() geçerlidir, sonraki next()'e kadar. Damaged ise error() nedenini verir.
    Status next();
    void seek(std::uint64_t offset) { offset_ = offset; }

    std::span<const std::byte> payload() const {
        return std::as_bytes(std::span(payload_.data(), payload_.size()));
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) -- Damaged yalnız damaged() ile döner, o doldurur
    const Error& error() const { return *error_; }
    const DeviceId& device() const { return device_; }
    std::uint64_t offset() const { return offset_; }
    std::uint64_t file_size() const { return file_.size(); }

private:
    explicit LogReader(InputFile file) : file_(std::move(file)) {}
    Status damaged(Error error);

    InputFile file_;
    DeviceId device_{};
    std::uint64_t offset_ = 0;
    std::string payload_;
    std::optional<Error> error_;
};

// Bu cihazın ops'unu dosyaya yazar. Dosya ops'un öneki değilse kenara alır, silmez ve yerine ops'tan üretir.
Result<ExportReport> export_log(Store& store, const std::filesystem::path& directory);

}  // namespace noteapp::graph::detail
