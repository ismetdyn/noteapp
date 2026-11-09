#include "pdfview/engine/pdf_engine.hpp"

#include "core/i18n.hpp"
#include "pdfview/engine/memory_budget.hpp"
#include "pdfview/page_text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

// Kapalı belge işleyicileri ve özellikler cmake/MuPDF.cmake'tedir; güvenlik ve lisans kararları (JS çalışmaz, yalnız
// PDF açılır) bunlara dayanır. Biri yanlışlıkla açılırsa derleme durur.
static_assert(FZ_ENABLE_JS == 0, "güvenilmez PDF'in JavaScript'i çalışmamalı");
static_assert(FZ_ENABLE_XPS == 0 && FZ_ENABLE_SVG == 0 && FZ_ENABLE_CBZ == 0 && FZ_ENABLE_IMG == 0);
static_assert(FZ_ENABLE_HTML == 0 && FZ_ENABLE_EPUB == 0 && FZ_ENABLE_FB2 == 0 && FZ_ENABLE_MOBI == 0);
static_assert(FZ_ENABLE_TXT == 0 && FZ_ENABLE_OFFICE == 0 && FZ_ENABLE_MD == 0 && FZ_ENABLE_HTML_ENGINE == 0);
static_assert(FZ_ENABLE_OCR_OUTPUT == 0 && FZ_ENABLE_DOCX_OUTPUT == 0 && FZ_ENABLE_ODT_OUTPUT == 0);
static_assert(FZ_ENABLE_BROTLI == 0 && FZ_ENABLE_BARCODE == 0 && FZ_ENABLE_ICC == 0);

