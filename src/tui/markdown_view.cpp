#include "tui/markdown_view.hpp"

#include "core/i18n.hpp"
#include "core/text.hpp"
#include "edit/display_text.hpp"
#include "pdf/markdown_blocks.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <limits>
#include <md4c.h>
#include <utility>
#include <vector>

namespace noteapp::tui {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::string_view kReplacement = "\xEF\xBF\xBD";
constexpr std::string_view kQuoteBar = "│ ";
constexpr std::string_view kCodeIndent = "  ";
constexpr int kTabCells = 4;  // EditorBuffer::kTabCells ile aynı: ham ve biçimli görünüm hizası tutsun
constexpr const auto& kBullets = pdf::kListBullets;  // GUI önizlemesi ve PDF ile aynı imler

TextLook bold_look() {
    TextLook look;
    look.bold = true;
    return look;
}

TextLook dim_look() {
    TextLook look;
    look.dim = true;
    return look;
}

TextLook token_look(TokenKind token) {
    TextLook look;
    look.token = token;
    return look;
}

TextLook heading_look(int level) {
    TextLook look = bold_look();
    look.token = level <= 2 ? TokenKind::Heading : TokenKind::Type;
    look.underline = level == 1;
    return look;
}

std::string_view attribute_text(const MD_ATTRIBUTE& attribute) {
    return {attribute.text, attribute.size};
}

// Öznitelik parçaları ham metin ve varlıktır; gösterim için çözülür, arındırma çağıranda.
std::string decoded_attribute(const MD_ATTRIBUTE& attribute) {
    const std::string_view raw = attribute_text(attribute);
    std::string out;
    for (std::size_t i = 0; attribute.substr_offsets[i] < attribute.size; ++i) {
        const MD_OFFSET begin = attribute.substr_offsets[i];
        const std::string_view part = raw.substr(begin, attribute.substr_offsets[i + 1] - begin);
        if (attribute.substr_types[i] == MD_TEXT_ENTITY)
            out += core::decode_html_entity(part);
        else if (attribute.substr_types[i] == MD_TEXT_NULLCHAR)
            out += kReplacement;
        else
            out += part;
    }
    return out;
}

CellAlign cell_align(MD_ALIGN align) {
    return align == MD_ALIGN_RIGHT ? CellAlign::Right : align == MD_ALIGN_CENTER ? CellAlign::Center : CellAlign::Left;
}

// Kap öneki (girinti, `>`) atlanır; çit en az üç aynı karakter ve ardından yalnız boşluktur.
bool is_closing_fence(std::string_view line, char fence) {
    constexpr std::size_t kMinFenceLength = 3;
    const std::size_t start = line.find_first_not_of(" \t>");
    if (start == std::string_view::npos)
        return false;
    line.remove_prefix(start);
    const std::size_t run = std::min(line.find_first_not_of(fence), line.size());
    return run >= kMinFenceLength && line.find_first_not_of(" \t\r\n", run) == std::string_view::npos;
}

// md4c konum vermez, ama metin geri çağrısı kaynağın içini gösterir: blok -> satır eşlemesi buradan çıkar.
// Metni olmayan blok (yatay çizgi, boş öğe) son tüketilen yerden sonraki ilk dolu satırdır.
class MarkdownRenderer {
public:
    MarkdownRenderer(std::string_view source, int width, CellWidthFn cell_width, const ViewLimits& limits)
        : source_(source), lines_(source), view_(width, cell_width, limits.lines), limits_(limits),
          width_(std::max(width, 1)) {}

    RenderedView finish() && {
        close_leaf();
        return std::move(view_).finish();
    }

    void enter_block(MD_BLOCKTYPE type, const void* detail) {
        if (enter_container(type, detail) || enter_table_part(type, detail))
            return;
        close_leaf();
        switch (type) {
        case MD_BLOCK_DOC:
            break;
        case MD_BLOCK_HR:
            add_rule();
            break;
        case MD_BLOCK_H:
            leaf_ = Leaf::Heading;
            heading_level_ = static_cast<int>(static_cast<const MD_BLOCK_H_DETAIL*>(detail)->level);
            break;
        case MD_BLOCK_CODE:
            begin_code(*static_cast<const MD_BLOCK_CODE_DETAIL*>(detail));
            break;
        default:
            leaf_ = Leaf::Paragraph;
            break;
        }
    }

