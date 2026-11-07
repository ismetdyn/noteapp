// NY-P markdown önizleme uygunluk koşumu. CommonMark spec örnekleri ve md4c'nin GFM uzantı örnekleri (tablo, görev
// listesi, üstü çizili, izinli otomatik bağlantı; md4c arşiviyle gelir, SHA256 sabit) md4c'nin kendi HTML çizicisinden
// (md_html) geçer. Çıktı küçük bir ağaca ayrıştırılır ve iki arayüzün yazılı kuralıyla normal bir biçime izdüşürülür:
//   GUI: blok modeli (pdf::parse_markdown, GUI önizlemesinin girdisi) satır başına bir blok: kap yolu|im|tür|içerik.
//   TUI: biçimli satırlar (render_markdown) önek, im ve içerikle, biçim bayraklarıyla.
// Gerçek çıktı aynı biçime indirilir; farklar örneğin spec bölümüne göre kategoriye sayılır. Kategori başına kalan fark
// tavanı aşarsa test düşer (tavanlar ve gerekçeleri aşağıda). Ayrıntılı fark dökümü için
// NOTEAPP_MD_CONFORMANCE_REPORT=<dosya>.
#include "core/i18n.hpp"
#include "pdf/markdown_blocks.hpp"
#include "tui/editor_view.hpp"
#include "tui/markdown_view.hpp"
#include "tui/styled_view.hpp"

#include <algorithm>
#include <cstdlib>
#include <doctest/doctest.h>
#include <fstream>
#include <map>
#include <md4c-html.h>
#include <md4c.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace noteapp;

namespace {

// İki arayüzün ortak sözdizimi (markdown_view.cpp, markdown_blocks.cpp), [[wiki]] bağlantısı dahil.
constexpr unsigned kPreviewFlags = MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH |
                                   MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_NOHTML | MD_FLAG_WIKILINKS;
constexpr int kTuiWidth = 1000;  // örnekler sarılmasın: fark yerleşimden değil biçimden gelsin

// ---------------------------------------------------------------------------------------------------------------
// Örnekler

struct Example {
    std::string file;
    int number = 0;
    std::string category;
    std::string markdown;
};

std::string category_of(std::string_view file, std::string_view section) {
    if (file != "spec.txt") {
        if (file == "spec-tables.txt")
            return "tables";
        if (file == "spec-tasklists.txt")
            return "task lists";
        if (file == "spec-strikethrough.txt")
            return "strikethrough";
        return "autolinks";
    }
    static const std::map<std::string, std::string, std::less<>> kSections = {
        {"ATX headings", "headings"},
        {"Setext headings", "headings"},
        {"Thematic breaks", "rules"},
        {"Indented code blocks", "code blocks"},
        {"Fenced code blocks", "code blocks"},
        {"Code spans", "inline code"},
        {"Block quotes", "block quotes"},
        {"List items", "lists/nesting"},
        {"Lists", "lists/nesting"},
        {"Emphasis and strong emphasis", "emphasis"},
        {"Links", "links/images"},
        {"Images", "links/images"},
        {"Link reference definitions", "links/images"},
        {"Autolinks", "autolinks"},
        {"Entity and numeric character references", "entities/escapes"},
        {"Backslash escapes", "entities/escapes"},
        {"Tabs", "entities/escapes"},
        {"Insecure characters", "entities/escapes"},
        {"Textual content", "entities/escapes"},
        {"HTML blocks", "raw HTML as text"},
        {"Raw HTML", "raw HTML as text"},
        {"Hard line breaks", "line breaks"},
        {"Soft line breaks", "line breaks"},
        {"Paragraphs", "paragraphs"},
        {"Blank lines", "paragraphs"},
        {"Precedence", "paragraphs"},
    };
    const auto found = kSections.find(section);
    return found == kSections.end() ? "other" : found->second;
}

std::vector<Example> load_examples(const std::string& file) {
    std::ifstream in(std::string(NOTEAPP_MD4C_TEST_DIR) + "/" + file, std::ios::binary);
    REQUIRE_MESSAGE(in, "md4c test file missing: ", file);
    constexpr std::string_view kFence = "```````````````````````````````` example";
    constexpr std::string_view kArrow = "→";  // spec sekmeyi görünür yazar
    std::vector<Example> examples;
    std::string section;
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        if (line.starts_with("## "))
            section = line.substr(3);
        if (line != kFence)
            continue;
        Example example{file, ++number, category_of(file, section), {}};
        while (std::getline(in, line) && line != ".") {
            for (std::size_t at = line.find(kArrow); at != std::string::npos; at = line.find(kArrow, at))
                line.replace(at, kArrow.size(), "\t");
            example.markdown += line + "\n";
        }
        while (std::getline(in, line) && !line.starts_with("````````````````````````````````"))
            ;
        examples.push_back(std::move(example));
    }
    return examples;
}

