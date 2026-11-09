#include "app/pdf_text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <ranges>
#include <string_view>

namespace noteapp::app {

namespace {

using pdfview::PageText;
using pdfview::TextLine;
using pdfview::TextSpan;

// Başlık eşikleri: satırın boyu / gövde boyu. Belgeler arası yaygın oranlar (h1 ~1.7-2x, h2 ~1.4x, h3 ~1.2x);
// 1.12'nin altı gövde sayılır (alt simge, dipnot, küçük boy farkları).
constexpr std::array<float, 3> kHeadingRatios{1.6F, 1.3F, 1.12F};
// Satır yüksekliğinin bu katından açık aralık paragraf sonudur (sıradan satır aralığı ~0.2).
constexpr float kParagraphGap = 0.6F;
// Gövde boyunun bu katından girintili ilk satır paragraf başıdır.
constexpr float kIndentEms = 1.0F;
constexpr float kSizeBuckets = 2.0F;  // boy yarım puntoya yuvarlanarak sayılır

bool is_continuation(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

std::size_t code_points(std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(text, [](char byte) { return !is_continuation(byte); }));
}

// Geçerli UTF-8'in tek kod noktası (pdfview::PdfDocument denetlemiştir).
char32_t decode(std::string_view bytes) {
    const auto at = [&](std::size_t i) { return static_cast<char32_t>(static_cast<unsigned char>(bytes[i])); };
    const char32_t lead = at(0);
    if (lead < 0x80 || bytes.size() < 2)
        return lead;
    if (bytes.size() == 2)
        return ((lead & 0x1FU) << 6U) | (at(1) & 0x3FU);
    if (bytes.size() == 3)
        return ((lead & 0x0FU) << 12U) | ((at(1) & 0x3FU) << 6U) | (at(2) & 0x3FU);
    return ((lead & 0x07U) << 18U) | ((at(1) & 0x3FU) << 12U) | ((at(2) & 0x3FU) << 6U) | (at(3) & 0x3FU);
}

// MuPDF'in fz_is_unicode_hyphen'i: tireyle bölünmüş satırın son karakteri.
bool is_hyphen(char32_t cp) {
    return cp == U'-' || cp == 0x00AD || cp == 0x2010 || cp == 0x2011;
}

bool is_space(char32_t cp) {
    return cp == U' ' || cp == 0x00A0 || (cp >= 0x2000 && cp <= 0x200B) || cp == 0x3000;
}

std::string spaced(std::string_view text) {
    std::string out(text);
    std::ranges::replace_if(out, [](char c) { return c == '\t' || c == '\n' || c == '\r'; }, ' ');
    return out;
}

float size_of(float bucket) {
    return bucket / kSizeBuckets;
}

int bucket_of(float size) {
    return static_cast<int>(std::lround(size * kSizeBuckets));
}

// Karakter sayısıyla ağırlıklı en yaygın boy (eşitlikte küçük olan).
float dominant_size(const std::map<int, std::size_t>& weights) {
    int best = 0;
    std::size_t most = 0;
    for (const auto& [bucket, weight] : weights) {
        if (weight > most) {
            best = bucket;
            most = weight;
        }
    }
    return size_of(static_cast<float>(best));
}

float body_size(const PageText& page) {
    std::map<int, std::size_t> weights;
    for (const auto& block : page.blocks)
        for (const TextLine& line : block.lines)
            for (const TextSpan& span : line.spans)
                weights[bucket_of(span.size)] += code_points(span.text);
    return dominant_size(weights);
}

int heading_level(const TextLine& line, float body) {
    if (body <= 0.0F)
        return 0;
    std::map<int, std::size_t> weights;
    for (const TextSpan& span : line.spans)
        weights[bucket_of(span.size)] += code_points(span.text);
    const float ratio = dominant_size(weights) / body;
    for (std::size_t level = 0; level < kHeadingRatios.size(); ++level)
        if (ratio >= kHeadingRatios[level])
            return static_cast<int>(level) + 1;
    return 0;
}

float line_left(const TextLine& line) {
    return line.spans.front().edges.front();
}

std::string line_text(const TextLine& line) {
    std::string text;
    for (const TextSpan& span : line.spans)
        text += span.text;
    return text;
}

// Liste imiyle başlayan satır: madde imi ya da kısa sıra ("1.", "12)", "a)") ve ardından boşluk.
bool is_list_item(const TextLine& line) {
    const std::string text = spaced(line_text(line));
    std::string_view rest(text);
    rest.remove_prefix(std::min(rest.find_first_not_of(' '), rest.size()));
    for (const std::string_view bullet : {"•", "◦", "▪", "‣", "●", "○", "■", "□", "–", "·", "-", "*"})
        if (rest.starts_with(bullet) && rest.substr(bullet.size()).starts_with(' '))
            return true;
    constexpr std::size_t kMaxOrdinalDigits = 3;
    std::size_t digits = 0;
    while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9')
        ++digits;
    std::size_t marker = digits;
    if (digits == 0 && !rest.empty() && std::isalpha(static_cast<unsigned char>(rest[0])) != 0)
        marker = 1;
    if (marker == 0 || digits > kMaxOrdinalDigits || marker + 1 >= rest.size())
        return false;
    const char close = rest[marker];
    return (close == ')' || (close == '.' && digits > 0)) && rest[marker + 1] == ' ';
}

pdf::TextStyle style_of(std::uint8_t bits) {
    return pdf::TextStyle{.bold = (bits & pdfview::kStyleBold) != 0,
                          .italic = (bits & pdfview::kStyleItalic) != 0,
                          .mono = (bits & pdfview::kStyleMono) != 0,
                          .strike = false};
}

// Paragrafın son karakteri (yoksa 0) ve onu düşürme.
char32_t last_char(const pdf::Block& block) {
    for (auto span = block.spans.rbegin(); span != block.spans.rend(); ++span) {
        const std::string& text = span->text;
        if (text.empty())
            continue;
        std::size_t start = text.size() - 1;
        while (start > 0 && is_continuation(text[start]))
            --start;
        return decode(std::string_view(text).substr(start));
    }
    return 0;
}

void drop_last_char(pdf::Block& block) {
    while (!block.spans.empty() && block.spans.back().text.empty())
        block.spans.pop_back();
    if (block.spans.empty())
        return;
    std::string& text = block.spans.back().text;
    while (!text.empty() && is_continuation(text.back()))
        text.pop_back();
    if (!text.empty())
        text.pop_back();
}

void append(pdf::Block& block, std::string text, pdf::TextStyle style) {
    if (text.empty())
        return;
    if (!block.spans.empty() && block.spans.back().style == style)
        block.spans.back().text += text;
    else
        block.spans.push_back(pdf::Span{.text = std::move(text), .style = style});
}

bool blank(const pdf::Span& span) {
    return span.text.find_first_not_of(' ') == std::string::npos;
}

// Baştaki ve sondaki boşluk; yalnız boşluk parçaları tek silmeyle düşer (düşman sayfada yüz binlercesi: parça parça
// silmek O(N²) olur, TUI'nin UI iş parçacığı donar).
void trim(pdf::Block& block) {
    block.spans.erase(block.spans.begin(), std::ranges::find_if_not(block.spans, blank));
    const auto last = std::ranges::find_if_not(block.spans | std::views::reverse, blank);
    block.spans.erase(last.base(), block.spans.end());
    if (block.spans.empty())
        return;
    std::string& first = block.spans.front().text;
    first.erase(0, first.find_first_not_of(' '));
    std::string& end = block.spans.back().text;
    end.erase(end.find_last_not_of(' ') + 1);
}

// Blok içindeki satırları paragraflara toplar.
class Paragraphs {
public:
    Paragraphs(float body, std::vector<pdf::Block>& out) : body_(body), out_(out) {}

