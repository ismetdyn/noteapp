#pragma once

#include "core/error.hpp"
#include "core/limits.hpp"
#include "pdfview/rendered_page.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

// PDF okuma istemcisi. Ayrıştırma ve çizim ayrı bir yardımcı süreçte koşar (`note __pdf-worker`, MuPDF yalnız orada
// bağlıdır): MuPDF'in güvenilmez girdide kesilemeyen döngüleri (onarım, bağlantı hedefi araması, ...) yalnız süreç
// öldürülerek sınırlanabilir. Bu sınıf MuPDF'i görmez; TUI ve GUI ikilileri MuPDF bağlamaz.
namespace noteapp::pdfview {

// Bellekteki baytlardan açılmış PDF; her belge uzun ömürlü kendi işçi sürecini taşır. Girdi güvenilmezdir
// (senkronla gelir): bozuk, kesik, şifreli ya da sınırı aşan belge core::Error döner. Sınırlar [limits.pdf]'tendir
// (0 = sınırsız). Aynı belge aynı anda tek iş parçacığından kullanılır; farklı belgeler (ayrı süreçler) koşut çalışır.
//
// Süre (limits.duration) ve durdurma (`stop`) bu süreçte tutulur: süre dolarsa ya da durdurma istenirse işçi
// öldürülür ve toplanır; çağrı LimitExceeded (`error.pdf_too_slow`) ya da Cancelled (`error.pdf_cancelled`) döner,
// kısmi sonuç dönmez. İşçi çökerse ya da bozuk çerçeve gönderirse aynı yol: InvalidDocument (`error.pdf_invalid`
// açılışta, `error.pdf_page_invalid` sayfada). Belge nesnesi geçerli kalır: sonraki çağrı işçiyi yeniden başlatır,
// baytları yeniden açtırır ve sayfa sayısının aynı olduğunu denetler (değilse InvalidDocument). Durdurma en geç ~50 ms
// içinde fark edilir. Kullanıcıya giden hata işçinin metnini taşımaz; işçinin ayrıntısı temizlenip loga yazılır.
class PdfDocument {
public:
    // Baytları kopyalar (yeniden başlatmada yeniden gönderilir). `worker`: işçiyi barındıran `note` ikilisi
    // (üretimde core::note_executable_beside_self()). Başlatılamazsa `error.process_not_found` ya da
    // `error.process_spawn_failed`. Diğer hatalar: InvalidDocument, Encrypted, LimitExceeded (sayfa sayısı, bellek
    // bütçesi, süre), Cancelled.
    static std::expected<PdfDocument, core::Error> open(std::string_view bytes, const core::PdfLimits& limits,
                                                        const std::filesystem::path& worker,
                                                        std::stop_token stop = {});

    PdfDocument(PdfDocument&&) noexcept;
    PdfDocument& operator=(PdfDocument&&) noexcept;
    PdfDocument(const PdfDocument&) = delete;
    PdfDocument& operator=(const PdfDocument&) = delete;
    ~PdfDocument();  // işçinin stdin'ini kapatır (temiz çıkış), kısa beklemeden sonra öldürüp toplar

    int page_count() const noexcept;

    // Sayfanın metni, okuma sırasıyla geçerli UTF-8 (satırlar '\n'). '\n' ve '\t' dışındaki denetim karakterleri,
    // tek vekil ve geçersiz dizi U+FFFD olur. Ön koşul: 0 <= page < page_count() (ihlal bug'dır: std::abort).
    // Metin limits.text_bytes'ı aşarsa LimitExceeded; sayfa bozuksa InvalidDocument.
    std::expected<std::string, core::Error> page_text(int page, std::stop_token stop = {});

    // Sayfayı `scale` kat (1 = 72 dpi) RGBA çizer. Ön koşul: 0 <= page < page_count(), scale sonlu ve > 0 (ihlal
    // bug'dır: std::abort). Piksel sayısı limits.render_pixels'ı aşarsa LimitExceeded; tampon ayrılamazsa
    // LimitExceeded (`error.pdf_no_memory`).
    std::expected<RenderedPage, core::Error> render_page(int page, float scale, std::stop_token stop = {});

private:
    struct Impl;
    explicit PdfDocument(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace noteapp::pdfview
