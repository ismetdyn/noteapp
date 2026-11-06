#include "pdf/markdown_blocks.hpp"

#include "core/i18n.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <md4c.h>
#include <utility>

namespace noteapp::pdf {

namespace {

constexpr char kReplacementCharacter[] = "\xEF\xBF\xBD";

// Öznitelik metni; varlık parçaları çözülür (HTML'deki href gibi). md4c wiki hedefini zaten varlıksız kurar.
std::string attribute_text(const MD_ATTRIBUTE& attribute) {
    const std::string_view raw(attribute.text, attribute.size);
    std::string out;
    for (std::size_t i = 0; attribute.substr_offsets[i] < attribute.size; ++i) {
        const MD_OFFSET begin = attribute.substr_offsets[i];
        const std::string_view part = raw.substr(begin, attribute.substr_offsets[i + 1] - begin);
        if (attribute.substr_types[i] == MD_TEXT_ENTITY)
            out += core::decode_html_entity(part);
        else if (attribute.substr_types[i] == MD_TEXT_NULLCHAR)
            out += kReplacementCharacter;
        else
            out += part;
    }
    return out;
}

class BlockBuilder {
public:
    explicit BlockBuilder(MarkdownOptions options) : options_(options) {}

    std::vector<Block> finish() && {
        flush();
        return std::move(blocks_);
    }

