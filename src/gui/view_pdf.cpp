// PDF önizlemesi (SPEC-gui "PDF önizleme"): sayfalar alt alta, yalnız görünenler çizilir. RGBA'yı işçi üretir
// (app::PdfPreview); doku burada, UI iş parçacığında kurulur. Sayfa görüntüsünün üstünde görünmez metin katmanı
// (app::PdfTextLayer): sürükleme karakter seçer, çift tık sözcük seçer; Ctrl+C ve sağ tık "Kopyala" öteki
// önizlemelerle ortak yoldan (View::draw_preview) kopyalar.
#include "core/i18n.hpp"
#include "core/log.hpp"
#include "gui/view_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <tuple>

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

// Sayfanın ekrandaki görüntüsü: punto <-> piksel.
struct PageFrame {
    ImVec2 top;
    ImVec2 scale;  // piksel / punto
    ImVec2 to_screen(float x, float y) const { return {top.x + (x * scale.x), top.y + (y * scale.y)}; }
    ImVec2 to_page(ImVec2 screen) const { return {(screen.x - top.x) / scale.x, (screen.y - top.y) / scale.y}; }
};

// Sayfanın altına (üstüne) çekilen fare son (ilk) karaktere kadar seçer; yan taşmada en yakın satır.
std::size_t drag_caret(const app::PdfTextLayer& layer, ImVec2 at) {
    if (at.y > layer.height())
        return layer.size();
    if (at.y < 0.0F)
        return 0;
    return layer.caret_at(at.x, at.y);
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
    if (cache.pdf_session != state_.pdf_serial())
        cache.pdf_text.reset();  // metin ölçekten bağımsız: yalnız oturumla atılır
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
            draw_pdf_text_layer(*pdf, page, top, size);
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
    PdfSelection& text = cache.pdf_text;
    while (text.pages.size() > cap) {
        auto oldest = text.pages.end();
        for (auto entry = text.pages.begin(); entry != text.pages.end(); ++entry)
            if ((entry->first < first || entry->first > last) &&
                (oldest == text.pages.end() || entry->second.used < oldest->second.used))
                oldest = entry;
        if (oldest == text.pages.end())
            break;
        if (oldest->first == text.page)
            text.clear_selection();
        text.pages.erase(oldest);
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

// Sayfanın metin katmanı: görüntünün üstünde görünmez düğme (fare), seçimin vurgusu. Metin yalnız görünen sayfa için
// istenir; gelen sonuç oturumdan alınır (take_text) ve burada tutulur. Okunamayan sayfanın hatası da tutulur:
// sayfa seçilemez, her karede yeniden istenmez (görüntü hatası ayrıca görünür).
void View::draw_pdf_text_layer(app::PdfPreview& pdf, int page, ImVec2 top, ImVec2 size) {
    PdfSelection& text = preview_->pdf_text;
    auto found = text.pages.find(page);
    if (found == text.pages.end()) {
        auto taken = pdf.take_text(page);
        if (!taken) {
            pdf.request_text(page);
            return;
        }
        PdfSelection::Page entry;
        if (*taken)
            entry.layer.emplace(**taken);
        else
            core::log_warn("event=pdf_text_layer_failed page=" + std::to_string(page + 1) + " error=" +
                           taken->error().message_key);
        found = text.pages.insert_or_assign(page, std::move(entry)).first;
    }
    found->second.used = frames_;
    if (!found->second.layer || found->second.layer->size() == 0)
        return;
    const app::PdfTextLayer& layer = *found->second.layer;
    const PageFrame frame{top, ImVec2(size.x / layer.width(), size.y / layer.height())};

    ImGui::SetCursorScreenPos(top);
    ImGui::InvisibleButton(("##pdf_text" + std::to_string(page)).c_str(), size);
    const ImVec2 mouse = frame.to_page(ImGui::GetIO().MousePos);
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        preview_link_hovered_ = true;  // çift tık sözcük seçer, düzenlemeye geçmez
    }
    if (ImGui::IsItemActivated()) {
        preview_selecting_ = true;  // bırakılınca draw_preview kopyalar
        text.page = page;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            std::tie(text.anchor, text.caret) = layer.word_at(mouse.x, mouse.y);
        } else {
            text.anchor = text.caret = drag_caret(layer, mouse);
        }
    } else if (ImGui::IsItemActive() && text.page == page && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0F)) {
        text.caret = drag_caret(layer, mouse);
    }

    if (text.page != page || !text.active())
        return;
    const auto [begin, end] = text.range();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);  // editörün seçim rengi
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Uzun seçimin (tümü) yalnız görünen satırları çizim listesine girer.
    const float clip_top = draw->GetClipRectMin().y;
    const float clip_bottom = draw->GetClipRectMax().y;
    for (const app::PdfRect& box : layer.boxes(begin, end)) {
        const ImVec2 min = frame.to_screen(box.left, box.top);
        const ImVec2 max = frame.to_screen(box.right, box.bottom);
        if (max.y >= clip_top && min.y <= clip_bottom)
            draw->AddRectFilled(min, max, color);
    }
    remember_rect("pdf_selection", frame.to_screen(0.0F, 0.0F), frame.to_screen(layer.width(), layer.height()));
}

// Kopyalama (bırakınca, Ctrl+C, sağ tık) bütün önizlemelerle ortak kuraldır: draw_preview seçim varsa bunu
// çağırır.
std::string View::pdf_selected_text() const {
    const PdfSelection& selection = preview_->pdf_text;
    const auto found = selection.pages.find(selection.page);
    if (!selection.active() || found == selection.pages.end() || !found->second.layer)
        return {};
    const auto [begin, end] = selection.range();
    return found->second.layer->text(begin, end);
}

}  // namespace noteapp::gui
