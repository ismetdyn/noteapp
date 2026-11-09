#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace noteapp::app {

// İşlem bildirimi (toast; SPEC-gui "Bildirimler", SPEC-tui "Bildirimler"). İki arayüz aynı kuyruğu çizer:
// GUI sağ altta kutular, TUI durum çubuğunun üstünde tek satır.
enum class NoticeKind { Info, Success, Error };

struct Notice {
    std::uint64_t id = 0;
    NoticeKind kind = NoticeKind::Info;
    std::string text;
    std::chrono::steady_clock::time_point posted;  // son yineleme anı: süre buradan sayılır
    std::size_t repeats = 1;                       // aynı tür ve metin üst üste gelince artar
    std::optional<std::string> folder;             // "Klasörü aç" eylemi (dışa aktarma)
    std::optional<std::string> update_version;     // "Güncelle" eylemi: kurulabilecek yeni sürüm (UPD5)
};

// Bildirim kuyruğu: en yeni sonda. Bilgi ve başarı `tuning.notice_ms` sonra düşer, hata ve yeni sürüm
// bildirimi kapatılana dek kalır (0 = hiçbiri kendiliğinden düşmez). Aynı tür ve metin yeni kutu açmaz: var
// olanın süresi tazelenir, sayısı artar. Süre dolumu bir uyanma anıdır (next_deadline), yoklama değil: boşta
// kare üretilmez. Kuyruk sınırsızdır; arayüz sığanı çizer.
class NoticeQueue {
public:
    using Clock = std::chrono::steady_clock;

    explicit NoticeQueue(std::chrono::milliseconds lifetime) : lifetime_(lifetime) {}
    // `tuning.notice_ms` (Limit, uint64) -> süre.
    // Tavan yok: temsil edilemeyen değer milliseconds::max() olur (kendiliğinden kapanmaz).
    static std::chrono::milliseconds lifetime_of(std::uint64_t ms) {
        using Rep = std::chrono::milliseconds::rep;
        constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<Rep>::max());
        return ms >= kMax ? std::chrono::milliseconds::max() : std::chrono::milliseconds(static_cast<Rep>(ms));
    }
    void set_lifetime(std::chrono::milliseconds lifetime) { lifetime_ = lifetime; }

    void post(NoticeKind kind, std::string text, Clock::time_point now,
              std::optional<std::string> folder = std::nullopt);
    // Yeni sürüm bildirimi (Info): arayüz `update_version` ile güncelleme penceresini açar. Süresi dolmaz:
    // kapatılana ya da kullanıcı ona davranana (dismiss_update) dek kalır; açılış denetimi en çok
    // update.check_interval'de bir gelir.
    void post_update(std::string text, std::string version, Clock::time_point now);
    void dismiss(std::uint64_t id);
    void dismiss_all(NoticeKind kind);  // TUI: sonraki tuş hatayı kapatır
    void dismiss_update();              // güncelleme penceresi açıldı
    // Süresi dolanları düşürür; bir şey düştüyse true (yeniden çiz).
    bool expire(Clock::time_point now);
    // En erken düşecek bildirimin anı; yoksa boş.
    std::optional<Clock::time_point> next_deadline() const;
    const std::vector<Notice>& items() const { return items_; }

private:
    void add(NoticeKind kind, std::string text, Clock::time_point now, std::optional<std::string> folder,
             std::optional<std::string> update_version);
    bool expires(const Notice& notice) const;
    // Düşme anı; süre saatin kalanını aşarsa (taşma) boş: kendiliğinden kapanmaz.
    std::optional<Clock::time_point> due(const Notice& notice) const;

    std::chrono::milliseconds lifetime_;
    std::vector<Notice> items_;
    std::uint64_t next_id_ = 1;
};

}  // namespace noteapp::app
