#include "pdfview/pdf_document.hpp"

#include "core/child_process.hpp"
#include "core/log.hpp"
#include "core/text.hpp"
#include "pdfview/page_text.hpp"
#include "pdfview/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace noteapp::pdfview {

namespace {

using Clock = core::ChildProcess::Clock;
namespace proto = protocol;

constexpr std::string_view kWorkerCommand = "__pdf-worker";
// Okuma bu dilimlerle beklenir: durdurma ve süre en geç bu kadar gecikmeyle fark edilir (yazma da 50 ms yoklar).
constexpr auto kSlice = std::chrono::milliseconds(50);
// Bu kadar uzun süre fiilen sınırsızdır; daha büyüğü now() + süre'yi taşırırdı.
constexpr std::uint64_t kMaxDurationMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max()).count() / 2);
// Kapanışta işçinin stdin EOF'unda kendiliğinden çıkması için beklenen süre; sonra öldürülür.
constexpr auto kExitGrace = std::chrono::milliseconds(200);
// Çıktısını kapatan (çöken) işçinin çıkış kodu loga en çok bu kadar beklenir; süreç çoğunlukla
// çoktan bitmiştir.
constexpr auto kExitCodeWait = std::chrono::milliseconds(100);

// İşçiyle tek alışverişin sonucu. Ok dışındakiler işçiyi öldürtür. Eof: işçi çıktısını kapattı (çöktü
// ya da çıktı); Broken: bozuk çerçeve ya da G/Ç hatası, işçi canlı olabilir.
enum class Io { Ok, Deadline, Stopped, Eof, Broken, NoMemory };

enum class Operation { Open, Text, Structured, Render };

std::string_view operation_name(Operation operation) {
    switch (operation) {
    case Operation::Open:
        return "open";
    case Operation::Text:
        return "page_text";
    case Operation::Structured:
        return "page_structure";
    case Operation::Render:
        return "render_page";
    }
    return "?";
}

std::string_view io_name(Io io) {
    switch (io) {
    case Io::Ok:
        return "ok";
    case Io::Deadline:
        return "deadline";
    case Io::Stopped:
        return "stopped";
    case Io::Eof:
        return "eof";
    case Io::Broken:
        return "broken";
    case Io::NoMemory:
        return "no_memory";
    }
    return "?";
}

std::string page_label(int page) {
    return page < 0 ? std::string("-") : std::to_string(page + 1);  // kullanıcıya 1'den sayılır
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

std::uint64_t own_pid() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

Clock::time_point deadline_for(const core::Limit& duration) {
    if (duration.unlimited() || duration.value >= kMaxDurationMs)
        return Clock::time_point::max();
    return Clock::now() + std::chrono::milliseconds(duration.value);
}

// 256 baytta yarım kalmış son UTF-8 dizisini atar; kalan core::terminal_safe'ten geçer (log satırına kaçış dizisi ya
// da satır sonu girmez).
std::string log_safe(std::string_view text) {
    std::size_t end = text.size();
    for (std::size_t back = 0; back < 3 && end > 0 && (static_cast<unsigned char>(text[end - 1]) & 0xC0U) == 0x80;
         ++back)
        --end;
    if (end > 0 && static_cast<unsigned char>(text[end - 1]) >= 0xC0) {
        std::size_t length = 1;
        if (!detail::decode_one(text.substr(end - 1), length))
            text = text.substr(0, end - 1);
    }
    return core::terminal_safe(text);
}

}  // namespace