    void add_block(const pdfview::TextBlock& block) {
        float min_left = std::numeric_limits<float>::max();
        for (const TextLine& line : block.lines)
            min_left = std::min(min_left, line_left(line));
        const TextLine* previous = nullptr;
        for (const TextLine& line : block.lines) {
            const int level = heading_level(line, body_);
            const float left = line_left(line);
            if (previous == nullptr || starts_paragraph(*previous, line, level, left, min_left)) {
                flush();
                current_.kind = level > 0 ? pdf::Block::Kind::Heading : pdf::Block::Kind::Paragraph;
                current_.heading_level = level;
            } else {
                join(*previous);
            }
            for (const TextSpan& span : line.spans)
                append(current_, spaced(span.text), style_of(span.style));
            previous = &line;
            previous_level_ = level;
            previous_left_ = left;
        }
        flush();
    }

private:
    bool starts_paragraph(const TextLine& previous, const TextLine& line, int level, float left,
                          float min_left) const {
        if (level != previous_level_ || is_list_item(line))
            return true;
        const float height = previous.bottom - previous.top;
        if (height > 0.0F && line.top - previous.bottom > kParagraphGap * height)
            return true;
        const float indent = body_ * kIndentEms;
        return level == 0 && left - min_left >= indent && previous_left_ - min_left < indent;
    }

