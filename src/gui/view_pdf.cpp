// PDF önizlemesi (SPEC-gui "PDF önizleme"): sayfalar alt alta, yalnız görünenler çizilir. RGBA'yı işçi üretir
// (app::PdfPreview); doku burada, UI iş parçacığında kurulur.
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "gui/view_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace noteapp::gui {

namespace {

// Çizim ölçeği arayüz ölçeğinin bu katı (1 = 72 dpi): A4 ~890 px genişlikte, metin okunur. GUI'de yakınlaştırma
// geleneği yok: sayfa pencereden genişse küçültülerek sığdırılır.
constexpr float kPdfScale = 1.5F;
constexpr float kPageGap = 12.0F;              // sayfalar arası, ölçek 1'de px
constexpr ImVec2 kA4Points{595.0F, 842.0F};    // ilk sayfa çizilene dek yuva tahmini

// Sayfayı yuvaya oranını koruyarak sığdırır; küçük sayfa büyütülmez.
ImVec2 fit_into(ImVec2 size, ImVec2 box) {
    const float factor = std::min({1.0F, box.x / size.x, box.y / size.y});
    return {std::floor(size.x * factor), std::floor(size.y * factor)};
}

}  // namespace

std::size_t View::pdf_texture_count() const {
    return static_cast<std::size_t>(std::ranges::count_if(
        preview_->pdf_textures, [](const auto& entry) { return entry.second.texture != nullptr; }));
}

