#include "gui/text_selection.hpp"

#include <algorithm>
#include <cfloat>

namespace noteapp::gui {

namespace {

// Aynı satırdaki iki koşu arası bundan genişse (yazı boyunun payı) aralarında boşluk vardır: kopyada ' '.
constexpr float kSpaceGapEms = 0.1F;

bool continuation(char byte) {
    constexpr unsigned char kMask = 0xC0;
    constexpr unsigned char kContinuation = 0x80;
    return (static_cast<unsigned char>(byte) & kMask) == kContinuation;
}

std::size_t next_code_point(std::string_view text, std::size_t at) {
    ++at;
    while (at < text.size() && continuation(text[at]))
        ++at;
    return std::min(at, text.size());
}

// Sözcük baytı: ASCII harf, rakam, '_' ya da ASCII dışı (UTF-8 çok baytlı; Türkçe harfler sözcüğü bölmesin).
bool word_byte(char byte) {
    constexpr unsigned char kAsciiEnd = 0x80;
    const auto value = static_cast<unsigned char>(byte);
    return value >= kAsciiEnd || (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') || value == '_';
}

// Boş metinde ölçülmez: CalcTextSizeA boş sonu (nullptr) "sıfırla biten dizgi" sayar, boş görünümün verisi de
// nullptr olabilir.
float measure(ImFont* font, float size, std::string_view text) {
    if (text.empty())
        return 0.0F;
    return font->CalcTextSizeA(size, FLT_MAX, 0.0F, text.data(), text.data() + text.size()).x;
}

}  // namespace

void PreviewSelection::begin_frame() {
    runs_.clear();
    text_.clear();
    painted_end_.reset();
}

void PreviewSelection::add(std::uint64_t key, double line, std::string_view text, ImVec2 pos, float height,
                        ImFont* font, float size, float width) {
    const Run run{key,  line, text_.size(), text.size(), pos, height, font,
                  size, width < 0.0F ? measure(font, size, text) : width};
    text_.append(text);
    runs_.push_back(run);
    if (empty())
        return;
    const Point low = std::min(anchor_, focus_);
    const Point high = std::max(anchor_, focus_);
    const std::size_t begin = key == low.key ? std::min(low.byte, run.length) : 0;
    const std::size_t end = key == high.key ? std::min(high.byte, run.length) : run.length;
    if (key < low.key || key > high.key || begin >= end) {
        painted_end_.reset();
        return;
    }
    float left = pos.x + x_of(run, begin);
    const float right = pos.x + x_of(run, end);
    // Satırdaki sözcükler arası boşluk da boyanır: seçim kesik kesik görünmesin.
    if (begin == 0 && painted_end_ && painted_end_->first == line && painted_end_->second < left)
        left = painted_end_->second;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(left, pos.y), ImVec2(right, pos.y + height),
                                              ImGui::GetColorU32(ImGuiCol_TextSelectedBg));
    painted_end_ = end == run.length ? std::optional(std::pair{line, right}) : std::nullopt;
}

float PreviewSelection::x_of(const Run& run, std::size_t byte) const {
    if (byte == 0)
        return 0.0F;
    if (byte >= run.length)
        return run.width;
    return measure(run.font, run.size, text_of(run).substr(0, byte));
}

std::size_t PreviewSelection::byte_at(const Run& run, float x, bool floor) const {
    const std::string_view text = text_of(run);
    const float offset = x - run.pos.x;
    if (offset <= 0.0F || text.empty())
        return 0;
    float advance = 0.0F;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t next = next_code_point(text, at);
        const float width = measure(run.font, run.size, text.substr(at, next - at));
        if (offset < advance + (floor ? width : width / 2.0F))
            return at;
        advance += width;
        if (floor && next == text.size())
            return at;  // sağında: son kod noktası
        at = next;
    }
    return text.size();
}