namespace noteapp::pdfview {

namespace {

// MuPDF hataları fz_try/fz_catch ile, yani setjmp/longjmp ile yayılır. longjmp, atladığı C++ yığın çerçevelerinin
// yıkıcılarını çalıştırmaz ve fz_try ile longjmp arasında değişen otomatik değişkenlerin değeri belirsizdir. Bu
// yüzden her fz_try aşağıdaki "mupdf_*" işlevlerinden birinin içindedir: bu işlevlerin yerelleri yalnız C
// işaretçisi/tam sayıdır (fz_var ile belleğe alınır), sonuçlar çağıranın nesnesine işaretçiyle yazılır, MuPDF'ten
// C++ koduna hiçbir çağrı yapılmaz (geri çağırmalar noexcept ve atmaz). RAII sarmalayıcıları ve std::string bu
// işlevlerin DIŞINDA, MuPDF döndükten sonra kurulur.

constexpr std::size_t kMessageCapacity = 256;  // fz_context'in hata iletisi tamponu da 256 bayttır

// MuPDF hatasının C++'a taşınan kopyası: fz_catch içinde yalnız C işlemleriyle doldurulur.
struct Failure {
    int code = FZ_ERROR_NONE;
    std::array<char, kMessageCapacity> message{};
};

void record_failure(fz_context* ctx, Failure* failure) noexcept {
    failure->code = fz_caught(ctx);
    const char* message = fz_caught_message(ctx);
    std::size_t length = 0;
    while (length + 1 < kMessageCapacity && message[length] != '\0') {
        failure->message[length] = message[length];
        ++length;
    }
    failure->message[length] = '\0';
    // Hatayı "işlendi" say: bir sonraki fz_throw "UNHANDLED EXCEPTION" uyarısı üretmesin. İleti geri çağırmaya
    // gider (sessiz), hata core::Error olarak döner.
    fz_report_error(ctx);
}

// ---- Süreç geneli kilitler ----
// Her belgenin bağlamı ayrıdır, ama MuPDF'in süreç geneli durumu vardır: openjpeg ayırıcısı statik bir bağlam
// işaretçisi kullanır (load-jpx.c: "fz_lock/fz_unlock handlers [must be] shared between all the fz_contexts in use
// at a time"). Kilitsiz iki bağlam iki iş parçacığında JPX çözerse yarışırdı; bu yüzden tüm bağlamlar aynı
// FZ_LOCK_MAX muteks takımını paylaşır. Kilit sırası MuPDF'indir (context.h): i <= n olan bir kilit tutulurken n
// alınmaz, yani kilitler azalan numarayla alınır ve FZ_LOCK_ALLOC (0) en son alınır.
std::array<std::mutex, FZ_LOCK_MAX>& shared_mutexes() {
    static std::array<std::mutex, FZ_LOCK_MAX> mutexes;
    return mutexes;
}

void lock_shared(void* /*user*/, int lock) noexcept {
    shared_mutexes()[static_cast<std::size_t>(lock)].lock();
}

void unlock_shared(void* /*user*/, int lock) noexcept {
    shared_mutexes()[static_cast<std::size_t>(lock)].unlock();
}

// MuPDF uyarı ve hata iletilerini varsayılan olarak stderr'e yazar: güvenilmez PDF TUI'nin ekranını ve CLI
// çıktısını bunlarla doldururdu. Hatalar core::Error olarak döner; uyarılar (onarılan xref vb.) atılır.
void ignore_message(void* /*user*/, const char* /*message*/) noexcept {}

// ---- MuPDF sınırı: yalnız C yerelleri, fz_try burada ----

struct OpenedPdf {
    fz_stream* stream = nullptr;
    pdf_document* doc = nullptr;
    int needs_password = 0;
    int pages = 0;
};

bool mupdf_open(fz_context* ctx, const unsigned char* data, std::size_t size, OpenedPdf* out,
                Failure* failure) noexcept {
    fz_try(ctx) {
        out->stream = fz_open_memory(ctx, data, size);
        out->doc = pdf_open_document_with_stream(ctx, out->stream);
        out->needs_password = pdf_needs_password(ctx, out->doc);
        if (out->needs_password == 0)
            out->pages = pdf_count_pages(ctx, out->doc);
    }
    fz_catch(ctx) {
        record_failure(ctx, failure);
        return false;
    }
    return true;
}

bool mupdf_page_text(fz_context* ctx, pdf_document* doc, int index, fz_buffer** out, Failure* failure) noexcept {
    fz_page* page = nullptr;
    fz_stext_page* text = nullptr;
    fz_var(page);
    fz_var(text);
    fz_try(ctx) {
        page = fz_load_page(ctx, &doc->super, index);
        text = fz_new_stext_page_from_page(ctx, page, nullptr);
        *out = fz_new_buffer_from_stext_page(ctx, text);
    }
    fz_always(ctx) {
        fz_drop_stext_page(ctx, text);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        record_failure(ctx, failure);
        return false;
    }
    return true;
}

// Sayfayı yükler ve `scale` kat çizimin piksel kutusunu hesaplar (fz_new_pixmap_from_page ile aynı yuvarlama).
bool mupdf_page_box(fz_context* ctx, pdf_document* doc, int index, float scale, fz_page** page_out, fz_irect* box,
                    Failure* failure) noexcept {
    fz_try(ctx) {
        *page_out = fz_load_page(ctx, &doc->super, index);
        *box = fz_round_rect(fz_transform_rect(fz_bound_page(ctx, *page_out), fz_scale(scale, scale)));
    }
    fz_catch(ctx) {
        record_failure(ctx, failure);
        return false;
    }
    return true;
}

// `samples` çağıranın RGBA tamponudur (box boyunda, satır adımı genişlik * 4): MuPDF doğrudan içine çizer, kopya
// yok. Beyaz opak zemin: alfa her yerde 255 kalır, MuPDF'in önceden çarpılmış alfası düz alfayla aynıdır.
bool mupdf_render(fz_context* ctx, fz_page* page, float scale, fz_irect box, unsigned char* samples,
                  Failure* failure) noexcept {
    fz_pixmap* pixmap = nullptr;
    fz_device* device = nullptr;
    fz_var(pixmap);
    fz_var(device);
    fz_try(ctx) {
        pixmap = fz_new_pixmap_with_bbox_and_data(ctx, fz_device_rgb(ctx), box, nullptr, 1, samples);
        fz_clear_pixmap_with_value(ctx, pixmap, 0xFF);
        device = fz_new_draw_device(ctx, fz_scale(scale, scale), pixmap);
        fz_run_page(ctx, page, device, fz_identity, nullptr);
        fz_close_device(ctx, device);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, device);
        fz_drop_pixmap(ctx, pixmap);
    }
    fz_catch(ctx) {
        record_failure(ctx, failure);
        return false;
    }
    return true;
}

// ---- C++ tarafı ----

// MuPDF nesnelerinin sahipliği: mupdf_* döndükten sonra kurulur (fz_try dışında). fz_drop_* atmaz, NULL kabul eder.
struct BufferDrop {
    fz_context* ctx;
    void operator()(fz_buffer* buffer) const noexcept { fz_drop_buffer(ctx, buffer); }
};

struct PageDrop {
    fz_context* ctx;
    void operator()(fz_page* page) const noexcept { fz_drop_page(ctx, page); }
};

constexpr int kBytesPerPixel = 4;

std::string page_label(int index) {
    return std::to_string(index + 1);  // kullanıcıya 1'den sayılır
}

core::Error out_of_memory(const core::PdfLimits& limits) {
    return core::Error{core::Error::Code::LimitExceeded, "error.pdf_out_of_memory",
                       {{"limit", limits.context_memory.text}}};
}

// MuPDF iletisi kullanıcıya gitmez (belge baytı ya da kaçış dizisi içerebilir); teşhis ayrıntısı olarak işçiden
// ebeveyne gider, ebeveyn core::terminal_safe'ten geçirip loglar. 256 baytta yarım kalmış son UTF-8 dizisi atılır.
std::string failure_detail(const Failure& failure) {
    std::string_view message(failure.message.data());
    std::size_t lead = message.size();
    for (std::size_t back = 0; back < 3 && lead > 0 && (static_cast<unsigned char>(message[lead - 1]) & 0xC0U) == 0x80;
         ++back)
        --lead;
    if (lead > 0) {
        std::size_t length = 1;
        if (!detail::decode_one(message.substr(lead - 1), length) &&
            static_cast<unsigned char>(message[lead - 1]) >= 0xC0)
            message = message.substr(0, lead - 1);  // kesik son dizi
    }
    return std::string(message);
}

}  // namespace

