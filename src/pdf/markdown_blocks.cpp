#include "pdf/markdown_blocks.hpp"

#include "core/i18n.hpp"
#include "core/text.hpp"

#include <md4c.h>
#include <utility>

namespace noteapp::pdf {

namespace {

constexpr char kReplacementCharacter[] = "\xEF\xBF\xBD";

class BlockBuilder {
public:
    std::vector<Block> finish() && {
        flush();
        return std::move(blocks_);
    }

    void enter_block(MD_BLOCKTYPE type, const void* detail) {
        switch (type) {
        case MD_BLOCK_DOC:
            break;
        case MD_BLOCK_QUOTE:
            flush();
            ++quote_depth_;
            break;
        case MD_BLOCK_UL:
            flush();
            lists_.push_back(0);
            break;
        case MD_BLOCK_OL:
            flush();
            lists_.push_back(static_cast<const MD_BLOCK_OL_DETAIL*>(detail)->start);
            break;
        case MD_BLOCK_LI:
            flush();
            pending_marker_ = list_marker(*static_cast<const MD_BLOCK_LI_DETAIL*>(detail));
            break;
        case MD_BLOCK_HR:
            flush();
            open(Block::Kind::Rule);
            flush();
            break;
        case MD_BLOCK_H:
            flush();
            open(Block::Kind::Heading);
            current_.heading_level = static_cast<int>(static_cast<const MD_BLOCK_H_DETAIL*>(detail)->level);
            break;
        case MD_BLOCK_CODE:
            flush();
            open(Block::Kind::Code);
            break;
        case MD_BLOCK_TABLE:
            flush();
            open(Block::Kind::Table);
            break;
        case MD_BLOCK_THEAD:
            in_table_head_ = true;
            break;
        case MD_BLOCK_TBODY:
            in_table_head_ = false;
            break;
        case MD_BLOCK_TR:
            current_.rows.push_back({{}, in_table_head_});
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            current_.rows.back().cells.emplace_back();
            in_table_cell_ = true;
            break;
        default:  // P ve kapalı bayraklara ait türler: metin yeni paragrafta toplanır
            flush();
            open(Block::Kind::Paragraph);
            break;
        }
    }

    void leave_block(MD_BLOCKTYPE type) {
        switch (type) {
        case MD_BLOCK_QUOTE:
            flush();
            --quote_depth_;
            break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            flush();
            lists_.pop_back();
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            in_table_cell_ = false;
            break;
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
        case MD_BLOCK_TR:
        case MD_BLOCK_DOC:
            break;
        default:
            flush();
            break;
        }
    }

    void enter_span(MD_SPANTYPE type) {
        switch (type) {
        case MD_SPAN_STRONG:
            ++bold_depth_;
            break;
        case MD_SPAN_EM:
            ++italic_depth_;
            break;
        case MD_SPAN_CODE:
            ++mono_depth_;
            break;
        case MD_SPAN_IMG:
            // İç içe görselde yalnız dıştaki yer tutucuyu belirler.
            if (image_depth_++ == 0)
                has_image_text_ = false;
            break;
        default:  // bağlantı ve üstü çizili: hedef/süs atılır, metin kalır
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
        case MD_SPAN_CODE:
            --mono_depth_;
            break;
        case MD_SPAN_IMG:
            if (--image_depth_ == 0 && !has_image_text_)
                append(core::tr("pdf.image_placeholder"));
            break;
        default:
            break;
        }
    }

    void text(MD_TEXTTYPE type, std::string_view value) {
        switch (type) {
        case MD_TEXT_NULLCHAR:
            append(kReplacementCharacter);
            break;
        case MD_TEXT_BR:
            append("\n");
            break;
        case MD_TEXT_SOFTBR:
            append(" ");
            break;
        case MD_TEXT_ENTITY:
            append(core::decode_html_entity(value));
            break;
        default:  // NORMAL, CODE ve (NOHTML altında yalnız düz metin olan) HTML
            append(value);
            break;
        }
    }

private:
    void open(Block::Kind kind) {
        current_ = Block{};
        current_.kind = kind;
        current_.list_depth = static_cast<int>(lists_.size());
        current_.quote_depth = quote_depth_;
        current_.marker = std::exchange(pending_marker_, {});
        is_open_ = true;
    }