struct PdfDocument::Impl {
    Impl(std::string_view source, const core::PdfLimits& pdf_limits, std::filesystem::path worker_path)
        : bytes(source), limits(pdf_limits), worker(std::move(worker_path)) {}
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        if (!child)
            return;
        child->close_stdin();  // işçi stdin EOF'unda 0 ile çıkar
        (void)child->wait_until(Clock::now() + kExitGrace);
        // ChildProcess'in yıkıcısı bitmemişse öldürür ve toplar.
    }

    // İşçiyi öldürür ve toplar; nedeni (ve çöktüyse çıkış kodunu) loglar. Belge geçerli kalır.
    void drop_worker(Io why, Operation operation, int page) {
        std::string exit_code = "-";
        if (child) {
            if (why == Io::Eof) {  // canlı işçiyi beklemek çağıranı (ve durdurmayı) bekletirdi
                if (const auto code = child->wait_until(Clock::now() + kExitCodeWait))
                    exit_code = std::to_string(*code);
            }
            child->kill();
            child.reset();
        }
        inbox.clear();
        core::log_warn(std::format("event=pdf_worker_failed operation={} page={} exit_code={} reason={}",
                                   operation_name(operation), page_label(page), exit_code, io_name(why)));
    }

    core::Error io_error(Io why, Operation operation, int page) const {
        switch (why) {
        case Io::Deadline:
            return core::Error{core::Error::Code::LimitExceeded, "error.pdf_too_slow", {{"limit", limits.duration.text}}};
        case Io::Stopped:
            return core::Error{core::Error::Code::Cancelled, "error.pdf_cancelled", {}};
        case Io::Ok:
        case Io::Eof:
        case Io::Broken:
        case Io::NoMemory:
            break;
        }
        if (operation == Operation::Open)
            return core::Error{core::Error::Code::InvalidDocument, "error.pdf_invalid", {}};
        return core::Error{core::Error::Code::InvalidDocument, "error.pdf_page_invalid", {{"page", page_label(page)}}};
    }

    // İşçinin iş hatası (işçi canlı kalır): anahtar ve argümanlar bu sürecin değerleriyle kurulur.
    core::Error failure_error(const proto::Failure& failure, Operation operation, int page) const {
        if (!failure.detail.empty())
            core::log_warn(std::format("event=pdf_worker_error operation={} page={} reason={} detail={}",
                                       operation_name(operation), page_label(page),
                                       static_cast<std::uint32_t>(failure.reason), log_safe(failure.detail)));
        using Code = core::Error::Code;
        const std::string a = std::to_string(failure.a);
        const std::string b = std::to_string(failure.b);
        switch (failure.reason) {
        case proto::FailReason::Invalid:
            break;
        case proto::FailReason::Encrypted:
            return core::Error{Code::Encrypted, "error.pdf_encrypted", {}};
        case proto::FailReason::TooManyPages:
            return core::Error{Code::LimitExceeded, "error.pdf_too_many_pages",
                               {{"count", a}, {"limit", limits.pages.text}}};
        case proto::FailReason::RenderTooLarge:
            return core::Error{Code::LimitExceeded,
                               "error.pdf_render_too_large",
                               {{"page", page_label(page)}, {"width", a}, {"height", b}, {"limit", limits.render_pixels.text}}};
        case proto::FailReason::TextTooLarge:
            return core::Error{Code::LimitExceeded, "error.pdf_text_too_large",
                               {{"page", page_label(page)}, {"size", a}, {"limit", limits.text_bytes.text}}};
        case proto::FailReason::OutOfMemory:
            return core::Error{Code::LimitExceeded, "error.pdf_out_of_memory", {{"limit", limits.context_memory.text}}};
        case proto::FailReason::NoMemory:
            return core::Error{Code::LimitExceeded, "error.pdf_no_memory",
                               {{"page", page_label(page)}, {"width", a}, {"height", b}}};
        }
        return io_error(Io::Broken, operation, page);  // Invalid: bozuk belge/sayfa
    }

    Io write(std::string_view data, Clock::time_point deadline, const std::stop_token& stop) {
        if (child->write_all(data, deadline, stop))
            return Io::Ok;
        if (stop.stop_requested())
            return Io::Stopped;
        return Clock::now() >= deadline ? Io::Deadline : Io::Broken;
    }

    // Tam `size` baytı `out`un sonuna ekler; önce önceki okumanın artığı tüketilir. Tampon gelen veriyle
    // büyür, işçinin bildirdiği uzunlukla değil: sınırsız (0) limitte yalan uzunluk bayt gelmeden bellek
    // ayırtmaz.
    template <class Buffer>
    Io read_append(Buffer& out, std::size_t size, Clock::time_point deadline, const std::stop_token& stop) {
        const auto append = [&out](const char* data, std::size_t count) {
            const auto* first = reinterpret_cast<const typename Buffer::value_type*>(data);
            out.insert(out.end(), first, first + count);
        };
        std::size_t got = std::min(size, inbox.size());
        append(inbox.data(), got);
        inbox.erase(0, got);
        std::string chunk;
        while (got < size) {
            if (stop.stop_requested())
                return Io::Stopped;
            const auto now = Clock::now();
            if (now >= deadline)
                return Io::Deadline;
            chunk.clear();
            const auto status = child->read_some(chunk, std::min(deadline, now + kSlice));
            if (status == core::ChildProcess::ReadStatus::Eof)
                return Io::Eof;
            if (status == core::ChildProcess::ReadStatus::Failed)
                return Io::Broken;
            const std::size_t take = std::min(size - got, chunk.size());
            append(chunk.data(), take);
            got += take;
            inbox.append(chunk, take, std::string::npos);
        }
        return Io::Ok;
    }

    Io read_string(std::string& out, std::size_t size, Clock::time_point deadline, const std::stop_token& stop) {
        out.clear();
        return read_append(out, size, deadline, stop);
    }

    // Cevap çerçevesi: beklenen tür ya da Failed. Uzunluklar okunmadan önce denetlenir (bozuk çerçeve = Broken).
    struct Reply {
        proto::Kind kind{};
        std::uint32_t page_count = 0;
        std::string text;
        PageText structured;
        RenderedPage rendered;
        proto::Failure failure;
        std::uint32_t width = 0;  // NoMemory için
        std::uint32_t height = 0;
    };

    Io read_reply(proto::Kind expected, Reply& reply, Clock::time_point deadline, const std::stop_token& stop) {
        std::string header_bytes;
        if (const Io io = read_string(header_bytes, proto::kHeaderBytes, deadline, stop); io != Io::Ok)
            return io;
        const std::optional<proto::Header> header = proto::decode_header(header_bytes);
        if (!header)
            return Io::Broken;
        reply.kind = header->kind;
        std::string payload;
        if (header->kind == proto::Kind::Failed) {
            if (header->length < proto::kFailedFieldBytes ||
                header->length > proto::kFailedFieldBytes + proto::kMaxFailedDetail)
                return Io::Broken;
            if (const Io io = read_string(payload, header->length, deadline, stop); io != Io::Ok)
                return io;
            std::optional<proto::Failure> failure = proto::decode_failed(payload);
            if (!failure)
                return Io::Broken;
            reply.failure = std::move(*failure);
            return Io::Ok;
        }
        if (header->kind != expected)
            return Io::Broken;
        switch (expected) {
        case proto::Kind::Opened: {
            if (header->length != 4)
                return Io::Broken;
            if (const Io io = read_string(payload, 4, deadline, stop); io != Io::Ok)
                return io;
            reply.page_count = proto::get_u32(payload, 0);
            if (reply.page_count > limits.pages.cap() || reply.page_count > INT_MAX)
                return Io::Broken;
            return Io::Ok;
        }
        case proto::Kind::TextResult: {
            if (header->length > limits.text_bytes.cap() || header->length > reply.text.max_size())
                return Io::Broken;
            try {
                return read_string(reply.text, static_cast<std::size_t>(header->length), deadline, stop);
            } catch (const std::bad_alloc&) {
                return Io::Broken;  // text_bytes = 0 (sınırsız) iken dev metin gönderildi
            }
        }
        case proto::Kind::StructuredResult: {
            // Yük metinden büyüktür (kutular) ama sınırı text_bytes'tan türer; bayt geldikçe büyür.
            if (header->length > proto::structured_length_cap(limits.text_bytes.cap()) ||
                header->length > payload.max_size())
                return Io::Broken;
            try {
                if (const Io io = read_string(payload, static_cast<std::size_t>(header->length), deadline, stop);
                    io != Io::Ok)
                    return io;
                std::optional<PageText> page = proto::decode_structured(payload, limits.text_bytes.cap());
                if (!page)
                    return Io::Broken;
                reply.structured = std::move(*page);
            } catch (const std::bad_alloc&) {
                return Io::Broken;  // text_bytes = 0 (sınırsız) iken dev yük gönderildi
            }
            return Io::Ok;
        }
        case proto::Kind::Rendered: {
            if (header->length < proto::kRenderedFieldBytes)
                return Io::Broken;
            if (const Io io = read_string(payload, proto::kRenderedFieldBytes, deadline, stop); io != Io::Ok)
                return io;
            reply.width = proto::get_u32(payload, 0);
            reply.height = proto::get_u32(payload, 4);
            const std::uint64_t pixels = static_cast<std::uint64_t>(reply.width) * reply.height;
            const std::optional<std::uint64_t> length = proto::rendered_length(reply.width, reply.height);
            if (!length || *length != header->length || pixels > limits.render_pixels.cap() ||
                reply.width > INT_MAX || reply.height > INT_MAX)
                return Io::Broken;
            const std::uint64_t bytes = header->length - proto::kRenderedFieldBytes;
            std::vector<std::uint8_t>& rgba = reply.rendered.rgba;
            if (bytes > rgba.max_size())
                return Io::NoMemory;
            reply.rendered.width = static_cast<int>(reply.width);
            reply.rendered.height = static_cast<int>(reply.height);
            try {
                return read_append(rgba, static_cast<std::size_t>(bytes), deadline, stop);
            } catch (const std::bad_alloc&) {
                return Io::NoMemory;  // render_pixels = 0 (sınırsız) iken dev sayfa gönderildi
            } catch (const std::length_error&) {
                return Io::NoMemory;
            }
        }
        case proto::Kind::Open:
        case proto::Kind::Text:
        case proto::Kind::Structured:
        case proto::Kind::Render:
        case proto::Kind::Failed:
            break;
        }
        return Io::Broken;
    }

    // İşçi yoksa başlatır ve belgeyi açtırır. İlk açılış sayfa sayısını sabitler; yeniden başlatmada sayı aynı olmalı.
    std::optional<core::Error> ensure_worker(Operation operation, int page, Clock::time_point deadline,
                                             const std::stop_token& stop) {
        if (child)
            return std::nullopt;
        const proto::OpenFields fields{.version = proto::kVersion,
                                       .parent_pid = own_pid(),
                                       .pages = limits.pages.value,
                                       .render_pixels = limits.render_pixels.value,
                                       .text_bytes = limits.text_bytes.value,
                                       .context_memory = limits.context_memory.value};
        // Çerçeve başlatmadan önce kurulur: ayırma hatası açılmamış bir işçiyi açık saydırmasın.
        const std::string head = proto::encode_open_head(fields, bytes.size());
        const std::string program = utf8(worker);
        child = core::ChildProcess::start({program, std::string(kWorkerCommand)}, true);
        if (!child) {
            std::error_code ignored;
            if (!std::filesystem::exists(worker, ignored))
                return core::Error{core::Error::Code::NotFound, "error.process_not_found", {{"program", program}}};
            return core::Error{core::Error::Code::Io, "error.process_spawn_failed",
                               {{"program", program}, {"reason", "spawn_failed"}}};
        }
        Reply reply;
        Io io = write(head, deadline, stop);
        if (io == Io::Ok)
            io = write(bytes, deadline, stop);  // belge kopyalanmadan
        if (io == Io::Ok)
            io = read_reply(proto::Kind::Opened, reply, deadline, stop);
        if (io != Io::Ok) {
            drop_worker(io, operation, page);
            return io_error(io, Operation::Open, page);
        }
        if (reply.kind == proto::Kind::Failed) {
            core::Error error = failure_error(reply.failure, Operation::Open, -1);
            child.reset();  // belgesiz işçi tutulmaz (yıkıcı öldürüp toplar); iş hatası, loglanacak çöküş değil
            inbox.clear();
            return error;
        }
        const int count = static_cast<int>(reply.page_count);
        if (pages >= 0 && count != pages) {
            drop_worker(Io::Broken, operation, page);  // aynı baytlar başka sayfa sayısı verdi: işçiye güvenilmez
            return core::Error{core::Error::Code::InvalidDocument, "error.pdf_invalid", {}};
        }
        pages = count;
        return std::nullopt;
    }

    // İstek gönderip cevabı okur; G/Ç başarısızsa işçiyi düşürüp hatayı döner.
    std::optional<core::Error> transact(std::string_view request, proto::Kind expected, Operation operation, int page,
                                        Reply& reply, Clock::time_point deadline, const std::stop_token& stop) {
        if (auto error = ensure_worker(operation, page, deadline, stop))
            return error;
        Io io = write(request, deadline, stop);
        if (io == Io::Ok)
            io = read_reply(expected, reply, deadline, stop);
        if (io == Io::NoMemory) {
            drop_worker(io, operation, page);  // gelmekte olan RGBA okunmayacak: akış senkron değil
            return core::Error{core::Error::Code::LimitExceeded, "error.pdf_no_memory",
                               {{"page", page_label(page)},
                                {"width", std::to_string(reply.width)},
                                {"height", std::to_string(reply.height)}}};
        }
        if (io != Io::Ok) {
            drop_worker(io, operation, page);
            return io_error(io, operation, page);
        }
        if (reply.kind == proto::Kind::Failed)
            return failure_error(reply.failure, operation, page);
        return std::nullopt;
    }

    void require_page(int index) const {
        if (index < 0 || index >= pages)
            std::abort();  // ön koşul ihlali bug'dır
    }

    std::string bytes;  // yeniden başlatmada yeniden gönderilir
    core::PdfLimits limits;
    std::filesystem::path worker;
    std::optional<core::ChildProcess> child;
    std::string inbox;  // okunmuş ama tüketilmemiş bayt
    int pages = -1;     // ilk açılışta sabitlenir
};