struct PdfEngine::Impl {
    Impl(std::string_view source, const core::PdfLimits& pdf_limits) : bytes(source), limits(pdf_limits) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    // Sıra önemli: belge akışa, akış `bytes`'a, hepsi bağlama, bağlam `budget`'a dayanır.
    ~Impl() {
        if (ctx == nullptr)
            return;
        if (opened.doc != nullptr)
            pdf_drop_document(ctx, opened.doc);
        fz_drop_stream(ctx, opened.stream);
        fz_drop_context(ctx);
    }

    // Her işlem (open, page_text, render_page) kendi bütçe reddini görür: önceki işlemin reddi sonrakinin hatasını
    // bellek hatası saydırmaz.
    void begin_operation() noexcept { budget.refused = false; }

    // MuPDF hatasını core::Error'a çevirir: bütçe bu işlemde reddettiyse ve hata ayırma hatasıysa bellek sınırı,
    // değilse bozuk belge (ileti last_detail'e).
    core::Error failure_error(const Failure& failure, int page) {
        last_detail = failure_detail(failure);
        if (detail::is_budget_failure(failure.code, budget.refused))
            return out_of_memory(limits);
        if (page < 0)
            return core::Error{core::Error::Code::InvalidDocument, "error.pdf_invalid", {}};
        return core::Error{core::Error::Code::InvalidDocument, "error.pdf_page_invalid", {{"page", page_label(page)}}};
    }

