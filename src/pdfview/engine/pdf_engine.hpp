#pragma once

#include "core/error.hpp"
#include "core/limits.hpp"
#include "pdfview/rendered_page.hpp"
#include "pdfview/structured_text.hpp"

#include <expected>
#include <memory>
#include <string>
#include <string_view>

// PDF okuma motoru (MuPDF, `noteapp_pdfengine`). src/pdf PDF ÜRETİR (libharu); bu modül PDF OKUR. Yalnız PDF işçi
// süreci (`note __pdf-worker`, engine/worker.cpp) ve testleri kullanır; uygulama pdfview::PdfDocument'i kullanır.
namespace noteapp::pdfview {

// Bellekteki baytlardan açılmış PDF. Girdi güvenilmezdir (senkronla gelir): bozuk, kesik, şifreli ya da sınırı
// aşan belge core::Error döner. Sınırlar [limits.pdf]'tendir (0 = sınırsız); `duration` burada kullanılmaz.
//
// Süre ve iptal yoktur: MuPDF'in bazı döngüleri (bozuk dosya onarımı, bağlantı hedefinin yavaş sayfa araması, ...)
// hiçbir kesmeye bakmaz. Bu yüzden motor ayrı süreçte koşar ve süre/iptal ebeveynin süreci öldürmesiyle sağlanır
// (pdfview::PdfDocument).
//
// İş parçacığı: her belge kendi MuPDF bağlamını (fz_context) ve bellek bütçesini taşır. Aynı belge aynı anda tek
// iş parçacığından kullanılır; farklı belgeler farklı iş parçacıklarında aynı anda çalışabilir — MuPDF'in süreç geneli
// durumu (openjpeg ayırıcısı, FreeType) için bağlamlar ortak kilit takımını paylaşır.
class PdfEngine {
public:
    // Baytları kopyalar: çağıranın tamponu dönüşten sonra serbest kalabilir. Hatalar: InvalidDocument (PDF değil,
    // bozuk), Encrypted (parola ister), LimitExceeded (sayfa sayısı > limits.pages ya da bellek bütçesi aşıldı).
    // Hata MuPDF iletisini taşımaz (belge baytı içerebilir). `failure_detail` boş değilse açılış hatasında MuPDF
    // iletisi buraya yazılır (yalnız teşhis, temizlenmemiş).
    static std::expected<PdfEngine, core::Error> open(std::string_view bytes, const core::PdfLimits& limits,
                                                      std::string* failure_detail = nullptr);

    PdfEngine(PdfEngine&&) noexcept;
    PdfEngine& operator=(PdfEngine&&) noexcept;
    PdfEngine(const PdfEngine&) = delete;
    PdfEngine& operator=(const PdfEngine&) = delete;
    ~PdfEngine();

    int page_count() const noexcept;

    // Son başarısız page_text/render_page'in MuPDF iletisi (yalnız teşhis; kullanıcıya gitmez, temizlenmemiş).
    std::string_view last_failure_detail() const noexcept;

    // Sayfanın metni, okuma sırasıyla geçerli UTF-8 (satırlar '\n', paragraflar boş satırla ayrılır). Belgenin
    // ToUnicode'u düşmanca olabilir: '\n' ve '\t' dışındaki denetim karakterleri (C0, DEL, C1), tek vekil ve geçersiz
    // dizi U+FFFD olur. Ön koşul: 0 <= page < page_count() (ihlal bug'dır: std::abort). Metin limits.text_bytes'ı
    // aşarsa LimitExceeded; sayfa bozuksa InvalidDocument.
    std::expected<std::string, core::Error> page_text(int page);

    // Sayfanın yapılı metni (bloklar, satırlar, biçimli parçalar, karakter kutuları; structured_text.hpp). Metin
    // geçerli UTF-8'dir: denetim karakterleri ve geçersiz kod noktaları U+FFFD, sekme ve satır sonu boşluk olur.
    // Sayfa dışındaki karakterler atılır, kutular sayfaya kırpılır. Ön koşul ve hatalar page_text gibi (toplam
    // metin limits.text_bytes).
    std::expected<PageText, core::Error> page_structure(int page);

    // Sayfayı `scale` kat (1 = 72 dpi) çizer. Ön koşul: 0 <= page < page_count(), scale sonlu ve > 0 (ihlal
    // bug'dır: std::abort). Piksel sayısı limits.render_pixels'ı aşarsa çizmeden LimitExceeded. Boş MediaBox'ı
    // MuPDF 612x792 pt sayar.
    std::expected<RenderedPage, core::Error> render_page(int page, float scale);

private:
    struct Impl;
    explicit PdfEngine(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace noteapp::pdfview
