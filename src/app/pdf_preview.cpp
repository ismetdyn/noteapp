#include "app/pdf_preview.hpp"

#include "core/i18n.hpp"
#include "core/log.hpp"
#include "pdfview/pdf_document.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <ranges>

namespace noteapp::app {

namespace {

bool is_cancelled(const core::Error& error) {
    return error.code == core::Error::Code::Cancelled;
}

}  // namespace

bool is_pdf_type(std::string_view extension) {
    return std::ranges::equal(extension, std::string_view("pdf"), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
    });
}

std::string pdf_page_separator(int page, int count) {
    return core::tr("app.pdf_page_separator", {{"page", std::to_string(page + 1)}, {"count", std::to_string(count)}});
}

std::expected<void, core::Error> read_pdf_text(std::string_view bytes, const core::PdfLimits& limits,
                                               const std::filesystem::path& worker, std::stop_token stop,
                                               const PdfPageSink& page_done) {
    auto document = pdfview::PdfDocument::open(bytes, limits, worker, stop);
    if (!document)
        return std::unexpected(std::move(document.error()));
    const int count = document->page_count();
    for (int page = 0; page < count; ++page) {
        const auto text = document->page_text(page, stop);
        if (!text && is_cancelled(text.error()))
            return std::unexpected(text.error());
        page_done(page, count, text);
    }
    return {};
}

PdfPreview::PdfPreview(std::string bytes, PdfPreviewOptions options)
    : options_(std::move(options)),
      worker_([this, bytes = std::move(bytes)](const std::stop_token& stop) mutable { run(std::move(bytes), stop); }) {}

// worker_ son üyedir, ilk yıkılır: jthread durdurur (yürüyen PdfDocument çağrısı işçi sürecini öldürür) ve bekler;
// diğer üyeler o ana dek yaşar.
PdfPreview::~PdfPreview() = default;

PdfPreview::State PdfPreview::state() const {
    const std::scoped_lock lock(mutex_);
    return state_;
}

int PdfPreview::page_count() const {
    const std::scoped_lock lock(mutex_);
    return page_count_;
}

std::optional<core::Error> PdfPreview::open_error() const {
    const std::scoped_lock lock(mutex_);
    return open_error_;
}

std::uint64_t PdfPreview::version() const {
    const std::scoped_lock lock(mutex_);
    return version_;
}

std::size_t PdfPreview::pending() const {
    const std::scoped_lock lock(mutex_);
    return queue_.size();
}

void PdfPreview::request_text(int page) {
    request({Kind::Text, page, 0.0F});
}

void PdfPreview::request_render(int page, float scale) {
    request({Kind::Render, page, scale});
}

void PdfPreview::request(Request request) {
    {
        const std::scoped_lock lock(mutex_);
        if (state_ == State::Failed || request.page < 0 ||
            (state_ == State::Ready && request.page >= page_count_) || known(request))
            return;
        queue_.push_back(request);
    }
    wake_.notify_one();
}

bool PdfPreview::known(const Request& request) const {
    if (in_flight_ == request || std::ranges::find(queue_, request) != queue_.end())
        return true;
    if (request.kind == Kind::Text)
        return texts_.contains(request.page);
    return render_errors_.contains({request.page, request.scale}) ||
           std::ranges::any_of(rendered_, [&](const Cached& cached) {
               return cached.page == request.page && cached.scale == request.scale;
           });
}

void PdfPreview::drop_pending() {
    const std::scoped_lock lock(mutex_);
    queue_.clear();
}

std::optional<PdfPreview::Text> PdfPreview::text(int page) const {
    const std::scoped_lock lock(mutex_);
    const auto found = texts_.find(page);
    return found == texts_.end() ? std::nullopt : std::optional<Text>(found->second);
}

std::optional<PdfPreview::Text> PdfPreview::take_text(int page) {
    const std::scoped_lock lock(mutex_);
    auto found = texts_.find(page);
    if (found == texts_.end())
        return std::nullopt;
    Text taken = std::move(found->second);
    texts_.erase(found);
    return taken;
}

std::optional<PdfPreview::Page> PdfPreview::render(int page, float scale) {
    const std::scoped_lock lock(mutex_);
    if (const auto failed = render_errors_.find({page, scale}); failed != render_errors_.end())
        return std::unexpected(failed->second);
    const auto found = std::ranges::find_if(
        rendered_, [&](const Cached& cached) { return cached.page == page && cached.scale == scale; });
    if (found == rendered_.end())
        return std::nullopt;
    rendered_.splice(rendered_.begin(), rendered_, found);
    return rendered_.front().rendered;
}

void PdfPreview::store_render(const Request& request, Page result) {
    if (!result) {
        render_errors_.insert_or_assign({request.page, request.scale}, std::move(result.error()));
        return;
    }
    rendered_.push_front({request.page, request.scale, std::move(*result)});
    while (rendered_.size() > options_.limits.cache_pages.cap())
        rendered_.pop_back();
}

void PdfPreview::run(std::string bytes, const std::stop_token& stop) {
    try {
        auto document = pdfview::PdfDocument::open(bytes, options_.limits, options_.worker, stop);
        bytes = std::string();  // belge kendi kopyasını tutar
        {
            const std::scoped_lock lock(mutex_);
            if (!document && is_cancelled(document.error()))
                return;
            if (document) {
                state_ = State::Ready;
                page_count_ = document->page_count();
                std::erase_if(queue_, [&](const Request& queued) { return queued.page >= page_count_; });
            }
            else {
                state_ = State::Failed;
                open_error_ = std::move(document.error());
                queue_.clear();
            }
            ++version_;
        }
        notify();
        if (!document)
            return;
        while (true) {
            Request next;
            {
                std::unique_lock lock(mutex_);
                if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); }))
                    return;
                next = queue_.front();
                queue_.pop_front();
                in_flight_ = next;
            }
            if (next.kind == Kind::Text) {
                auto text = document->page_text(next.page, stop);
                if (!text && is_cancelled(text.error()))
                    return;
                const std::scoped_lock lock(mutex_);
                texts_.insert_or_assign(next.page, std::move(text));
                in_flight_.reset();
                ++version_;
            }
            else {
                auto page = document->render_page(next.page, next.scale, stop);
                if (!page && is_cancelled(page.error()))
                    return;
                Page shared = page ? Page(std::make_shared<const pdfview::RenderedPage>(std::move(*page)))
                                   : Page(std::unexpected(std::move(page.error())));
                const std::scoped_lock lock(mutex_);
                store_render(next, std::move(shared));
                in_flight_.reset();
                ++version_;
            }
            notify();
        }
    }
    catch (const std::exception& e) {
        // Beklenmeyen hata (bellek, iş parçacığı): oturum bozuk belge gibi kapanır, arayüz hatayı gösterir.
        core::log_error(std::string("pdf preview worker: ") + e.what());
        {
            const std::scoped_lock lock(mutex_);
            state_ = State::Failed;
            open_error_ = core::Error{core::Error::Code::InvalidDocument, "error.pdf_invalid", {}};
            queue_.clear();
            in_flight_.reset();
            ++version_;
        }
        notify();
    }
}

void PdfPreview::notify() const {
    if (!options_.on_change)
        return;
    try {
        options_.on_change();
    }
    catch (const std::exception& e) {
        core::log_warn(std::string("pdf preview on_change: ") + e.what());
    }
}

}  // namespace noteapp::app