const PreviewSelection::Run* PreviewSelection::nearest(ImVec2 point, bool* inside) const {
    const Run* best = nullptr;
    float best_dy = FLT_MAX;
    float best_dx = FLT_MAX;
    for (const Run& run : runs_) {
        const auto distance = [](float at, float low, float high) {
            return at < low ? low - at : at > high ? at - high : 0.0F;
        };
        const float dy = distance(point.y, run.pos.y, run.pos.y + run.height);
        const float dx = distance(point.x, run.pos.x, run.pos.x + run.width);
        if (dy < best_dy || (dy == best_dy && dx < best_dx)) {
            best = &run;
            best_dy = dy;
            best_dx = dx;
        }
    }
    *inside = best != nullptr && best_dy == 0.0F && best_dx == 0.0F;
    return best;
}

void PreviewSelection::select_word(const Run& run, std::size_t byte) {
    const std::string_view text = text_of(run);
    std::size_t begin = byte;
    std::size_t end = byte;
    if (byte < text.size() && word_byte(text[byte])) {
        while (begin > 0 && word_byte(text[begin - 1]))
            --begin;
        while (end < text.size() && word_byte(text[end]))
            ++end;
    } else if (byte < text.size()) {
        end = next_code_point(text, byte);  // noktalama ya da boşluk: tek kod noktası
    }
    anchor_ = {run.key, begin};
    focus_ = {run.key, end};
}

bool PreviewSelection::update(bool can_start) {
    // Okuma sırası anahtardır; kırpıcı aynı satırı iki kez çizerse (ölçüm adımı) bir kez sayılır.
    std::ranges::stable_sort(runs_, {}, &Run::key);
    const auto duplicates = std::ranges::unique(runs_, {}, &Run::key);
    runs_.erase(duplicates.begin(), duplicates.end());
    const ImGuiIO& io = ImGui::GetIO();
    bool inside = false;
    if (can_start && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const Run* run = nearest(io.MousePos, &inside);
        if (run == nullptr) {
            clear();
            return false;
        }
        if (inside && ImGui::GetMouseClickedCount(ImGuiMouseButton_Left) == 2) {
            select_word(*run, byte_at(*run, io.MousePos.x, true));
            dragging_ = false;
            return true;
        }
        focus_ = {run->key, byte_at(*run, io.MousePos.x, false)};
        if (!io.KeyShift)  // Shift+tık seçimi genişletir (editör gibi)
            anchor_ = focus_;
        dragging_ = true;
        return false;
    }
    if (!dragging_)
        return false;
    if (const Run* run = nearest(io.MousePos, &inside))
        focus_ = {run->key, byte_at(*run, io.MousePos.x, false)};
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        dragging_ = false;
    return false;
}

void PreviewSelection::clear() {
    anchor_ = focus_ = {};
    dragging_ = false;
}

std::string PreviewSelection::selected_text() const {
    std::string out;
    if (empty())
        return out;
    const Point low = std::min(anchor_, focus_);
    const Point high = std::max(anchor_, focus_);
    const Run* previous = nullptr;
    for (const Run& run : runs_) {
        if (run.key < low.key || run.key > high.key)
            continue;
        const std::size_t begin = run.key == low.key ? std::min(low.byte, run.length) : 0;
        const std::size_t end = run.key == high.key ? std::min(high.byte, run.length) : run.length;
        if (begin >= end)
            continue;
        if (previous != nullptr && previous->line != run.line)
            out += '\n';
        else if (previous != nullptr && run.pos.x - (previous->pos.x + previous->width) > run.size * kSpaceGapEms)
            out += ' ';
        out.append(text_of(run).substr(begin, end - begin));
        previous = &run;
    }
    return out;
}

std::optional<ImVec2> PreviewSelection::locate(std::string_view needle, bool end) const {
    for (const Run& run : runs_) {
        const std::size_t at = text_of(run).find(needle);
        if (at == std::string_view::npos)
            continue;
        const std::size_t byte = at + (end ? needle.size() : 0);
        return ImVec2(run.pos.x + x_of(run, byte), run.pos.y + (run.height / 2.0F));
    }
    return std::nullopt;
}

}  // namespace noteapp::gui
