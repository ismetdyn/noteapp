#pragma once

#include "core/error.hpp"
#include "core/limits.hpp"
#include "pdfview/rendered_page.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// PDF önizlemesi (CLI `note show`, TUI metni, GUI sayfa görüntüleri): üç arayüz aynı kuralı buradan çağırır. Okuma
// pdfview::PdfDocument'tadır (öldürülebilir yardımcı süreç); çağrıları bloklar, burada UI iş parçacığı dışında koşar.
namespace noteapp::app {

// Not tipi PDF mi (uzantı, noktasız, büyük/küçük harf duyarsız): önizleme içerikten değil tipten seçilir; bozuk
// ya da PDF olmayan baytları PdfDocument reddeder.
bool is_pdf_type(std::string_view extension);

// Sayfa ayırıcı satırı (CLI ve TUI aynı): `page` 0 tabanlı, metinde 1 tabanlı ("--- page 3/12 ---").
std::string pdf_page_separator(int page, int count);

// Bütün belgenin metni sırayla, çağıranın iş parçacığında (CLI). `page_done` her sayfada bir kez: metin ya da
// sayfanın hatası (bozuk sayfa, text_bytes, süre); sayfa hatası okumayı durdurmaz. Açılış hatası ve durdurma
// (Cancelled) döner; durdurulan sayfa için `page_done` çağrılmaz.
using PdfPageSink = std::function<void(int page, int count, const std::expected<std::string, core::Error>& text)>;
std::expected<void, core::Error> read_pdf_text(std::string_view bytes, const core::PdfLimits& limits,
                                               const std::filesystem::path& worker, std::stop_token stop,
                                               const PdfPageSink& page_done);

struct PdfPreviewOptions {
    core::PdfLimits limits{};      // [limits.pdf]; cache_pages çizilmiş sayfa önbelleğidir
    std::filesystem::path worker;  // `note` ikilisi (üretimde core::note_executable_beside_self())
    // Her sonuçtan (açılış, sayfa) sonra işçi iş parçacığından; veri taşımaz, arayüz durumu okur (TUI Post, GUI
    // post). İçinden PdfPreview'un kilitli üyeleri çağrılabilir. Boş olabilir; atarsa loglanır, işçi sürer.
    std::function<void()> on_change;
};

// Açık bir PDF'in arka plan oturumu: tek işçi iş parçacığı belgeyi açar, istekleri FIFO sırayla yürütür. Aynı
// istek (kuyrukta, yürüyen, sonucu var ya da başarısız olmuş) yinelenmez. Başarısız sayfa (süre dolması dahil)
// hatasıyla hatırlanır, yeniden istenmez: kaydırma her karede aynı yavaş sayfayı yeniden başlatmaz. Çizilmiş
// sayfalar en çok limits.cache_pages tanedir (en az kullanılan düşer; 0 = sınırsız). Metinler take_text ile
// alınana dek kalır: çağıran ne kadar istediğini kendi sınırıyla tutar (TUI: limits.tui.view_bytes).
//
// drop_pending() yalnız bekleyen istekleri siler; yürüyen istek biter (onu kesmek işçiyi öldürür, sonraki istek
// yeniden başlatırdı). Yıkıcı yürüyen isteği keser (işçi ~50 ms içinde öldürülür) ve iş parçacığını bekler.
// Üyeler UI iş parçacığından çağrılır; kilitlidir.
class PdfPreview {
public:
    using Text = std::expected<std::string, core::Error>;
    using Page = std::expected<std::shared_ptr<const pdfview::RenderedPage>, core::Error>;
    enum class State { Opening, Ready, Failed };

    PdfPreview(std::string bytes, PdfPreviewOptions options);
    ~PdfPreview();
    PdfPreview(const PdfPreview&) = delete;
    PdfPreview& operator=(const PdfPreview&) = delete;

    State state() const;
    int page_count() const;                      // Ready değilse 0
    std::optional<core::Error> open_error() const;  // Failed'de dolu

    // Açılış bitmeden gelen istek bekler; sayfa aralık dışıysa ya da açılış başarısızsa yok sayılır. `scale`
    // sonlu ve > 0 (1 = 72 dpi).
    void request_text(int page);
    void request_render(int page, float scale);
    void drop_pending();
    std::size_t pending() const;  // bekleyen (yürüyen hariç) istek

    // Sonuç yoksa nullopt (henüz gelmedi ya da önbellekten düştü).
    std::optional<Text> text(int page) const;
    // text gibi ama sonucu oturumdan siler: metnin tek kopyası çağıranda kalır (TUI). Sayfa yeniden istenirse yeniden
    // okunur.
    std::optional<Text> take_text(int page);
    std::optional<Page> render(int page, float scale);  // en son kullanılan olur

    // Her değişiklikte (açılış, sonuç) artar: arayüz yeniden kurmak için karşılaştırır.
    std::uint64_t version() const;

private:
    enum class Kind { Text, Render };
    struct Request {
        Kind kind{};
        int page = 0;
        float scale = 0.0F;
        bool operator==(const Request&) const = default;
    };
    struct Cached {
        int page = 0;
        float scale = 0.0F;
        std::shared_ptr<const pdfview::RenderedPage> rendered;
    };

    void request(Request request);
    bool known(const Request& request) const;  // kilit altında: kuyrukta, yürüyor, sonucu ya da hatası var
    void run(std::string bytes, const std::stop_token& stop);
    void store_render(const Request& request, Page result);  // kilit altında
    void notify() const;

    const PdfPreviewOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    State state_ = State::Opening;
    int page_count_ = 0;
    std::optional<core::Error> open_error_;
    std::deque<Request> queue_;
    std::optional<Request> in_flight_;
    std::map<int, Text> texts_;
    std::list<Cached> rendered_;  // baş en son kullanılan
    std::map<std::pair<int, float>, core::Error> render_errors_;
    std::uint64_t version_ = 0;
    std::jthread worker_;  // son üye: önce durur, diğer üyeler yaşarken beklenir
};

}  // namespace noteapp::app