    void leave_block(MD_BLOCKTYPE type) {
        if (leave_table_part(type) || type == MD_BLOCK_DOC)
            return;
        close_leaf();
        switch (type) {
        case MD_BLOCK_QUOTE:
            nesting_.pop_back();
            quotes_.pop_back();
            pending_gap_ = true;
            break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            flush_marker();
            lists_.pop_back();
            nesting_.pop_back();
            pending_gap_ = !in_tight_list();
            break;
        case MD_BLOCK_LI:
            flush_marker();
            break;
        case MD_BLOCK_CODE:
            // Girintisi biten kap çiti kapatmadan da bitirir: sıradaki satır ancak gerçekten çitse tüketilir.
            if (code_fence_ != 0 && is_closing_fence(lines_.line_text(peek_unread_line()), code_fence_))
                take_unread_line();
            break;
        default:
            break;
        }
    }

    void enter_span(MD_SPANTYPE type, const void* detail) {
        switch (type) {
        case MD_SPAN_STRONG:
            ++bold_depth_;
            break;
        case MD_SPAN_EM:
            ++italic_depth_;
            break;
        case MD_SPAN_DEL:
            ++strike_depth_;
            break;
        case MD_SPAN_CODE:
            ++code_depth_;
            break;
        case MD_SPAN_A:
            enter_link(*static_cast<const MD_SPAN_A_DETAIL*>(detail));
            break;
        case MD_SPAN_IMG:
            if (image_depth_++ == 0)
                image_alt_.clear();
            break;
        default:
            break;
        }
    }

    void leave_span(MD_SPANTYPE type) {
        switch (type) {
        case MD_SPAN_STRONG:
            --bold_depth_;
            break;
        case MD_SPAN_EM:
            --italic_depth_;
            break;
        case MD_SPAN_DEL:
            --strike_depth_;
            break;
        case MD_SPAN_CODE:
            --code_depth_;
            break;
        case MD_SPAN_A:
            leave_link();
            break;
        case MD_SPAN_IMG:
            if (--image_depth_ == 0)
                add_image();
            break;
        default:
            break;
        }
    }

    void text(MD_TEXTTYPE type, std::string_view value) {
        note_source(value);
        if (leaf_ == Leaf::Code) {
            code_ += type == MD_TEXT_NULLCHAR ? kReplacement : value;
            return;
        }
        if (!table_ && leaf_ == Leaf::None)
            leaf_ = Leaf::Paragraph;
        const std::string shown = inline_text(type, value);
        if (image_depth_ > 0)
            image_alt_ += shown;
        else
            inline_.append(shown, current_look());
    }

private:
    enum class Leaf { None, Paragraph, Heading, Code };

    struct ListFrame {
        bool tight = true;
        unsigned next_number = 0;  // 0: işaretli liste
        int item_indent = 0;       // imin başladığı hücre
        int content_indent = 0;    // öğe metninin başladığı hücre
    };

    bool enter_container(MD_BLOCKTYPE type, const void* detail) {
        switch (type) {
        case MD_BLOCK_QUOTE:
            close_leaf();
            flush_marker();
            add_gap(peek_unread_line());  // alıntıdan önceki boşluk barsız
            quotes_.push_back(nesting_.size());
            nesting_ += 'Q';
            return true;
        case MD_BLOCK_UL:
            open_list(static_cast<const MD_BLOCK_UL_DETAIL*>(detail)->is_tight != 0, 0);
            return true;
        case MD_BLOCK_OL: {
            const auto* ordered = static_cast<const MD_BLOCK_OL_DETAIL*>(detail);
            // Numara 0'dan başlayabilir: işaretli listeyle karışmasın diye bir fazlası tutulur.
            open_list(ordered->is_tight != 0, ordered->start + 1);
            return true;
        }
        case MD_BLOCK_LI:
            close_leaf();
            flush_marker();
            open_item(*static_cast<const MD_BLOCK_LI_DETAIL*>(detail));
            return true;
        default:
            return false;
        }
    }