void View::draw_pdf_preview() {
    app::PdfPreview* pdf = state_.pdf_preview();
    if (pdf == nullptr)
        return;
    PreviewCache& cache = *preview_;
    const float ui = ImGui::GetStyle().FontScaleDpi;
    const float scale = kPdfScale * ui;
    if (cache.pdf_session != state_.pdf_serial() || cache.pdf_scale != scale) {
        cache.pdf_textures.clear();  // geçen karenin çizim listesi çoktan sunuldu
        cache.pdf_session = state_.pdf_serial();
        cache.pdf_scale = scale;
        cache.pdf_page_size = {};
        cache.pdf_visible = {-1, -1};
    }
    switch (pdf->state()) {
    case app::PdfPreview::State::Opening:
        ImGui::TextDisabled("%s", core::tr("app.pdf_loading").c_str());
        return;
    case app::PdfPreview::State::Failed:
        draw_error(core::format_error(pdf->open_error().value()));
        return;
    case app::PdfPreview::State::Ready:
        break;
    }
    const int count = pdf->page_count();
    if (count <= 0)
        return;

    // Bütün sayfalar aynı yuvada (ilk çizilen sayfanın boyu; A4 tahmini ondan önce): kaydırma kararlı, görünen
    // sayfalar kaydırmadan hesaplanır.
    const float available = ImGui::GetContentRegionAvail().x;
    const ImVec2 natural = cache.pdf_page_size.x > 0.0F ? cache.pdf_page_size
                                                        : ImVec2(kA4Points.x * scale, kA4Points.y * scale);
    const ImVec2 slot_size = fit_into(natural, ImVec2(std::max(available, 1.0F), natural.y));
    const float slot = slot_size.y + (kPageGap * ui);
    const float scrolled = ImGui::GetScrollY();
    const int first = std::clamp(static_cast<int>(scrolled / slot), 0, count - 1);
    const int last = std::clamp(static_cast<int>((scrolled + ImGui::GetWindowHeight()) / slot), first, count - 1);
    if (cache.pdf_visible != std::pair(first, last)) {
        pdf->drop_pending();  // görünmez olan sayfaların bekleyen çizimleri; yürüyen çizim biter
        cache.pdf_visible = {first, last};
    }

    // Gelen sayfanın dokusu; hata yerinde gösterilir ve saklanır (her karede yeniden denenmez).
    const auto upload = [&](int page, const pdfview::RenderedPage& rendered) {
        PreviewCache::PdfTexture entry;
        entry.width = static_cast<float>(rendered.width);
        entry.height = static_cast<float>(rendered.height);
        entry.used = frames_;
        entry.texture.reset(SDL_CreateTexture(platform_->renderer.get(), SDL_PIXELFORMAT_RGBA32,
                                              SDL_TEXTUREACCESS_STATIC, rendered.width, rendered.height));
        if (!entry.texture ||
            !SDL_UpdateTexture(entry.texture.get(), nullptr, rendered.rgba.data(), rendered.width * 4)) {
            core::log_warn(std::string("pdf texture: ") + SDL_GetError());  // SDL metni yalnız loga
            entry.error = core::tr("app.pdf_texture_failed", {{"width", std::to_string(rendered.width)},
                                                               {"height", std::to_string(rendered.height)}});
            entry.texture.reset();
        }
        else {
            SDL_SetTextureScaleMode(entry.texture.get(), SDL_SCALEMODE_LINEAR);
            if (cache.pdf_page_size.x <= 0.0F)
                cache.pdf_page_size = ImVec2(entry.width, entry.height);
        }
        return cache.pdf_textures.insert_or_assign(page, std::move(entry)).first;
    };
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    for (int page = first; page <= last; ++page) {
        const ImVec2 top(origin.x + std::floor((available - slot_size.x) / 2.0F),
                         origin.y + (slot * static_cast<float>(page)));
        auto found = cache.pdf_textures.find(page);
        std::string message;
        if (found == cache.pdf_textures.end()) {
            if (const auto result = pdf->render(page, scale); !result)
                pdf->request_render(page, scale);
            else if (!*result)
                message = core::format_error(result->error());
            else
                found = upload(page, ***result);
        }
        if (found != cache.pdf_textures.end() && found->second.texture) {
            found->second.used = frames_;
            const ImVec2 size = fit_into(ImVec2(found->second.width, found->second.height), slot_size);
            draw->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(
                               found->second.texture.get()))),
                           top, ImVec2(top.x + size.x, top.y + size.y));
        }
        else {
            if (found != cache.pdf_textures.end())
                message = found->second.error;
            draw->AddRectFilled(top, ImVec2(top.x + slot_size.x, top.y + slot_size.y),
                                ImGui::GetColorU32(ImGuiCol_FrameBg));
            ImGui::SetCursorScreenPos(ImVec2(top.x + padding.x, top.y + padding.y));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + slot_size.x - (2.0F * padding.x));
            if (message.empty())
                ImGui::TextDisabled("%s",
                                    core::tr("app.pdf_page_loading", {{"page", std::to_string(page + 1)}}).c_str());
            else {
                ImGui::PushStyleColor(ImGuiCol_Text, theme::current().danger);
                ImGui::TextUnformatted(message.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopTextWrapPos();
        }
        remember_rect("pdf_page:" + std::to_string(page + 1), top,
                      ImVec2(top.x + slot_size.x, top.y + slot_size.y));
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(available, slot * static_cast<float>(count)));

    // Doku sınırı: görünen sayfalar dışında en uzun süredir çizilmeyen düşer. Bu karede çizilen doku düşmez.
    const std::size_t cap = state_.pdf_limits().cache_pages.cap();
    while (cache.pdf_textures.size() > cap) {
        auto oldest = cache.pdf_textures.end();
        for (auto entry = cache.pdf_textures.begin(); entry != cache.pdf_textures.end(); ++entry)
            if ((entry->first < first || entry->first > last) &&
                (oldest == cache.pdf_textures.end() || entry->second.used < oldest->second.used))
                oldest = entry;
        if (oldest == cache.pdf_textures.end())
            break;
        cache.pdf_textures.erase(oldest);
    }

    // Sayfa göstergesi "3 / 12": görünen alanın sağ üstünde, sayfaların üstünde; yarısından çoğu geçen sayfa
    // sonrakine sayılır.
    const int current = std::clamp(static_cast<int>((scrolled + (slot / 2.0F)) / slot) + 1, 1, count);
    const std::string label = core::tr("app.pdf_page_indicator",
                                       {{"page", std::to_string(current)}, {"count", std::to_string(count)}});
    const ImVec2 label_size = ImGui::CalcTextSize(label.c_str());
    const ImVec2 at(origin.x + available - label_size.x - (2.0F * padding.x),
                    ImGui::GetWindowPos().y + ImGui::GetStyle().WindowPadding.y);
    const ImVec2 end(at.x + label_size.x + (2.0F * padding.x), at.y + label_size.y + (2.0F * padding.y));
    draw->AddRectFilled(at, end, ImGui::GetColorU32(ImGuiCol_PopupBg), ImGui::GetStyle().FrameRounding);
    ImGui::SetCursorScreenPos(ImVec2(at.x + padding.x, at.y + padding.y));
    ImGui::TextUnformatted(label.c_str());
    remember_rect("pdf_indicator", at, end);
}

}  // namespace noteapp::gui