    void require_page(int index) const {
        if (index < 0 || index >= opened.pages)
            std::abort();  // ön koşul ihlali bug'dır; MuPDF'e geçersiz sayfa verilmez
    }

    std::string bytes;  // fz_open_memory kopyalamaz: belge yaşadıkça burada durur
    core::PdfLimits limits;
    detail::MemoryBudget budget;
    fz_context* ctx = nullptr;
    OpenedPdf opened;
    std::string last_detail;  // son MuPDF hatasının iletisi (yalnız teşhis)
};

PdfEngine::PdfEngine(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PdfEngine::PdfEngine(PdfEngine&&) noexcept = default;
PdfEngine& PdfEngine::operator=(PdfEngine&&) noexcept = default;
PdfEngine::~PdfEngine() = default;

std::expected<PdfEngine, core::Error> PdfEngine::open(std::string_view bytes, const core::PdfLimits& limits,
                                                     std::string* failure_detail) {
    auto impl = std::make_unique<Impl>(bytes, limits);
    const fz_alloc_context allocator{&impl->budget, detail::budget_malloc, detail::budget_realloc,
                                     detail::budget_free};
    const fz_locks_context locks{nullptr, lock_shared, unlock_shared};
    const std::size_t memory_limit = limits.context_memory.cap();
    // Önbellek (fz_store) bütçeyi tek başına doldurmasın diye en çok bütçe kadar; MuPDF yapıları kopyalar.
    const std::size_t store = std::min<std::size_t>(FZ_STORE_DEFAULT, memory_limit);
    // Bağlam kurulurken bütçe açık: kurulum hatası, sessiz geri çağırma henüz takılamadan stderr'e yazılırdı
    // (TUI ekranını bozar). Kurulumun ayırması sabit boydadır (~yüz KB'ler), yine `used`'a sayılır; sınır hemen
    // ardından kurulur ve aşılmışsa belge açılmaz.
    impl->ctx = fz_new_context(&allocator, &locks, store);
    if (impl->ctx == nullptr)
        return std::unexpected(out_of_memory(limits));  // sistem belleği yetmedi
    fz_set_error_callback(impl->ctx, ignore_message, nullptr);
    fz_set_warning_callback(impl->ctx, ignore_message, nullptr);
    impl->budget.limit = memory_limit;
    if (impl->budget.used > memory_limit)
        return std::unexpected(out_of_memory(limits));

    impl->begin_operation();
    Failure failure;
    const auto* data = reinterpret_cast<const unsigned char*>(impl->bytes.data());
    if (!mupdf_open(impl->ctx, data, impl->bytes.size(), &impl->opened, &failure)) {
        core::Error error = impl->failure_error(failure, -1);
        if (failure_detail != nullptr)
            *failure_detail = impl->last_detail;
        return std::unexpected(std::move(error));
    }
    if (impl->opened.needs_password != 0)
        return std::unexpected(core::Error{core::Error::Code::Encrypted, "error.pdf_encrypted", {}});
    if (!limits.pages.allows(static_cast<std::uint64_t>(impl->opened.pages)))
        return std::unexpected(core::Error{core::Error::Code::LimitExceeded, "error.pdf_too_many_pages",
                                           {{"count", std::to_string(impl->opened.pages)},
                                            {"limit", limits.pages.text}}});
    return PdfEngine(std::move(impl));
}

std::string_view PdfEngine::last_failure_detail() const noexcept {
    return impl_->last_detail;
}

int PdfEngine::page_count() const noexcept {
    return impl_->opened.pages;
}

std::expected<std::string, core::Error> PdfEngine::page_text(int page) {
    Impl& impl = *impl_;
    impl.require_page(page);
    impl.begin_operation();
    fz_buffer* raw_buffer = nullptr;
    Failure failure;
    const bool extracted = mupdf_page_text(impl.ctx, impl.opened.doc, page, &raw_buffer, &failure);
    const std::unique_ptr<fz_buffer, BufferDrop> buffer(raw_buffer, BufferDrop{impl.ctx});
    if (!extracted)
        return std::unexpected(impl.failure_error(failure, page));

    unsigned char* data = nullptr;
    const std::size_t size = fz_buffer_storage(impl.ctx, buffer.get(), &data);  // atmaz: yalnız alanları okur
    const auto too_large = [&](std::size_t bytes) {
        return core::Error{core::Error::Code::LimitExceeded, "error.pdf_text_too_large",
                           {{"page", page_label(page)},
                            {"size", std::to_string(bytes)},
                            {"limit", impl.limits.text_bytes.text}}};
    };
    // Önce ham boy: temizlik kopyası (en çok 3 katı, bütçe dışında) sınırı aşan metin için hiç kurulmaz.
    if (!impl.limits.text_bytes.allows(size))
        return std::unexpected(too_large(size));
    std::string text = detail::clean_text(std::string_view(reinterpret_cast<const char*>(data), size));
    if (!impl.limits.text_bytes.allows(text.size()))
        return std::unexpected(too_large(text.size()));  // geçersiz baytlar U+FFFD olunca büyüyebilir
    return text;
}

std::expected<RenderedPage, core::Error> PdfEngine::render_page(int page, float scale) {
    Impl& impl = *impl_;
    impl.require_page(page);
    if (!std::isfinite(scale) || !(scale > 0.0F))
        std::abort();  // ön koşul: NaN/sonsuz ölçek MuPDF'in tam sayı yuvarlamasında tanımsız davranıştır
    impl.begin_operation();

    fz_page* raw_page = nullptr;
    fz_irect box{};
    Failure failure;
    const bool loaded = mupdf_page_box(impl.ctx, impl.opened.doc, page, scale, &raw_page, &box, &failure);
    const std::unique_ptr<fz_page, PageDrop> loaded_page(raw_page, PageDrop{impl.ctx});
    if (!loaded)
        return std::unexpected(impl.failure_error(failure, page));

    const int width = box.x1 - box.x0;
    const int height = box.y1 - box.y0;
    // MuPDF boş ya da 1 pt'den dar kutuyu kendisi büyütür (pdf-page.c); bu yalnız aşırı küçük ölçekte olur.
    if (width <= 0 || height <= 0)
        return RenderedPage{};
    const auto pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (!impl.limits.render_pixels.allows(pixels))
        return std::unexpected(core::Error{core::Error::Code::LimitExceeded, "error.pdf_render_too_large",
                                           {{"page", page_label(page)},
                                            {"width", std::to_string(width)},
                                            {"height", std::to_string(height)},
                                            {"limit", impl.limits.render_pixels.text}}});

    RenderedPage rendered{.width = width, .height = height, .rgba = {}};
    const auto no_memory = [&] {
        return core::Error{core::Error::Code::LimitExceeded, "error.pdf_no_memory",
                           {{"page", page_label(page)},
                            {"width", std::to_string(width)},
                            {"height", std::to_string(height)}}};
    };
    if (pixels > rendered.rgba.max_size() / kBytesPerPixel)
        return std::unexpected(no_memory());
    try {
        rendered.rgba.resize(static_cast<std::size_t>(pixels) * kBytesPerPixel);
    } catch (const std::bad_alloc&) {
        // render_pixels = 0 (sınırsız) iken dev sayfa: süreç düşmez, çizim yapılmaz.
        return std::unexpected(no_memory());
    }

    if (!mupdf_render(impl.ctx, loaded_page.get(), scale, box, rendered.rgba.data(), &failure))
        return std::unexpected(impl.failure_error(failure, page));
    return rendered;
}

}  // namespace noteapp::pdfview