// ---------------------------------------------------------------------------------------------------------------
// md_html çıktısının ağacı. md4c HTML'i düzgün üretir (NOHTML: ham HTML kaçışlanmış metindir), ayrıştırıcı küçük kalır.

struct Node {
    std::string tag;  // boş: metin
    std::string text;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<Node> children;

    std::optional<std::string> attr(std::string_view name) const {
        for (const auto& [key, value] : attrs)
            if (key == name)
                return value;
        return std::nullopt;
    }
};

std::string unescape(std::string_view text) {
    static constexpr std::pair<std::string_view, std::string_view> kEntities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#x27;", "'"}};
    std::string out;
    for (std::size_t at = 0; at < text.size();) {
        bool replaced = false;
        for (const auto& [entity, value] : kEntities)
            if (text.substr(at).starts_with(entity)) {
                out += value;
                at += entity.size();
                replaced = true;
                break;
            }
        if (!replaced)
            out += text[at++];
    }
    return out;
}

Node parse_html(std::string_view html) {
    Node root{"root", {}, {}, {}};
    std::vector<Node*> stack{&root};  // yalnız en üstteki büyür: alttakilerin işaretçisi geçerli kalır
    for (std::size_t at = 0; at < html.size();) {
        if (html[at] != '<') {
            const std::size_t end = std::min(html.find('<', at), html.size());
            stack.back()->children.push_back({{}, unescape(html.substr(at, end - at)), {}, {}});
            at = end;
            continue;
        }
        std::size_t end = at + 1;
        for (bool quoted = false; end < html.size() && (quoted || html[end] != '>'); ++end)
            quoted = html[end] == '"' ? !quoted : quoted;
        std::string_view inside = html.substr(at + 1, end - at - 1);
        at = end + 1;
        if (inside.starts_with('/')) {
            const std::string name(inside.substr(1));
            while (stack.size() > 1 && stack.back()->tag != name)
                stack.pop_back();
            if (stack.size() > 1)
                stack.pop_back();
            continue;
        }
        Node node;
        const std::size_t name_end = std::min(inside.find(' '), inside.size());
        node.tag = std::string(inside.substr(0, name_end));
        inside.remove_prefix(name_end);
        while (!inside.empty()) {
            inside.remove_prefix(std::min(inside.find_first_not_of(" /"), inside.size()));
            if (inside.empty())
                break;
            const std::size_t key_end = std::min(inside.find_first_of("= "), inside.size());
            std::string key(inside.substr(0, key_end));
            inside.remove_prefix(key_end);
            std::string value;
            if (inside.starts_with("=\"")) {
                const std::size_t close = inside.find('"', 2);
                value = unescape(inside.substr(2, close - 2));
                inside.remove_prefix(close + 1);
            }
            node.attrs.emplace_back(std::move(key), std::move(value));
        }
        const bool is_void = node.tag == "br" || node.tag == "hr" || node.tag == "img" || node.tag == "input";
        stack.back()->children.push_back(std::move(node));
        if (!is_void)
            stack.push_back(&stack.back()->children.back());
    }
    return root;
}

std::string reference_html(std::string_view markdown, unsigned flags) {
    std::string html;
    md_html(
        markdown.data(), static_cast<MD_SIZE>(markdown.size()),
        [](const MD_CHAR* text, MD_SIZE size, void* out) { static_cast<std::string*>(out)->append(text, size); },
        &html, flags, 0);
    return html;
}

// md_html'in href kaçışı (md4c-html.c render_url_escaped): harf, rakam ve "~-_.+!*(),%#@?=;:/$&" dışı %XX.
std::string url_escaped(std::string_view url) {
    constexpr std::string_view kKept = "~-_.+!*(),%#@?=;:/$&";
    constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string out;
    for (const char c : url) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0 && byte < 0x80)
            out += c;
        else if (kKept.find(c) != std::string_view::npos)
            out += c;
        else {
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 0xF];
        }
    }
    return out;
}

// HTML'deki gibi madde imi derinliğe göre döner (disc, circle, square); iki arayüzün kuralı.
std::string bullet(std::string_view path) {
    static constexpr std::string_view kBullets[] = {"• ", "◦ ", "▪ "};
    const auto depth = static_cast<std::size_t>(std::ranges::count(path, 'L'));
    return std::string(kBullets[(depth - 1) % std::size(kBullets)]);
}

bool is_heading(std::string_view tag) {
    return tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6';
}

bool is_block(std::string_view tag) {
    return tag == "p" || is_heading(tag) || tag == "pre" || tag == "hr" || tag == "table" || tag == "blockquote" ||
           tag == "ul" || tag == "ol";
}