    bool enter_table_part(MD_BLOCKTYPE type, const void* detail) {
        switch (type) {
        case MD_BLOCK_TABLE:
            close_leaf();
            flush_marker();
            table_.emplace();
            return true;
        case MD_BLOCK_THEAD:
            in_table_head_ = true;
            return true;
        case MD_BLOCK_TBODY:
            in_table_head_ = false;
            return true;
        case MD_BLOCK_TR:
            open_row();
            return true;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            inline_ = {};
            if (in_table_head_)
                open_table().aligns.push_back(cell_align(static_cast<const MD_BLOCK_TD_DETAIL*>(detail)->align));
            return true;
        default:
            return false;
        }
    }

    bool leave_table_part(MD_BLOCKTYPE type) {
        switch (type) {
        case MD_BLOCK_TABLE:
            add_table_block();
            return true;
        case MD_BLOCK_THEAD:
            open_table().header_rows = open_table().rows.size();
            return true;
        case MD_BLOCK_TBODY:
            return true;
        case MD_BLOCK_TR:
            close_row();
            return true;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            close_cell();
            return true;
        default:
            return false;
        }
    }

    void open_list(bool tight, unsigned next_number) {
        close_leaf();
        flush_marker();
        const int base = lists_.empty() ? 0 : lists_.back().content_indent;
        const int indent = std::min(base, width_ / 4);  // düşman iç içelik metni sıfıra sıkıştırmasın
        lists_.push_back({tight, next_number, indent, indent});
        nesting_ += 'L';
    }

    void open_item(const MD_BLOCK_LI_DETAIL& item) {
        ListFrame& list = lists_.back();
        std::string number;
        if (list.next_number > 0)
            number = std::to_string(list.next_number++ - 1) + ". ";
        else
            number = kBullets[(lists_.size() - 1) % std::size(kBullets)];
        const std::string task = !item.is_task ? "" : (item.task_mark == ' ' ? "[ ] " : "[x] ");
        // Dev numara (9 hane) ya da dar panel: öğe metnine yer kalsın.
        if (view_.cells(number) + view_.cells(task) > width_ / 4)
            number = task.empty() ? std::string(kBullets[0]) : std::string{};
        pending_marker_ = InlineText{};
        pending_marker_->append(number, dim_look());
        pending_marker_->append(task, token_look(TokenKind::Keyword));
        list.content_indent = list.item_indent + view_.cells(pending_marker_->text);
    }

    // md4c tablo parçalarını yalnız MD_BLOCK_TABLE içinde üretir; orası table_'ı kurar. İhlal bug'dır: durur.
    TableData& open_table() {
        if (!table_)
            std::abort();
        return *table_;
    }

    void open_row() {
        row_line_.reset();
        skip_row_ = open_table().rows.size() >= limits_.table_rows;
        if (skip_row_)
            ++open_table().omitted_rows;
        else
            open_table().rows.emplace_back();
        row_cells_ = 0;
    }

    void close_cell() {
        ++row_cells_;
        if (!skip_row_ && open_table().rows.back().size() < limits_.table_columns)
            open_table().rows.back().push_back(std::move(inline_));
        inline_ = {};
    }

    void close_row() {
        open_table().column_count = std::max(open_table().column_count, row_cells_);
        if (skip_row_)
            return;
        const std::size_t previous =
            open_table().source_lines.empty() ? peek_unread_line() : open_table().source_lines.back() + 1;
        open_table().source_lines.push_back(row_line_.value_or(previous));
    }

    void begin_code(const MD_BLOCK_CODE_DETAIL& code) {
        leaf_ = Leaf::Code;
        code_.clear();
        code_fence_ = code.fence_char;
        code_language_ = fence_language(attribute_text(code.lang));
    }

    void enter_link(const MD_SPAN_A_DETAIL& link) {
        ++link_depth_;
        if (image_depth_ > 0)
            return;
        // Otomatik bağlantının metni adresin kendisidir: iki kez yazılmaz.
        link_href_ = link.is_autolink != 0 ? std::string{} : display_safe(decoded_attribute(link.href));
    }