    void enter_block(MD_BLOCKTYPE type, const void* detail) {
        switch (type) {
        case MD_BLOCK_DOC:
            break;
        case MD_BLOCK_QUOTE:
            open_container();
            ++quote_depth_;
            nesting_ += 'Q';
            break;
        case MD_BLOCK_UL:
            open_container();
            lists_.push_back({false, 0});
            nesting_ += 'L';
            list_opened_ = true;
            break;
        case MD_BLOCK_OL:
            open_container();
            lists_.push_back({true, static_cast<const MD_BLOCK_OL_DETAIL*>(detail)->start});
            nesting_ += 'L';
            list_opened_ = true;
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
        case MD_BLOCK_CODE: {
            flush();
            open(Block::Kind::Code);
            const MD_ATTRIBUTE& lang = static_cast<const MD_BLOCK_CODE_DETAIL*>(detail)->lang;
            current_.language.assign(lang.text == nullptr ? "" : lang.text, lang.size);
            break;
        }
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
            if (in_table_head_)
                current_.aligns.push_back(align_of(static_cast<const MD_BLOCK_TD_DETAIL*>(detail)->align));
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
            nesting_.pop_back();
            break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            flush();
            lists_.pop_back();
            nesting_.pop_back();
            break;
        case MD_BLOCK_LI:
            flush();
            flush_marker();  // boş öğe
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

    void enter_span(MD_SPANTYPE type, const void* detail) {
        switch (type) {
        case MD_SPAN_A:
            if (options_.links)
                links_.push_back({attribute_text(static_cast<const MD_SPAN_A_DETAIL*>(detail)->href), false});
            break;
        case MD_SPAN_WIKILINK:
            links_.push_back({attribute_text(static_cast<const MD_SPAN_WIKILINK_DETAIL*>(detail)->target), true});
            break;
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
            // İç içe görselde yalnız dıştaki yer tutucuyu belirler. Alt metin görselin dışındaki biçimle, içindeki
            // vurgu ve bağlantı olmadan yazılır (HTML alt="..." düz metindir).
            if (image_depth_++ == 0) {
                has_image_text_ = false;
                image_style_ = current_style();
                image_link_ = links_.empty() ? Link{} : links_.back();
            }
            break;
        case MD_SPAN_DEL:
            ++strike_depth_;
            break;
        default:  // PDF'te bağlantı: hedef atılır, metin kalır
            break;
        }
    }

    void leave_span(MD_SPANTYPE type) {
        switch (type) {
        case MD_SPAN_A:
            if (options_.links)
                links_.pop_back();
            break;
        case MD_SPAN_WIKILINK:  // yalnız options_.links ile ayrıştırılır
            links_.pop_back();
            break;
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
            if (image_depth_ == 1 && !has_image_text_)
                append(core::tr("pdf.image_placeholder"));
            --image_depth_;
            break;
        case MD_SPAN_DEL:
            --strike_depth_;
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
            append(image_depth_ > 0 ? " " : "\n");  // alt metin tek satırdır (HTML alt="...")
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
        current_.nesting = nesting_;
        current_.list_start = std::exchange(list_opened_, false);
        current_.marker = std::exchange(pending_marker_, {});
        is_open_ = true;
    }

    // Öğe içinde blok açılmadan kap açılıyor ya da öğe boş bitti: im kendi boş paragrafında (TUI gibi); yoksa iç
    // listenin imi onu ezerdi.
    void flush_marker() {
        if (pending_marker_.empty())
            return;
        open(Block::Kind::Paragraph);
        flush();
    }

    void open_container() {
        flush();
        flush_marker();
    }

    static Align align_of(MD_ALIGN align) {
        switch (align) {
        case MD_ALIGN_LEFT:
            return Align::Left;
        case MD_ALIGN_CENTER:
            return Align::Center;
        case MD_ALIGN_RIGHT:
            return Align::Right;
        default:
            return Align::Default;
        }
    }

    void flush() {
        if (is_open_)
            blocks_.push_back(std::move(current_));
        is_open_ = false;
        current_ = Block{};
    }

    std::string list_marker(const MD_BLOCK_LI_DETAIL& item) {
        std::string marker;
        if (!lists_.empty() && lists_.back().ordered)
            marker = std::to_string(lists_.back().next++) + ". ";
        else
            marker = kListBullets[(std::max<std::size_t>(lists_.size(), 1) - 1) % std::size(kListBullets)];
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
        const TextStyle style = image_depth_ > 0 ? image_style_ : current_style();
        const Link link = image_depth_ > 0 ? image_link_ : links_.empty() ? Link{} : links_.back();
        std::vector<Span>& spans =
            in_table_cell_ && !current_.rows.empty() ? current_.rows.back().cells.back() : current_.spans;
        if (!spans.empty() && spans.back().style == style && spans.back().link == link.target &&
            spans.back().wiki == link.wiki)
            spans.back().text += value;
        else
            spans.push_back({std::string(value), style, link.target, link.wiki});
    }

    TextStyle current_style() const {
        return {bold_depth_ > 0, italic_depth_ > 0, mono_depth_ > 0 || current_.kind == Block::Kind::Code,
                strike_depth_ > 0};
    }

    struct Link {
        std::string target;
        bool wiki = false;
    };
    struct List {
        bool ordered = false;
        unsigned next = 0;  // sıradaki numara (sıralı listede; 0'dan başlayabilir)
    };

    MarkdownOptions options_;
    std::vector<Link> links_;  // iç içe bağlantıda en içteki geçerli
    std::vector<Block> blocks_;
    Block current_;
    bool is_open_ = false;
    std::vector<List> lists_;
    std::string nesting_;  // açık kaplar dıştan içe (Block::nesting)
    bool list_opened_ = false;  // liste açıldı, ilk bloğu henüz yok
    std::string pending_marker_;
    int quote_depth_ = 0;
    int bold_depth_ = 0;
    int italic_depth_ = 0;
    int mono_depth_ = 0;
    int strike_depth_ = 0;
    int image_depth_ = 0;
    TextStyle image_style_;  // görselin açıldığı yerdeki biçim ve bağlantı: alt metin bununla yazılır
    Link image_link_;
    bool has_image_text_ = false;
    bool in_table_head_ = false;
    bool in_table_cell_ = false;
};

BlockBuilder& builder_of(void* userdata) {
    return *static_cast<BlockBuilder*>(userdata);
}

}  // namespace

std::vector<Block> parse_markdown(std::string_view text, MarkdownOptions options) {
    BlockBuilder builder(options);
    MD_PARSER parser{};
    // MD_DIALECT_GITHUB'ın dipnot/uyarı blokları plan kapsamında değil; yalnız gereken bayraklar.
    // MD_FLAG_NOHTML sözleşmedir: not içeriği güvenilmez.
    parser.flags = MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH | MD_FLAG_PERMISSIVEAUTOLINKS |
                   MD_FLAG_NOHTML;
    if (options.links)
        parser.flags |= MD_FLAG_WIKILINKS;
    parser.enter_block = [](MD_BLOCKTYPE type, void* detail, void* userdata) {
        builder_of(userdata).enter_block(type, detail);
        return 0;
    };
    parser.leave_block = [](MD_BLOCKTYPE type, void*, void* userdata) {
        builder_of(userdata).leave_block(type);
        return 0;
    };
    parser.enter_span = [](MD_SPANTYPE type, void* detail, void* userdata) {
        builder_of(userdata).enter_span(type, detail);
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