PdfDocument::PdfDocument(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PdfDocument::PdfDocument(PdfDocument&&) noexcept = default;
PdfDocument& PdfDocument::operator=(PdfDocument&&) noexcept = default;
PdfDocument::~PdfDocument() = default;

std::expected<PdfDocument, core::Error> PdfDocument::open(std::string_view bytes, const core::PdfLimits& limits,
                                                         const std::filesystem::path& worker, std::stop_token stop) {
    auto impl = std::make_unique<Impl>(bytes, limits, worker);
    if (auto error = impl->ensure_worker(Operation::Open, -1, deadline_for(limits.duration), stop))
        return std::unexpected(std::move(*error));
    return PdfDocument(std::move(impl));
}

int PdfDocument::page_count() const noexcept {
    return impl_->pages;
}

std::expected<std::string, core::Error> PdfDocument::page_text(int page, std::stop_token stop) {
    Impl& impl = *impl_;
    impl.require_page(page);
    Impl::Reply reply;
    if (auto error = impl.transact(proto::encode_text_request(static_cast<std::uint32_t>(page)),
                                   proto::Kind::TextResult, Operation::Text, page, reply,
                                   deadline_for(impl.limits.duration), stop))
        return std::unexpected(std::move(*error));
    // İşçi temizlemiş olsa da bu süreç kendi kuralını yeniden uygular: işçi güvenilmez.
    std::string text = detail::clean_text(reply.text);
    if (!impl.limits.text_bytes.allows(text.size()))
        return std::unexpected(core::Error{core::Error::Code::LimitExceeded, "error.pdf_text_too_large",
                                           {{"page", page_label(page)},
                                            {"size", std::to_string(text.size())},
                                            {"limit", impl.limits.text_bytes.text}}});
    return text;
}

std::expected<PageText, core::Error> PdfDocument::page_structure(int page, std::stop_token stop) {
    Impl& impl = *impl_;
    impl.require_page(page);
    Impl::Reply reply;
    if (auto error = impl.transact(proto::encode_structured_request(static_cast<std::uint32_t>(page)),
                                   proto::Kind::StructuredResult, Operation::Structured, page, reply,
                                   deadline_for(impl.limits.duration), stop))
        return std::unexpected(std::move(*error));
    // Metin geçerli UTF-8 diye denetlendi; denetim karakterleri bu sürecin kuralıyla yeniden temizlenir. Her
    // karakter tek kod noktası kalır (U+FFFD), kenar sayısı bozulmaz.
    std::uint64_t size = 0;
    for (TextBlock& block : reply.structured.blocks)
        for (TextLine& line : block.lines)
            for (TextSpan& span : line.spans) {
                span.text = detail::clean_text(span.text);
                size += span.text.size();
            }
    if (!impl.limits.text_bytes.allows(size))
        return std::unexpected(core::Error{core::Error::Code::LimitExceeded, "error.pdf_text_too_large",
                                           {{"page", page_label(page)},
                                            {"size", std::to_string(size)},
                                            {"limit", impl.limits.text_bytes.text}}});
    return std::move(reply.structured);
}

std::expected<RenderedPage, core::Error> PdfDocument::render_page(int page, float scale, std::stop_token stop) {
    Impl& impl = *impl_;
    impl.require_page(page);
    if (!std::isfinite(scale) || !(scale > 0.0F))
        std::abort();  // ön koşul: işçi de geçersiz ölçeği reddeder
    Impl::Reply reply;
    if (auto error = impl.transact(proto::encode_render_request(static_cast<std::uint32_t>(page), scale),
                                   proto::Kind::Rendered, Operation::Render, page, reply,
                                   deadline_for(impl.limits.duration), stop))
        return std::unexpected(std::move(*error));
    return std::move(reply.rendered);
}

}  // namespace noteapp::pdfview