    void leave_link() {
        --link_depth_;
        if (image_depth_ == 0 && !link_href_.empty())
            inline_.append(" (" + link_href_ + ")", dim_look());
        if (image_depth_ == 0)
            link_href_.clear();
    }

    void add_image() {
        const std::string shown = image_alt_.empty() ? core::tr("tui.md_image_no_alt")
                                                     : core::tr("tui.md_image", {{"alt", image_alt_}});
        inline_.append(shown, token_look(TokenKind::String));
        image_alt_.clear();
    }

    static std::string inline_text(MD_TEXTTYPE type, std::string_view value) {
        switch (type) {
        case MD_TEXT_NULLCHAR:
            return std::string(kReplacement);
        case MD_TEXT_BR:
            return "\n";
        case MD_TEXT_SOFTBR:
            return " ";
        case MD_TEXT_ENTITY:
            return display_safe(core::decode_html_entity(value));
        default:
            return display_safe(value);
        }
    }

    TextLook current_look() const {
        TextLook look = leaf_ == Leaf::Heading ? heading_look(heading_level_) : TextLook{};
        look.bold = look.bold || bold_depth_ > 0 || in_table_head_;
        look.italic = italic_depth_ > 0;
        look.strike = strike_depth_ > 0;
        if (link_depth_ > 0) {
            look.underline = true;
            look.token = TokenKind::Keyword;
        }
        if (code_depth_ > 0)
            look.token = TokenKind::Code;
        return look;
    }

    void note_source(std::string_view value) {
        const std::greater_equal<> at_or_after;
        const std::less_equal<> at_or_before;
        const char* const end = source_.data() + source_.size();
        if (!at_or_after(value.data(), source_.data()) || !at_or_before(value.data(), end))
            return;  // sabit dizgi: md4c'nin ürettiği girinti ya da satır sonu
        const auto offset = static_cast<std::size_t>(value.data() - source_.data());
        consumed_end_ = std::max(consumed_end_, offset + value.size());
        const std::size_t line = lines_.line_of(offset);
        if (table_ && !row_line_)
            row_line_ = line;
        if (!table_ && !leaf_line_) {
            // Kod bloğunun boş ilk satırları kaynağı göstermez ama sayılır.
            const std::size_t before =
                leaf_ == Leaf::Code ? static_cast<std::size_t>(std::ranges::count(code_, '\n')) : 0;
            leaf_line_ = line - std::min(before, line);
        }
    }