bool is_blank_text(const Node& node) {
    return node.tag.empty() && node.text.find_first_not_of(" \t\n") == std::string::npos;
}

std::string text_of(const Node& node) {
    std::string out = node.text;
    for (const Node& child : node.children)
        out += text_of(child);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Biçimli metin: koşu (metin, bayraklar). Serileştirme bitişik aynı bayraklıları birleştirir, "\n" zorunlu satır sonu.

struct Run {
    std::string text;
    std::string flags;
};
using Runs = std::vector<Run>;

void add(Runs& runs, std::string_view text, const std::string& flags = {}) {
    if (text.empty())
        return;
    if (!runs.empty() && runs.back().flags == flags)
        runs.back().text += text;
    else
        runs.push_back({std::string(text), flags});
}

// Baştaki ve sondaki biçimsiz boşluk atılır; biçimli boşluk (` ` kod) görünür kalır.
void trim(Runs& runs) {
    while (!runs.empty() && runs.front().flags.empty()) {
        std::string& text = runs.front().text;
        text.erase(0, std::min(text.find_first_not_of(" \n"), text.size()));
        if (!text.empty())
            break;
        runs.erase(runs.begin());
    }
    while (!runs.empty() && runs.back().flags.empty()) {
        std::string& text = runs.back().text;
        const std::size_t last = text.find_last_not_of(" \n");
        text.erase(last == std::string::npos ? 0 : last + 1);
        if (!text.empty())
            break;
        runs.pop_back();
    }
}

std::string serialize(const Runs& runs) {
    Runs merged;
    for (const Run& run : runs)
        add(merged, run.text, run.flags);
    std::string out;
    for (const Run& run : merged) {
        std::string text;
        for (const char c : run.text)
            text += c == '\n' ? std::string("⏎") : std::string(1, c);
        out += run.flags.empty() ? text : "«" + run.flags + ":" + text + "»";
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// GUI: blok modeli. Satır: kap yolu (Q alıntı, L liste; dıştan içe) | im | tür | içerik.

struct GuiStyle {
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
    std::string link;
    bool wiki = false;

    std::string flags() const {
        std::string out;
        out += bold ? "b" : "";
        out += italic ? "i" : "";
        out += strike ? "s" : "";
        out += code ? "c" : "";
        if (!link.empty())
            out += (wiki ? " w=" : " a=") + link;
        return out;
    }
};

class GuiProjector {
public:
    std::vector<std::string> run(const Node& root) {
        blocks(root);
        return std::move(out_);
    }

private:
    void blocks(const Node& parent) {
        for (const Node& node : parent.children)
            block(node);
    }

    void block(const Node& node) {
        if (node.tag.empty())
            return;
        if (node.tag == "blockquote" || node.tag == "ul" || node.tag == "ol")
            flush_marker();
        if (node.tag == "blockquote") {
            path_ += 'Q';
            blocks(node);
            path_.pop_back();
        } else if (node.tag == "ul" || node.tag == "ol") {
            list(node);
        } else if (node.tag == "p") {
            Runs runs;
            inlines(node, {}, runs);
            leaf("p", runs);
        } else if (is_heading(node.tag)) {
            Runs runs;
            inlines(node, {}, runs);
            leaf(node.tag, runs);
        } else if (node.tag == "pre") {
            std::string code = text_of(node);
            if (code.ends_with('\n'))
                code.pop_back();
            emit("code", serialize({{code, {}}}));
        } else if (node.tag == "hr") {
            emit("hr", {});
        } else if (node.tag == "table") {
            table(node);
        }
    }

    void list(const Node& node) {
        const bool ordered = node.tag == "ol";
        unsigned number = ordered ? static_cast<unsigned>(std::stoul(node.attr("start").value_or("1"))) : 0;
        path_ += 'L';
        for (const Node& item : node.children) {
            if (item.tag != "li")
                continue;
            marker_ = ordered ? std::to_string(number++) + ". " : bullet(path_);
            for (const Node& child : item.children)
                if (child.tag == "input")
                    marker_ += child.attr("checked") ? "[x] " : "[ ] ";
            item_content(item);
            flush_marker();  // boş öğe: im kendi satırında
        }
        path_.pop_back();
    }

    // Sıkı listede md4c P açmaz: öğedeki ardışık satır içi düğümler bir paragraftır.
    void item_content(const Node& item) {
        Runs runs;
        bool has_inline = false;
        const auto close_paragraph = [&] {
            if (has_inline) {
                trim(runs);
                if (!runs.empty())
                    leaf("p", runs);
            }
            runs.clear();
            has_inline = false;
        };
        for (const Node& child : item.children) {
            if (child.tag == "input")
                continue;
            if (is_block(child.tag)) {
                close_paragraph();
                block(child);
                continue;
            }
            if (is_blank_text(child) && !has_inline)
                continue;
            has_inline = true;
            inline_node(child, {}, runs);
        }
        close_paragraph();
    }

    void table(const Node& node) {
        std::string content;
        for (const Node& part : node.children)
            for (const Node& row : part.children) {
                if (row.tag != "tr")
                    continue;
                content += part.tag == "thead" ? "H" : "D";
                for (const Node& cell : row.children) {
                    if (cell.tag != "th" && cell.tag != "td")
                        continue;
                    const std::string align = cell.attr("align").value_or("");
                    Runs runs;
                    inlines(cell, {}, runs);
                    trim(runs);
                    content += "|" + (align.empty() ? std::string{} : align.substr(0, 1)) + ":" + serialize(runs);
                }
                content += ";";
            }
        emit("table", content);
    }

    void inlines(const Node& parent, const GuiStyle& style, Runs& runs) {
        for (const Node& child : parent.children)
            inline_node(child, style, runs);
    }

    void inline_node(const Node& node, GuiStyle style, Runs& runs) {
        if (node.tag.empty()) {
            std::string text = node.text;
            // md_html satır sonunu yumuşak kırılımda ve <br>'den sonra yazar; GUI'de yumuşak kırılım boşluktur.
            if (after_break_ && text.starts_with('\n'))
                text.erase(0, 1);
            after_break_ = false;
            std::ranges::replace(text, '\n', ' ');
            add(runs, text, style.flags());
            return;
        }
        after_break_ = false;
        if (node.tag == "br") {
            add(runs, "\n", style.flags());
            after_break_ = true;
            return;
        }
        if (node.tag == "img") {  // GUI kuralı: görsel yerine alt metni (yoksa yer tutucu), süssüz
            const std::string alt = node.attr("alt").value_or("");
            add(runs, alt.empty() ? core::tr("pdf.image_placeholder") : alt, style.flags());
            return;
        }
        if (node.tag == "strong")
            style.bold = true;
        else if (node.tag == "em")
            style.italic = true;
        else if (node.tag == "del")
            style.strike = true;
        else if (node.tag == "code")
            style.code = true;
        else if (node.tag == "a") {
            style.link = node.attr("href").value_or("");
            style.wiki = false;
        } else if (node.tag == "x-wikilink") {
            style.link = node.attr("data-target").value_or("");
            style.wiki = true;
        }
        inlines(node, style, runs);
    }

    void leaf(const std::string& kind, Runs runs) {
        trim(runs);
        emit(kind, serialize(runs));
    }

    void emit(const std::string& kind, const std::string& content) {
        out_.push_back(path_ + "|" + std::exchange(marker_, {}) + "|" + kind + "|" + content);
    }

    void flush_marker() {
        if (!marker_.empty())
            emit("p", {});
    }

    std::string path_;
    std::string marker_;
    bool after_break_ = false;
    std::vector<std::string> out_;
};

std::vector<std::string> gui_actual(std::string_view markdown) {
    std::vector<std::string> out;
    std::string path;  // blok yolu farkla gelir: önceki yolun ilk nesting_kept kabı + nesting_opened
    for (const pdf::Block& block : pdf::parse_markdown(markdown, {.links = true})) {
        path.resize(block.nesting_kept);
        path += block.nesting_opened;
        const auto runs_of = [](const std::vector<pdf::Span>& spans) {
            Runs runs;
            for (const pdf::Span& span : spans) {
                GuiStyle style{span.style.bold, span.style.italic, span.style.strike, span.style.mono, {}, span.wiki};
                style.link = span.wiki ? span.link : url_escaped(span.link);
                add(runs, span.text, style.flags());
            }
            trim(runs);
            return runs;
        };
        std::string kind;
        std::string content;
        switch (block.kind) {
        case pdf::Block::Kind::Paragraph:
            kind = "p";
            content = serialize(runs_of(block.spans));
            break;
        case pdf::Block::Kind::Heading:
            kind = "h" + std::to_string(block.heading_level);
            content = serialize(runs_of(block.spans));
            break;
        case pdf::Block::Kind::Code: {
            kind = "code";
            std::string code;
            for (const pdf::Span& span : block.spans)
                code += span.text;
            if (code.ends_with('\n'))
                code.pop_back();
            content = serialize({{code, {}}});
            break;
        }
        case pdf::Block::Kind::Rule:
            kind = "hr";
            break;
        case pdf::Block::Kind::Table:
            kind = "table";
            for (const pdf::TableRow& row : block.rows) {
                content += row.is_header ? "H" : "D";
                for (std::size_t column = 0; column < row.cells.size(); ++column) {
                    static constexpr std::string_view kAligns[] = {"", "l", "c", "r"};
                    const pdf::Align align = column < block.aligns.size() ? block.aligns[column] : pdf::Align::Default;
                    content += "|" + std::string(kAligns[static_cast<std::size_t>(align)]) + ":" +
                               serialize(runs_of(row.cells[column]));
                }
                content += ";";
            }
            break;
        }
        out.push_back(path + "|" + block.marker + "|" + kind + "|" + content);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// TUI: biçimli satırlar. Bayraklar TextLook'tan: b kalın, i eğik, s üstü çizili, d soluk, a bağlantı (altı çizili
// anahtar sözcük), k anahtar sözcük (görev imi), c kod, g dizge (görsel), q yorum (alıntı çubuğu).

std::string tui_flags(const tui::TextLook& look) {
    std::string out;
    out += look.bold ? "b" : "";
    out += look.italic ? "i" : "";
    out += look.strike ? "s" : "";
    out += look.dim ? "d" : "";
    out += look.token == tui::TokenKind::Keyword ? (look.underline ? "a" : "k") : "";
    out += look.token == tui::TokenKind::Code ? "c" : "";
    out += look.token == tui::TokenKind::String ? "g" : "";
    out += look.token == tui::TokenKind::Comment ? "q" : "";
    return out;
}

struct TuiLine {
    Runs runs;
    bool code = false;  // kod bloğu satırı: renklendirme dile bağlı, yalnız metin karşılaştırılır
};

std::string text_only(const Runs& runs) {
    std::string out;
    for (const Run& run : runs)
        out += run.text;
    return out;
}

// Sondaki düz boşluk atılır; yalnız çubuk ve boşluktan oluşan (bloklar arası) satır boştur.
std::optional<std::string> normalized(const TuiLine& line) {
    Runs runs = line.runs;
    while (!runs.empty() && runs.back().flags.empty()) {
        std::string& text = runs.back().text;
        text.erase(text.find_last_not_of(' ') + 1);
        if (!text.empty())
            break;
        runs.pop_back();
    }
    const bool blank = std::ranges::all_of(runs, [](const Run& run) {
        return run.flags == "q" || run.text.find_first_not_of(' ') == std::string::npos;
    });
    if (blank)
        return std::nullopt;
    if (line.code) {
        std::string text = text_only(runs);
        text.erase(text.find_last_not_of(' ') + 1);
        return "code:" + text;
    }
    return serialize(runs);
}

class TuiProjector {
public:
    std::vector<TuiLine> run(const Node& root) {
        blocks(root);
        return std::move(out_);
    }

private:
    struct Frame {
        bool quote = false;
        bool ordered = false;
        unsigned next = 0;
        int item_indent = 0;
        int content_indent = 0;
    };

    void blocks(const Node& parent) {
        for (const Node& node : parent.children)
            block(node);
    }

    // Kapların önekleri dıştan içe; bekleyen im en içteki listenin öğe girintisinde.
    Runs prefix(bool first) {
        Runs runs;
        int indent = 0;
        const bool marked = first && marker_.has_value();
        for (std::size_t i = 0; i < stack_.size(); ++i) {
            const Frame& frame = stack_[i];
            if (frame.quote) {
                add(runs, "│ ", "q");
                continue;
            }
            if (marked && i + 1 == stack_.size()) {
                add(runs, std::string(static_cast<std::size_t>(frame.item_indent - indent), ' '));
                for (const Run& run : *marker_)
                    add(runs, run.text, run.flags);
                indent = frame.content_indent;
                continue;
            }
            add(runs, std::string(static_cast<std::size_t>(frame.content_indent - indent), ' '));
            indent = frame.content_indent;
        }
        if (marked)
            marker_.reset();
        return runs;
    }

    int cells(std::string_view text) const { return builder_.cells(text); }
    int cells(const Runs& runs) const { return cells(text_only(runs)); }

    void flush_marker() {
        if (marker_)
            out_.push_back({prefix(true)});
    }

    void block(const Node& node) {
        if (node.tag.empty())
            return;
        if (node.tag == "blockquote" || node.tag == "ul" || node.tag == "ol" || node.tag == "table")
            flush_marker();
        if (node.tag == "blockquote") {
            stack_.push_back({.quote = true});
            blocks(node);
            stack_.pop_back();
        } else if (node.tag == "ul" || node.tag == "ol") {
            list(node);
        } else if (node.tag == "p") {
            Runs runs;
            inlines(node, {}, runs);
            paragraph(runs);
        } else if (is_heading(node.tag)) {
            Runs runs;
            inlines(node, {.bold = true}, runs);
            trim(runs);
            Runs line = prefix(true);
            add(line, std::string(static_cast<std::size_t>(node.tag[1] - '0'), '#') + " ", "d");
            for (const Run& run : runs)
                add(line, run.text, run.flags);
            out_.push_back({line});
        } else if (node.tag == "pre") {
            code(text_of(node));
        } else if (node.tag == "hr") {
            Runs line = prefix(true);
            std::string rule;
            for (int i = cells(line); i < kTuiWidth; ++i)
                rule += "─";
            add(line, rule, "d");
            out_.push_back({line});
        } else if (node.tag == "table") {
            table(node);
        }
    }

    void list(const Node& node) {
        int base = 0;
        int depth = 1;
        for (const Frame& frame : stack_)
            if (!frame.quote) {
                base = frame.content_indent;
                ++depth;
            }
        const bool ordered = node.tag == "ol";
        stack_.push_back({.ordered = ordered,
                          .next = ordered ? static_cast<unsigned>(std::stoul(node.attr("start").value_or("1"))) : 0,
                          .item_indent = base,
                          .content_indent = base});
        static constexpr std::string_view kBullets[] = {"• ", "◦ ", "▪ "};
        for (const Node& item : node.children) {
            if (item.tag != "li")
                continue;
            Frame& frame = stack_.back();
            Runs marker;
            add(marker,
                ordered ? std::to_string(frame.next++) + ". "
                        : std::string(kBullets[static_cast<std::size_t>(depth - 1) % std::size(kBullets)]),
                "d");
            for (const Node& child : item.children)
                if (child.tag == "input")
                    add(marker, child.attr("checked") ? "[x] " : "[ ] ", "k");
            frame.content_indent = frame.item_indent + cells(marker);
            marker_ = std::move(marker);
            item_content(item);
            flush_marker();
        }
        stack_.pop_back();
    }

    void item_content(const Node& item) {
        Runs runs;
        bool has_inline = false;
        const auto close_paragraph = [&] {
            if (has_inline)
                paragraph(runs);
            runs.clear();
            has_inline = false;
        };
        for (const Node& child : item.children) {
            if (child.tag == "input")
                continue;
            if (is_block(child.tag)) {
                close_paragraph();
                block(child);
                continue;
            }
            if (is_blank_text(child) && !has_inline)
                continue;
            has_inline = true;
            inline_node(child, {}, runs);
        }
        close_paragraph();
    }

    // Zorunlu satır sonu yeni satırdır; devam satırının öneki imsiz.
    void paragraph(Runs runs) {
        trim(runs);
        if (runs.empty())
            return;
        Runs line = prefix(true);
        for (const Run& run : runs) {
            std::string_view text = run.text;
            while (true) {
                const std::size_t eol = text.find('\n');
                add(line, text.substr(0, eol), run.flags);
                if (eol == std::string_view::npos)
                    break;
                out_.push_back({line});
                line = prefix(false);
                text.remove_prefix(eol + 1);
            }
        }
        out_.push_back({line});
    }

    void code(std::string text) {
        if (text.ends_with('\n'))
            text.pop_back();
        bool first = true;
        std::string_view rest = text;
        while (true) {
            const std::size_t eol = rest.find('\n');
            std::string shown;
            for (const char c : rest.substr(0, eol))
                shown += c == '\t' ? std::string(4, ' ') : std::string(1, c);
            Runs line = prefix(first);
            add(line, "  " + tui::display_safe(shown));
            out_.push_back({line, true});
            first = false;
            if (eol == std::string_view::npos)
                break;
            rest.remove_prefix(eol + 1);
        }
    }

    void table(const Node& node) {
        struct Cell {
            Runs runs;
            std::string align;
        };
        std::vector<std::vector<Cell>> rows;
        std::size_t header_rows = 0;
        for (const Node& part : node.children)
            for (const Node& row : part.children) {
                if (row.tag != "tr")
                    continue;
                std::vector<Cell>& cells_of_row = rows.emplace_back();
                for (const Node& cell : row.children) {
                    if (cell.tag != "th" && cell.tag != "td")
                        continue;
                    Cell& out = cells_of_row.emplace_back();
                    out.align = cell.attr("align").value_or("");
                    inlines(cell, {.bold = part.tag == "thead"}, out.runs);
                    trim(out.runs);
                }
                header_rows += part.tag == "thead" ? 1 : 0;
            }
        std::size_t columns = 0;
        for (const auto& row : rows)
            columns = std::max(columns, row.size());
        std::vector<int> widths(columns, 1);
        for (const auto& row : rows)
            for (std::size_t c = 0; c < row.size(); ++c)
                widths[c] = std::max(widths[c], cells(row[c].runs));
        const Runs base = prefix(false);
        for (std::size_t r = 0; r < rows.size(); ++r) {
            Runs line = base;
            add(line, "│ ", "d");
            for (std::size_t c = 0; c < columns; ++c) {
                const Cell empty;
                const Cell& cell = c < rows[r].size() ? rows[r][c] : empty;
                const int spare = widths[c] - cells(cell.runs);
                const int before = cell.align == "right" ? spare : cell.align == "center" ? spare / 2 : 0;
                add(line, std::string(static_cast<std::size_t>(before), ' '));
                for (const Run& run : cell.runs)
                    add(line, run.text, run.flags);
                add(line, std::string(static_cast<std::size_t>(spare - before), ' '));
                add(line, c + 1 < columns ? " │ " : " │", "d");
            }
            out_.push_back({line});
            if (r + 1 == header_rows) {
                std::string rule = "├";
                for (std::size_t c = 0; c < columns; ++c) {
                    for (int i = 0; i < widths[c] + 2; ++i)
                        rule += "─";
                    rule += c + 1 < columns ? "┼" : "┤";
                }
                Runs rule_line = base;
                add(rule_line, rule, "d");
                out_.push_back({rule_line});
            }
        }
    }

    struct Style {
        bool bold = false;
        bool italic = false;
        bool strike = false;
        bool code = false;
        bool link = false;

        std::string flags() const {
            std::string out;
            out += bold ? "b" : "";
            out += italic ? "i" : "";
            out += strike ? "s" : "";
            out += link && !code ? "a" : "";
            out += code ? "c" : "";
            return out;
        }
    };

    void inlines(const Node& parent, const Style& style, Runs& runs) {
        for (const Node& child : parent.children)
            inline_node(child, style, runs);
    }

    void inline_node(const Node& node, Style style, Runs& runs) {
        if (node.tag.empty()) {
            std::string text = node.text;
            if (after_break_ && text.starts_with('\n'))
                text.erase(0, 1);
            after_break_ = false;
            add(runs, tui::display_safe(text), style.flags());
            return;
        }
        after_break_ = false;
        if (node.tag == "br") {
            add(runs, "\n", style.flags());
            after_break_ = true;
            return;
        }
        if (node.tag == "img") {  // TUI kuralı: [görsel: alt], dizge renginde, dış biçimsiz
            const std::string alt = tui::display_safe(node.attr("alt").value_or(""));
            add(runs, alt.empty() ? core::tr("tui.md_image_no_alt") : core::tr("tui.md_image", {{"alt", alt}}), "g");
            return;
        }
        if (node.tag == "strong")
            style.bold = true;
        else if (node.tag == "em")
            style.italic = true;
        else if (node.tag == "del")
            style.strike = true;
        else if (node.tag == "code")
            style.code = true;
        else if (node.tag == "a" || node.tag == "x-wikilink")  // wiki: ayraçsız metin, adres yazılmaz
            style.link = true;
        inlines(node, style, runs);
        if (node.tag == "a") {
            // TUI kuralı: bağlantı metni + soluk " (adres)"; otomatik bağlantının metni adresin kendisidir.
            const std::string href = node.attr("href").value_or("");
            const std::string shown = text_of(node);
            const std::string escaped = url_escaped(shown);
            const bool autolink = href == escaped || href == "mailto:" + escaped || href == "http://" + escaped;
            if (!href.empty() && !autolink)
                add(runs, " (" + href + ")", "d");
        }
    }

    tui::ViewBuilder builder_{kTuiWidth, tui::terminal_cell_width};
    std::vector<Frame> stack_;
    std::optional<Runs> marker_;
    bool after_break_ = false;
    std::vector<TuiLine> out_;
};

std::vector<TuiLine> tui_actual(std::string_view markdown) {
    const tui::RenderedView view = tui::render_markdown(markdown, kTuiWidth, tui::terminal_cell_width);
    std::vector<TuiLine> out;
    for (std::size_t i = 0; i < view.lines.size(); ++i) {
        const std::string& text = view.lines[i];
        TuiLine line;
        std::size_t at = 0;
        for (const tui::StyledSpan& span : view.spans[i]) {
            add(line.runs, std::string_view(text).substr(at, span.begin - at));
            add(line.runs, std::string_view(text).substr(span.begin, span.end - span.begin), tui_flags(span.look));
            at = span.end;
        }
        add(line.runs, std::string_view(text).substr(at));
        // Bağlantı adresi kaynaktaki (çözülmüş) biçimiyle yazılır; md_html'in kaçışıyla karşılaştırılsın.
        for (Run& run : line.runs)
            if (run.flags == "d" && run.text.starts_with(" (") && run.text.ends_with(')') && &run != &line.runs.front())
                run.text = " (" + url_escaped(std::string_view(run.text).substr(2, run.text.size() - 3)) + ")";
        out.push_back(std::move(line));
    }
    return out;
}

std::vector<std::string> normalized_lines(const std::vector<TuiLine>& lines) {
    std::vector<std::string> out;
    for (const TuiLine& line : lines)
        if (std::optional<std::string> shown = normalized(line))
            out.push_back(*shown);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------

struct Tally {
    int total = 0;
    int gui = 0;
    int tui = 0;
};

std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines)
        out += "    " + line + "\n";
    return out;
}

// Gerçek çıktıda hangi satırın kod olduğu bilinmez: beklenen kod satırının yerindeki satır yalnız metniyle
// karşılaştırılır.
std::vector<std::string> tui_compared(const std::vector<TuiLine>& actual, const std::vector<TuiLine>& expected) {
    std::vector<const TuiLine*> visible_expected;
    for (const TuiLine& line : expected)
        if (normalized(line))
            visible_expected.push_back(&line);
    std::vector<std::string> out;
    std::size_t index = 0;
    for (const TuiLine& line : actual) {
        if (!normalized(line))
            continue;
        TuiLine copy = line;
        copy.code = index < visible_expected.size() && visible_expected[index]->code;
        out.push_back(*normalized(copy));
        ++index;
    }
    return out;
}

}  // namespace

TEST_CASE("markdown conformance: GUI and TUI previews match md4c's HTML structure") {
    core::init_i18n("en");
    std::vector<Example> examples;
    for (const char* file : {"spec.txt", "spec-tables.txt", "spec-tasklists.txt", "spec-strikethrough.txt",
                             "spec-permissive-autolinks.txt"}) {
        std::vector<Example> loaded = load_examples(file);
        examples.insert(examples.end(), loaded.begin(), loaded.end());
    }
    REQUIRE(examples.size() > 650);  // spec 652 + GFM uzantıları

    std::map<std::string, Tally> tallies;
    std::ostringstream report;
    for (const Example& example : examples) {
        Tally& tally = tallies[example.category];
        ++tally.total;
        const std::vector<std::string> gui_expected =
            GuiProjector().run(parse_html(reference_html(example.markdown, kPreviewFlags)));
        const std::vector<std::string> gui = gui_actual(example.markdown);
        const std::vector<TuiLine> tui_expected_lines =
            TuiProjector().run(parse_html(reference_html(example.markdown, kPreviewFlags)));
        const std::vector<std::string> tui_expected = normalized_lines(tui_expected_lines);
        const std::vector<std::string> tui = tui_compared(tui_actual(example.markdown), tui_expected_lines);
        const bool gui_differs = gui != gui_expected;
        const bool tui_differs = tui != tui_expected;
        tally.gui += gui_differs ? 1 : 0;
        tally.tui += tui_differs ? 1 : 0;
        if (gui_differs || tui_differs) {
            report << "== " << example.file << " #" << example.number << " [" << example.category << "]"
                   << (gui_differs ? " GUI" : "") << (tui_differs ? " TUI" : "") << "\n"
                   << example.markdown << "--\n";
            if (gui_differs)
                report << "  GUI expected:\n" << joined(gui_expected) << "  GUI actual:\n" << joined(gui);
            if (tui_differs)
                report << "  TUI expected:\n" << joined(tui_expected) << "  TUI actual:\n" << joined(tui);
        }
    }
    if (const char* path = std::getenv("NOTEAPP_MD_CONFORMANCE_REPORT")) {
        std::ofstream(path, std::ios::binary) << report.str();
    }

    // Kalan farkların tavanı (kategori, GUI, TUI); başlangıç (NY-P öncesi) GUI 48, TUI 19 farktı.
    // entities/escapes TUI 1: spec #40 `&#9;foo` paragrafı TUI'de baştaki boşlukla başlar; HTML baştaki boşluğu
    // gösterirken yutar, GUI de (sarma satır başındaki boşluğu atar). Görünür etkisi tek hücre.
    const std::map<std::string, std::pair<int, int>> ceilings = {{"entities/escapes", {0, 1}}};
    std::ostringstream table;
    table << "category | examples | GUI mismatches | TUI mismatches\n";
    Tally sum;
    for (const auto& [category, tally] : tallies) {
        table << category << " | " << tally.total << " | " << tally.gui << " | " << tally.tui << "\n";
        sum.total += tally.total;
        sum.gui += tally.gui;
        sum.tui += tally.tui;
    }
    table << "total | " << sum.total << " | " << sum.gui << " | " << sum.tui << "\n";
    MESSAGE(table.str());
    for (const auto& [category, tally] : tallies) {
        CAPTURE(category);
        const auto found = ceilings.find(category);
        const auto [gui_ceiling, tui_ceiling] = found == ceilings.end() ? std::pair{0, 0} : found->second;
        CHECK(tally.gui <= gui_ceiling);
        CHECK(tally.tui <= tui_ceiling);
    }
}
