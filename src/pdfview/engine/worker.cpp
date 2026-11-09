#include "pdfview/engine/worker.hpp"

#include "pdfview/engine/pdf_engine.hpp"
#include "pdfview/protocol.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <chrono>
#include <thread>
#include <unistd.h>
#endif

namespace noteapp::pdfview {

namespace {

namespace proto = protocol;

enum Exit : int { kClean = 0, kTerminal = 2, kBadRequest = 3, kNoMemory = 4, kWriteFailed = 5 };

bool stdin_is_terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

// Ebeveyn öldüğünde (yeniden ebeveynlenme) işçi kendiliğinden çıkar; PR_SET_PDEATHSIG değil: o, işçiyi başlatan
// iş parçacığı bitince de tetiklenir. Windows'ta ebeveynin KILL_ON_JOB_CLOSE iş nesnesi bu işi görür.
void watch_parent([[maybe_unused]] std::uint64_t parent_pid) {
#ifndef _WIN32
    if (parent_pid == 0)
        return;
    std::thread([parent_pid] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (static_cast<std::uint64_t>(getppid()) != parent_pid)
                _exit(kClean);
        }
    }).detach();
#endif
}

// 0: EOF (hiç bayt yok), 1: tam, -1: yarıda kesildi.
int read_exact(std::string& out, std::size_t size) {
    out.resize(size);
    std::size_t got = 0;
    while (got < size) {
        const std::size_t n = std::fread(out.data() + got, 1, size - got, stdin);
        if (n == 0)
            return got == 0 ? 0 : -1;
        got += n;
    }
    return 1;
}

bool write_all(std::string_view data) {
    return std::fwrite(data.data(), 1, data.size(), stdout) == data.size();
}

std::uint64_t number_arg(const core::Error& error, std::string_view name) {
    for (const auto& [key, value] : error.args) {
        if (key != name)
            continue;
        std::uint64_t number = 0;
        std::from_chars(value.data(), value.data() + value.size(), number);
        return number;
    }
    return 0;
}

// Motorun hatasını tel nedenine çevirir; anahtar ve metin karşıya gitmez, ebeveyn kendi değerleriyle kurar.
proto::Failure to_failure(const core::Error& error, std::string_view detail) {
    const std::string_view key = error.message_key;
    proto::Failure failure{.reason = proto::FailReason::Invalid, .a = 0, .b = 0, .detail = std::string(detail)};
    if (key == "error.pdf_encrypted") {
        failure.reason = proto::FailReason::Encrypted;
    } else if (key == "error.pdf_too_many_pages") {
        failure.reason = proto::FailReason::TooManyPages;
        failure.a = number_arg(error, "count");
    } else if (key == "error.pdf_render_too_large") {
        failure.reason = proto::FailReason::RenderTooLarge;
        failure.a = number_arg(error, "width");
        failure.b = number_arg(error, "height");
    } else if (key == "error.pdf_text_too_large") {
        failure.reason = proto::FailReason::TextTooLarge;
        failure.a = number_arg(error, "size");
    } else if (key == "error.pdf_out_of_memory") {
        failure.reason = proto::FailReason::OutOfMemory;
    } else if (key == "error.pdf_no_memory") {
        failure.reason = proto::FailReason::NoMemory;
        failure.a = number_arg(error, "width");
        failure.b = number_arg(error, "height");
    }
    return failure;
}

core::Limit limit_of(std::uint64_t value) {
    return core::Limit{value, std::to_string(value)};
}

int serve() {
    std::optional<PdfEngine> engine;
    std::string header_bytes;
    std::string payload;
    for (;;) {
        const int got = read_exact(header_bytes, proto::kHeaderBytes);
        if (got == 0)
            return kClean;
        const std::optional<proto::Header> header = got < 0 ? std::nullopt : proto::decode_header(header_bytes);
        if (!header || header->length > payload.max_size())
            return kBadRequest;
        // Başlıktan hemen sonra gelen EOF da bozuk istektir (yük sıfırlarla doldurulup işlenmez).
        if (read_exact(payload, static_cast<std::size_t>(header->length)) != 1)
            return kBadRequest;
        std::string reply;
        switch (header->kind) {
        case proto::Kind::Open: {
            const std::optional<proto::OpenFields> fields = proto::decode_open(payload);
            if (!fields)
                return kBadRequest;
            if (!engine)
                watch_parent(fields->parent_pid);
            const core::PdfLimits limits{.pages = limit_of(fields->pages),
                                         .render_pixels = limit_of(fields->render_pixels),
                                         .text_bytes = limit_of(fields->text_bytes),
                                         .context_memory = limit_of(fields->context_memory),
                                         .duration = core::Limit{}};
            engine.reset();
            std::string detail;
            // Süre ve iptal motorda yok: ebeveyn tutar ve gerekirse bu süreci öldürür.
            auto opened = PdfEngine::open(std::string_view(payload).substr(proto::kOpenFieldBytes), limits, &detail);
            if (opened) {
                engine.emplace(std::move(*opened));
                reply = proto::encode_opened(static_cast<std::uint32_t>(engine->page_count()));
            } else {
                reply = proto::encode_failed(to_failure(opened.error(), detail));
            }
            break;
        }
        case proto::Kind::Text: {
            const std::optional<std::uint32_t> page = proto::decode_text_request(payload);
            if (!engine || !page || *page >= static_cast<std::uint32_t>(engine->page_count()))
                return kBadRequest;
            const auto text = engine->page_text(static_cast<int>(*page));
            reply = text ? proto::encode_text_result(*text)
                         : proto::encode_failed(to_failure(text.error(), engine->last_failure_detail()));
            break;
        }
        case proto::Kind::Render: {
            const std::optional<proto::RenderRequest> request = proto::decode_render_request(payload);
            if (!engine || !request || request->page >= static_cast<std::uint32_t>(engine->page_count()) ||
                !std::isfinite(request->scale) || !(request->scale > 0.0F))
                return kBadRequest;
            const auto rendered = engine->render_page(static_cast<int>(request->page), request->scale);
            if (!rendered) {
                reply = proto::encode_failed(to_failure(rendered.error(), engine->last_failure_detail()));
                break;
            }
            // Dev RGBA ikinci kez kopyalanmaz: başlık ve alanlar ayrı, pikseller doğrudan yazılır.
            const auto width = static_cast<std::uint32_t>(rendered->width);
            const auto height = static_cast<std::uint32_t>(rendered->height);
            const std::string_view rgba(reinterpret_cast<const char*>(rendered->rgba.data()), rendered->rgba.size());
            std::string head = proto::encode_header(proto::Kind::Rendered, proto::kRenderedFieldBytes + rgba.size());
            proto::put_u32(head, width);
            proto::put_u32(head, height);
            if (!write_all(head) || !write_all(rgba) || std::fflush(stdout) != 0)
                return kWriteFailed;
            continue;
        }
        case proto::Kind::Opened:
        case proto::Kind::TextResult:
        case proto::Kind::Rendered:
        case proto::Kind::Failed:
            return kBadRequest;
        }
        if (!write_all(reply) || std::fflush(stdout) != 0)
            return kWriteFailed;
    }
}

}  // namespace

int run_pdf_worker() {
    if (stdin_is_terminal())
        return kTerminal;
    try {
        return serve();
    } catch (const std::bad_alloc&) {
        return kNoMemory;
    }
}

}  // namespace noteapp::pdfview