    void flush() {
        if (is_open_)
            blocks_.push_back(std::move(current_));
        is_open_ = false;
        current_ = Block{};
    }

    std::string list_marker(const MD_BLOCK_LI_DETAIL& item) {
        std::string marker;
        if (!lists_.empty() && lists_.back() > 0)
            marker = std::to_string(lists_.back()++) + ". ";
        else
            marker = "• ";
        if (item.is_task)
            marker += item.task_mark == ' ' ? "[ ] " : "[x] ";
        return marker;
    }

    // Sıkı listede md4c P açmaz: metin doğrudan LI içinde gelir.
    void append(std::string_view value) {
        if (!is_open_)
            open(Block::Kind::Paragraph);
        if (image_depth_ > 0)
            has_image_text_ = true;
        const TextStyle style{bold_depth_ > 0, italic_depth_ > 0,
                              mono_depth_ > 0 || current_.kind == Block::Kind::Code};
        std::vector<Span>& spans =
            in_table_cell_ && !current_.rows.empty() ? current_.rows.back().cells.back() : current_.spans;
        if (!spans.empty() && spans.back().style == style)
            spans.back().text += value;
        else
            spans.push_back({std::string(value), style});
    }

    std::vector<Block> blocks_;
    Block current_;
    bool is_open_ = false;
    std::vector<unsigned> lists_;  // sıradaki numara; 0 = işaretli liste
    std::string pending_marker_;
    int quote_depth_ = 0;
    int bold_depth_ = 0;
    int italic_depth_ = 0;
    int mono_depth_ = 0;
    int image_depth_ = 0;
    bool has_image_text_ = false;
    bool in_table_head_ = false;
    bool in_table_cell_ = false;
};

BlockBuilder& builder_of(void* userdata) {
    return *static_cast<BlockBuilder*>(userdata);
}

}  // namespace

std::vector<Block> parse_markdown(std::string_view text) {
    BlockBuilder builder;
    MD_PARSER parser{};
    // MD_DIALECT_GITHUB'ın dipnot/uyarı blokları plan kapsamında değil; yalnız gereken bayraklar.
    // MD_FLAG_NOHTML sözleşmedir: not içeriği güvenilmez.
    parser.flags = MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH | MD_FLAG_PERMISSIVEAUTOLINKS |
                   MD_FLAG_NOHTML;
    parser.enter_block = [](MD_BLOCKTYPE type, void* detail, void* userdata) {
        builder_of(userdata).enter_block(type, detail);
        return 0;
    };
    parser.leave_block = [](MD_BLOCKTYPE type, void*, void* userdata) {
        builder_of(userdata).leave_block(type);
        return 0;
    };
    parser.enter_span = [](MD_SPANTYPE type, void*, void* userdata) {
        builder_of(userdata).enter_span(type);
        return 0;
    };
    parser.leave_span = [](MD_SPANTYPE type, void*, void* userdata) {
        builder_of(userdata).leave_span(type);
        return 0;
    };
    parser.text = [](MD_TEXTTYPE type, const MD_CHAR* value, MD_SIZE size, void* userdata) {
        builder_of(userdata).text(type, std::string_view(value, size));
        return 0;
    };
    // md4c yalnız bellek yetmezse hata verir; o ana kadarkiler çizilir, bad_alloc değilse dışa aktarma sürer.
    md_parse(text.data(), static_cast<MD_SIZE>(text.size()), &parser, &builder);
    return std::move(builder).finish();
}

}  // namespace noteapp::pdf