    // Tüketilen yerden sonraki ilk dolu satır; aşılan boşluklar bir daha taranmaz.
    std::size_t peek_unread_line() {
        std::size_t line = consumed_end_ == 0 ? 0 : lines_.line_of(consumed_end_ - 1) + 1;
        const auto is_blank = [](std::string_view text) {
            return std::ranges::all_of(
                text, [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
        };
        while (line + 1 < lines_.count() && is_blank(lines_.line_text(line))) {
            ++line;
            consumed_end_ = static_cast<std::size_t>(lines_.line_text(line).data() - source_.data());
        }
        return std::min(line, lines_.count() - 1);
    }

    std::size_t take_unread_line() {
        const std::size_t line = peek_unread_line();
        const std::string_view text = lines_.line_text(line);
        consumed_end_ = static_cast<std::size_t>(text.data() - source_.data()) + text.size();
        return line;
    }

    bool in_tight_list() const { return !lists_.empty() && lists_.back().tight; }

    // Kaplar dıştan içe (nesting_): alıntı çubuğu kendi kabının yerinde, liste girintisi kendi listesinin yerinde;
    // öğedeki alıntının çubuğu öğe metninin hizasındadır. `cells` liste girintisinin toplamıdır (çubuklar hariç).
    // Yalnız çizilen son çubuğa dek gezilir (çubuklar panelin çeyreği, kalan girinti sonda): düşman iç içelikte
    // satır başına iş derinlikle büyümez.
    InlineText indented_prefix(int cells, bool tail = true) const {
        constexpr int kWidthPerBar = 8;  // barlar panelin en çok çeyreğini alır
        const auto shown = std::min(quotes_.size(), static_cast<std::size_t>(std::max(width_ / kWidthPerBar, 1)));
        const std::size_t walk = shown == 0 ? 0 : quotes_[shown - 1] + 1;
        InlineText prefix;
        int at = 0;
        std::size_t list = 0;
        for (std::size_t i = 0; i < walk; ++i) {
            if (nesting_[i] == 'Q') {
                prefix.append(kQuoteBar, token_look(TokenKind::Comment));
                continue;
            }
            const int target = std::min(lists_[list++].content_indent, cells);
            prefix.append(std::string(static_cast<std::size_t>(std::max(target - at, 0)), ' '));
            at = std::max(at, target);
        }
        if (tail)
            prefix.append(std::string(static_cast<std::size_t>(std::max(cells - at, 0)), ' '));
        return prefix;
    }

    // Bloklar arası boş satır: yalnız çubuklar (aradaki liste girintisiyle), sonda boşluk yok.
    InlineText gap_prefix() const { return indented_prefix(std::numeric_limits<int>::max(), false); }

    int content_indent() const { return lists_.empty() ? 0 : lists_.back().content_indent; }

    // İlk satırın öneki: bekleyen öğe imi varsa burada asılı durur ve tüketilir.
    InlineText first_prefix() {
        if (!pending_marker_)
            return indented_prefix(content_indent());
        InlineText prefix = indented_prefix(lists_.back().item_indent);
        prefix.append(pending_marker_->text);
        const std::size_t base = prefix.text.size() - pending_marker_->text.size();
        for (StyledSpan span : pending_marker_->spans) {
            span.begin += base;
            span.end += base;
            prefix.spans.push_back(span);
        }
        pending_marker_.reset();
        return prefix;
    }

    void add_gap(std::size_t source_line) {
        if (pending_gap_ && !view_.empty())
            view_.add_line(gap_prefix(), source_line);
        pending_gap_ = false;
    }

    // İçinde blok açılmayan öğe ("- " ya da iç içe liste) imini kendi satırında gösterir.
    void flush_marker() {
        if (!pending_marker_)
            return;
        const std::size_t line = peek_unread_line();
        add_gap(line);
        view_.add_line(first_prefix(), line);
        pending_gap_ = !in_tight_list();
    }

    void add_leaf(const InlineText& content, std::size_t line) {
        add_gap(line);
        const InlineText first = first_prefix();
        view_.add(first, indented_prefix(content_indent()), content, line);
        pending_gap_ = !in_tight_list();
    }

    void add_rule() {
        const std::size_t line = take_unread_line();
        add_gap(line);
        InlineText prefix = first_prefix();
        const int cells = view_.width() - view_.cells(prefix.text);
        std::string rule;
        for (int i = 0; i < cells; ++i)
            rule += "─";
        prefix.append(rule, dim_look());
        view_.add_line(prefix, line);
        pending_gap_ = !in_tight_list();
    }

    void add_heading(std::size_t line) {
        add_gap(line);
        const std::string marker = std::string(static_cast<std::size_t>(heading_level_), '#') + " ";
        InlineText first = first_prefix();
        const int hanging = content_indent() + view_.cells(marker);
        first.append(marker, dim_look());
        view_.add(first, indented_prefix(hanging), inline_, line);
        pending_gap_ = !in_tight_list();
    }

    void add_code(std::size_t line) {
        add_gap(line);
        if (code_.ends_with('\n'))
            code_.pop_back();
        InlineText first = first_prefix();
        first.append(kCodeIndent);
        const int hanging = content_indent() + view_.cells(kCodeIndent);
        const InlineText next = indented_prefix(hanging);
        const InlineText* prefix = &first;
        SyntaxState state = kInitialSyntaxState;
        std::size_t start = 0;
        for (std::size_t index = 0;; ++index) {
            const std::size_t end = code_.find('\n', start);
            const std::string shown = code_line(std::string_view(code_).substr(start, end - start));
            view_.add(*prefix, next,
                      highlighted_line(code_language_, shown, state, token_look(TokenKind::Code),
                                       limits_.highlight_line_bytes),
                      line + index);
            prefix = &next;
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        code_.clear();
        pending_gap_ = !in_tight_list();
    }

    static std::string code_line(std::string_view raw) {
        std::string spaced;
        spaced.reserve(raw.size());
        for (const char c : raw) {
            if (c == '\t')
                spaced.append(kTabCells, ' ');
            else
                spaced += c;
        }
        return terminal_safe(spaced);
    }

    void add_table_block() {
        TableData table = std::move(open_table());
        table_.reset();
        in_table_head_ = false;
        const std::size_t line = table.source_lines.empty() ? peek_unread_line() : table.source_lines.front();
        add_gap(line);
        add_table(view_, table, indented_prefix(content_indent()));
        pending_gap_ = !in_tight_list();
    }

    void close_leaf() {
        if (leaf_ == Leaf::None)
            return;
        const std::size_t line = leaf_line_ ? *leaf_line_ : peek_unread_line();
        switch (leaf_) {
        case Leaf::Heading:
            add_heading(line);
            break;
        case Leaf::Code:
            add_code(line);
            break;
        default:
            add_leaf(inline_, line);
            break;
        }
        inline_ = {};
        leaf_ = Leaf::None;
        leaf_line_.reset();
    }

    std::string_view source_;
    SourceLines lines_;
    ViewBuilder view_;
    ViewLimits limits_;
    int width_;
    Leaf leaf_ = Leaf::None;
    std::optional<std::size_t> leaf_line_;
    InlineText inline_;
    int heading_level_ = 0;
    std::string code_;
    char code_fence_ = '\0';  // 0: girintili kod
    SyntaxLanguage code_language_ = SyntaxLanguage::Plain;
    std::string nesting_;  // açık kaplar dıştan içe: 'Q' alıntı, 'L' liste (pdf::Block::nesting gibi)
    std::vector<std::size_t> quotes_;  // nesting_'teki alıntıların yeri
    std::vector<ListFrame> lists_;
    std::optional<InlineText> pending_marker_;
    bool pending_gap_ = false;
    std::optional<TableData> table_;
    bool in_table_head_ = false;
    bool skip_row_ = false;
    std::size_t row_cells_ = 0;
    std::optional<std::size_t> row_line_;
    int bold_depth_ = 0;
    int italic_depth_ = 0;
    int strike_depth_ = 0;
    int code_depth_ = 0;
    int link_depth_ = 0;
    int image_depth_ = 0;
    std::string link_href_;
    std::string image_alt_;
    std::size_t consumed_end_ = 0;
};

MarkdownRenderer& renderer_of(void* userdata) {
    return *static_cast<MarkdownRenderer*>(userdata);
}

}  // namespace

RenderedView render_markdown(std::string_view source, int width, CellWidthFn cell_width,
                             const ViewLimits& limits) {
    const std::string_view body = source.starts_with(kBom) ? source.substr(kBom.size()) : source;
    MarkdownRenderer renderer(body, width, cell_width, limits);
    MD_PARSER parser{};
    // PDF dışa aktarımıyla aynı sözdizimi. MD_FLAG_NOHTML sözleşmedir: HTML düz metin kalır, not güvenilmez.
    parser.flags = MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH | MD_FLAG_PERMISSIVEAUTOLINKS |
                   MD_FLAG_NOHTML;
    parser.enter_block = [](MD_BLOCKTYPE type, void* detail, void* userdata) {
        renderer_of(userdata).enter_block(type, detail);
        return 0;
    };
    parser.leave_block = [](MD_BLOCKTYPE type, void*, void* userdata) {
        renderer_of(userdata).leave_block(type);
        return 0;
    };
    parser.enter_span = [](MD_SPANTYPE type, void* detail, void* userdata) {
        renderer_of(userdata).enter_span(type, detail);
        return 0;
    };
    parser.leave_span = [](MD_SPANTYPE type, void*, void* userdata) {
        renderer_of(userdata).leave_span(type);
        return 0;
    };
    parser.text = [](MD_TEXTTYPE type, const MD_CHAR* value, MD_SIZE size, void* userdata) {
        renderer_of(userdata).text(type, std::string_view(value, size));
        return 0;
    };
    // md4c yalnız bellek yetmezse hata verir: o ana kadarkiler gösterilir, görünüm bilgi amaçlıdır.
    md_parse(body.data(), static_cast<MD_SIZE>(body.size()), &parser, &renderer);
    return std::move(renderer).finish();
}

}  // namespace noteapp::tui