    // Sarılmış satır: tireyle bölünmüş sözcük tiresiz, değilse tek boşlukla birleşir.
    void join(const TextLine& previous) {
        const char32_t last = last_char(current_);
        if (previous.joined && is_hyphen(last))
            drop_last_char(current_);
        else if (last != 0 && !is_space(last) && !previous.joined)
            append(current_, " ", current_.spans.back().style);
    }

    void flush() {
        trim(current_);
        if (!current_.spans.empty())
            out_.push_back(std::move(current_));
        current_ = {};
    }

    float body_;
    std::vector<pdf::Block>& out_;
    pdf::Block current_;
    int previous_level_ = 0;
    float previous_left_ = 0.0F;
};

}  // namespace

std::vector<pdf::Block> pdf_page_blocks(const PageText& page) {
    std::vector<pdf::Block> blocks;
    Paragraphs paragraphs(body_size(page), blocks);
    for (const auto& block : page.blocks)
        paragraphs.add_block(block);
    return blocks;
}

PdfTextLayer::PdfTextLayer(const PageText& page) : width_(page.width), height_(page.height) {
    for (std::size_t b = 0; b < page.blocks.size(); ++b) {
        for (const TextLine& line : page.blocks[b].lines) {
            const auto line_index = static_cast<std::uint32_t>(lines_.size());
            lines_.push_back(Line{.top = line.top,
                                  .bottom = line.bottom,
                                  .block = static_cast<std::uint32_t>(b),
                                  .joined = line.joined,
                                  .first = static_cast<std::uint32_t>(chars_.size())});
            for (const TextSpan& span : line.spans) {
                std::size_t at = 0;
                for (std::size_t i = 0; i * 2 + 1 < span.edges.size() && at < span.text.size(); ++i) {
                    std::size_t length = 1;
                    while (at + length < span.text.size() && is_continuation(span.text[at + length]))
                        ++length;
                    chars_.push_back(Char{.left = span.edges[i * 2],
                                          .right = span.edges[(i * 2) + 1],
                                          .line = line_index,
                                          .byte = static_cast<std::uint32_t>(text_.size()),
                                          .length = static_cast<std::uint8_t>(length)});
                    text_ += spaced(std::string_view(span.text).substr(at, length));
                    at += length;
                }
            }
        }
    }
}

std::size_t PdfTextLayer::line_at(float x, float y) const {
    std::size_t best = 0;
    float best_vertical = std::numeric_limits<float>::max();
    float best_horizontal = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < lines_.size(); ++index) {
        const Line& line = lines_[index];
        const std::size_t first = line.first;
        const std::size_t end = index + 1 < lines_.size() ? lines_[index + 1].first : chars_.size();
        if (first == end)
            continue;
        const float vertical = y < line.top ? line.top - y : std::max(y - line.bottom, 0.0F);
        const float left = chars_[first].left;
        const float right = chars_[end - 1].right;
        const float horizontal = x < left ? left - x : std::max(x - right, 0.0F);
        if (vertical < best_vertical || (vertical == best_vertical && horizontal < best_horizontal)) {
            best = index;
            best_vertical = vertical;
            best_horizontal = horizontal;
        }
    }
    return best;
}

std::pair<std::size_t, std::size_t> PdfTextLayer::line_chars(std::size_t line) const {
    return {lines_[line].first, line + 1 < lines_.size() ? lines_[line + 1].first : chars_.size()};
}

std::size_t PdfTextLayer::caret_at(float x, float y) const {
    if (chars_.empty())
        return 0;
    const auto [first, end] = line_chars(line_at(x, y));
    for (std::size_t index = first; index < end; ++index)
        if (x < (chars_[index].left + chars_[index].right) / 2.0F)
            return index;
    return end;
}

bool PdfTextLayer::is_word_char(std::size_t index) const {
    const char32_t cp = decode(std::string_view(text_).substr(chars_[index].byte, chars_[index].length));
    if (cp < 0x80)
        return std::isalnum(static_cast<int>(cp)) != 0 || cp == U'_';
    return !is_space(cp) && !(cp >= 0x2010 && cp <= 0x206F) && !(cp >= 0x3000 && cp <= 0x303F) &&
           cp != 0x00AD && cp != 0xFFFD;
}

std::pair<std::size_t, std::size_t> PdfTextLayer::word_at(float x, float y) const {
    if (chars_.empty())
        return {0, 0};
    const auto [first, end] = line_chars(line_at(x, y));
    std::size_t hit = end - 1;
    for (std::size_t index = first; index < end; ++index) {
        if (x < chars_[index].right) {
            hit = index;
            break;
        }
    }
    if (!is_word_char(hit))
        return {hit, hit + 1};
    std::size_t begin = hit;
    while (begin > first && is_word_char(begin - 1))
        --begin;
    std::size_t stop = hit + 1;
    while (stop < end && is_word_char(stop))
        ++stop;
    return {begin, stop};
}

std::string PdfTextLayer::text(std::size_t begin, std::size_t end) const {
    end = std::min(end, chars_.size());
    std::string out;
    for (std::size_t index = begin; index < end; ++index) {
        const Char& ch = chars_[index];
        if (index > begin && ch.line != chars_[index - 1].line) {
            const Line& previous = lines_[chars_[index - 1].line];
            const char32_t last = decode(std::string_view(text_).substr(chars_[index - 1].byte, chars_[index - 1].length));
            if (previous.block != lines_[ch.line].block) {
                out += '\n';
            } else if (previous.joined && is_hyphen(last)) {
                out.resize(out.size() - chars_[index - 1].length);
            } else if (!is_space(last)) {
                out += ' ';
            }
        }
        out.append(text_, ch.byte, ch.length);
    }
    return out;
}

std::vector<PdfRect> PdfTextLayer::boxes(std::size_t begin, std::size_t end) const {
    end = std::min(end, chars_.size());
    std::vector<PdfRect> out;
    for (std::size_t index = begin; index < end; ++index) {
        const Char& ch = chars_[index];
        if (index > begin && ch.line == chars_[index - 1].line) {
            out.back().left = std::min(out.back().left, ch.left);
            out.back().right = std::max(out.back().right, ch.right);
            continue;
        }
        const Line& line = lines_[ch.line];
        out.push_back(PdfRect{.left = ch.left, .top = line.top, .right = ch.right, .bottom = line.bottom});
    }
    return out;
}

}  // namespace noteapp::app
