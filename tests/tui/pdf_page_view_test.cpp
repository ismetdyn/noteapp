#include "core/i18n.hpp"
#include "edit/editor_session.hpp"
#include "support/note_seed.hpp"
#include "support/test_support.hpp"
#include "support/text_pdf.hpp"
#include "tui/editor_view.hpp"
#include "tui/list_actions.hpp"
#include "tui/mouse.hpp"
#include "tui/pdf_view.hpp"
#include "tui/tui_state.hpp"
#include "tui/view.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <doctest/doctest.h>
#include <format>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace noteapp;
using tui::Intent;
using tui::Key;

namespace {

constexpr int kWidth = 100;
constexpr int kHeight = 40;
constexpr float kPageWidth = 200.0F;  // support/text_pdf.hpp: sayfalar 200x100 pt
constexpr float kPageHeight = 100.0F;

// Üst yarısı siyah dolu dikdörtgen, altı beyaz zemin (PDF'te y yukarı doğru artar).
std::string half_black_pdf() {
    pdftest::PdfBuilder builder;
    return test::text_pdf_detail::finish(builder, {{"<< >>", "0 0 0 rg 0 50 200 50 re f"}});
}

// Boş beyaz sayfalar, her biri kendi boyunda (punto).
std::string sized_pdf(const std::vector<std::pair<int, int>>& sizes) {
    pdftest::PdfBuilder builder;
    const int catalog = builder.reserve();
    const int tree = builder.reserve();
    const int content = test::text_pdf_detail::add_stream(builder, "", "q Q");
    std::string kids;
    for (const auto& [width, height] : sizes)
        kids += std::format("{} 0 R ", builder.add(std::format(
                                           "<< /Type /Page /Parent {} 0 R /MediaBox [0 0 {} {}] /Contents {} 0 R >>",
                                           tree, width, height, content)));
    builder.set(catalog, std::format("<< /Type /Catalog /Pages {} 0 R >>", tree));
    builder.set(tree, std::format("<< /Type /Pages /Kids [{}] /Count {} >>", kids, sizes.size()));
    return builder.build(catalog);
}

ftxui::Screen render_screen(const tui::TuiState& state, const tui::Preview& preview, int width = kWidth) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(kHeight));
    ftxui::Render(screen, tui::render(state, preview, {"sync off", ""}, {width, kHeight}));
    return screen;
}

std::optional<std::pair<int, int>> find_ascii(const ftxui::Screen& screen, const std::string& needle) {
    for (int y = 0; y < screen.dimy(); ++y)
        for (int x = 0; x + static_cast<int>(needle.size()) <= screen.dimx(); ++x) {
            bool match = true;
            for (std::size_t i = 0; i < needle.size() && match; ++i)
                match = screen.PixelAt(x + static_cast<int>(i), y).character == std::string(1, needle[i]);
            if (match)
                return std::pair{x, y};
        }
    return std::nullopt;
}

// Hücrelerin glifleri satır satır (renk kaçışı yok): etiket araması için.
std::string screen_text(const ftxui::Screen& screen) {
    std::string text;
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x)
            text += screen.PixelAt(x, y).character;
        text += '\n';
    }
    return text;
}

int pane_columns(const tui::TuiState& state) {
    return tui::editor_viewport({state.window_width(), state.window_height()}, state.layout()).columns;
}

// main'in sync_preview'u gibi: her çizimde önbellek yayımlar, model satırları alır. `done` tutana dek (en çok 20 sn).
// `done` değerle alınır: dönen başvuru önbelleğindir, geçici yükleme nesnesine bağlanmaz.
template <class Done>
const tui::Preview& pump(tui::PreviewCache& cache, tui::TuiState& state, const core::NoteStore& store, Done done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (true) {
        const tui::Preview& shown = cache.throttled(state, store, std::chrono::steady_clock::now());
        state.set_preview_lines(&shown.lines, cache.generation(), shown.partial,
                                shown.rendered ? &shown.styled : nullptr, &shown.wiki_targets);
        if (done(shown) || std::chrono::steady_clock::now() > deadline)
            return shown;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool page_drawn(const tui::Preview& shown) {
    return shown.pdf == tui::Preview::Pdf::Pages && !shown.page_images.empty() && shown.page_images[0].pixels;
}

// Gerçek `note __pdf-worker` ile PDF notu; REVIEW odaklı, metin görünümünün ilk sayfası yüklenmiş.
struct PdfFixture {
    test::TempDir dir;
    std::optional<core::NoteStore> store;
    tui::TuiState state;
    tui::PreviewCache cache;

    explicit PdfFixture(const std::string& pdf, core::PdfLimits limits = {}) {
        core::init_i18n("en");
        test::upsert_note(dir.path(), "doc.pdf", pdf);
        store.emplace(dir.path());
        cache.set_pdf(std::move(limits), NOTEAPP_NOTE_EXE, [] {});
        state.set_notes(store->list());
        state.set_window(kWidth, kHeight);
        pump(cache, state, *store, [](const tui::Preview& shown) {
            return shown.text.find("--- page 1/") != std::string::npos;
        });
        state.focus_preview();
    }
    ~PdfFixture() { cache.close_pdf(); }
    PdfFixture(const PdfFixture&) = delete;
    PdfFixture& operator=(const PdfFixture&) = delete;

    const tui::Preview& show_pages() {
        REQUIRE(state.handle(Key::Char, "R") == Intent::None);
        return pump(cache, state, *store, page_drawn);
    }
};

}  // namespace

TEST_CASE("page_block_row box-samples the page to the pane width, two pixel rows per cell") {
    // 4x4: üst iki satır siyah, alt iki satır beyaz; 2 sütuna kutu örneklenince her hedef piksel 2x2 kaynak.
    pdfview::RenderedPage page{.width = 4, .height = 4, .rgba = {}};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const std::uint8_t value = y < 2 ? 0 : 255;
            page.rgba.insert(page.rgba.end(), {value, value, value, 255});
        }
    // 2 sütun: hedef 2x2 piksel, tek hücre satırı. Üst piksel siyah, alt beyaz.
    const auto row = tui::page_block_row(page, 0, 2);
    REQUIRE(row.size() == 2);
    CHECK(row[0].top == tui::Rgb{0, 0, 0});
    CHECK(row[0].bottom == tui::Rgb{255, 255, 255});
    // Sayfanın altındaki hücre satırı boştur.
    CHECK_FALSE(tui::page_block_row(page, 1, 2)[0].top.has_value());
    // 1 sütun: hedef tek piksel genişlik, 4x4 kutunun ortalaması gri; ikinci piksel satırı sayfanın dışında.
    const auto narrow = tui::page_block_row(page, 0, 1);
    CHECK(narrow[0].top == tui::Rgb{128, 128, 128});
    CHECK_FALSE(narrow[0].bottom.has_value());
    CHECK(tui::page_block_rows(kPageWidth, kPageHeight, 80) == 20);
    CHECK(tui::page_block_rows(kPageWidth, kPageHeight, 81) == 21);
}

TEST_CASE("R switches a PDF to page view and back; cells are ▀ with the upper pixel in front, the lower behind") {
    PdfFixture f(half_black_pdf());
    CHECK(f.cache.shown().pdf == tui::Preview::Pdf::Text);
    CHECK(screen_text(render_screen(f.state, f.cache.shown())).find("REVIEW · text") != std::string::npos);

    const tui::Preview& shown = f.show_pages();
    REQUIRE(page_drawn(shown));
    const int columns = pane_columns(f.state);
    const int rows = tui::page_block_rows(kPageWidth, kPageHeight, columns);
    // Ayırıcı, sayfa yüksekliği kadar görüntü satırı, boş satır.
    REQUIRE(shown.page_rows.size() == shown.lines.size());
    REQUIRE(shown.lines.size() >= static_cast<std::size_t>(rows) + 2);
    CHECK(shown.lines[0] == "--- page 1/1 ---");
    CHECK(shown.page_rows[0].page < 0);
    CHECK(shown.page_rows[1].page == 0);
    CHECK(shown.page_rows[1].row == 0);
    CHECK(shown.page_rows[static_cast<std::size_t>(rows)].row == rows - 1);
    CHECK(shown.page_rows[static_cast<std::size_t>(rows) + 1].page < 0);

    const ftxui::Screen screen = render_screen(f.state, shown);
    CHECK(screen_text(screen).find("REVIEW · page") != std::string::npos);
    const auto separator = find_ascii(screen, "--- page 1/1 ---");
    REQUIRE(separator.has_value());
    const auto [x, y] = *separator;
    const ftxui::Cell& top = screen.PixelAt(x + 1, y + 1);  // sayfanın üst yarısı: siyah üstünde siyah
    CHECK(top.character == "▀");
    CHECK(top.foreground_color == ftxui::Color::RGB(0, 0, 0));
    CHECK(top.background_color == ftxui::Color::RGB(0, 0, 0));
    const ftxui::Cell& bottom = screen.PixelAt(x + 1, y + rows);  // alt yarı: beyaz üstünde beyaz
    CHECK(bottom.character == "▀");
    CHECK(bottom.foreground_color == ftxui::Color::RGB(255, 255, 255));
    CHECK(bottom.background_color == ftxui::Color::RGB(255, 255, 255));

    REQUIRE(f.state.handle(Key::Char, "R") == Intent::None);
    const tui::Preview& text = pump(f.cache, f.state, *f.store, [](const tui::Preview& p) {
        return p.pdf == tui::Preview::Pdf::Text;
    });
    CHECK(text.pdf == tui::Preview::Pdf::Text);
    CHECK(screen_text(render_screen(f.state, text)).find("REVIEW · text") != std::string::npos);
}

TEST_CASE("PDF page view re-renders the page at the new pane width") {
    PdfFixture f(half_black_pdf());
    const tui::Preview& first = f.show_pages();
    REQUIRE(page_drawn(first));
    const int columns = pane_columns(f.state);
    // Ölçek sayfayı panel genişliğine sığdırır: çizilmiş sayfa sütun sayısı kadar piksel genişliktedir.
    CHECK(std::abs(first.page_images[0].pixels->width - columns) <= 1);

    f.state.set_window(kWidth + 40, kHeight);
    const int wider = pane_columns(f.state);
    REQUIRE(wider > columns + 2);
    const tui::Preview& resized = pump(f.cache, f.state, *f.store, [wider](const tui::Preview& shown) {
        return page_drawn(shown) && std::abs(shown.page_images[0].pixels->width - wider) <= 1;
    });
    REQUIRE(page_drawn(resized));
    CHECK(std::abs(resized.page_images[0].pixels->width - wider) <= 1);
    CHECK(resized.page_rows.size() ==
          static_cast<std::size_t>(tui::page_block_rows(kPageWidth, kPageHeight, wider)) + 2);
}

// Satır yuvası ilk sayfanın boyundadır (kaydırma kaymaz); ölçek sayfanın kendi genişliğinden: küçük ilk sayfa
// sonraki büyük sayfayı panelin katlarca genişliğinde istetmez.
TEST_CASE("PDF page view renders every page to the pane width from that page's own size") {
    PdfFixture f(sized_pdf({{50, 25}, {200, 100}}));
    pump(f.cache, f.state, *f.store,
         [](const tui::Preview& shown) { return shown.text.find("--- page 2/2 ---") != std::string::npos; });
    REQUIRE(f.state.handle(Key::Char, "R") == Intent::None);
    const auto both_drawn = [](const tui::Preview& shown) {
        return shown.pdf == tui::Preview::Pdf::Pages && shown.page_images.size() == 2 &&
               shown.page_images[0].pixels && shown.page_images[1].pixels;
    };
    const tui::Preview& shown = pump(f.cache, f.state, *f.store, both_drawn);
    REQUIRE(both_drawn(shown));
    const int columns = pane_columns(f.state);
    CHECK(std::abs(shown.page_images[0].pixels->width - columns) <= 1);
    CHECK(std::abs(shown.page_images[1].pixels->width - columns) <= 1);
    CHECK(shown.page_rows.size() == 2 * (static_cast<std::size_t>(tui::page_block_rows(50.0F, 25.0F, columns)) + 2));
}

TEST_CASE("sixel page view: cell-pixel wide render, untouched cells, the image place recorded") {
    PdfFixture f(half_black_pdf());
    constexpr tui::CellPixels kCell{.width = 8, .height = 16};
    f.cache.set_sixel_cell(kCell);
    const tui::Preview& shown = f.show_pages();
    REQUIRE(page_drawn(shown));
    const int columns = pane_columns(f.state);
    REQUIRE(shown.sixel == kCell);
    CHECK(std::abs(shown.page_images[0].pixels->width - (columns * kCell.width)) <= 1);
    const int rows = tui::page_block_rows(kPageWidth, kPageHeight, columns, kCell);
    // 200x100 pt: piksel yüksekliği genişliğin yarısı, satıra yukarı yuvarlanır.
    CHECK(rows == ((columns * kCell.width / 2) + kCell.height - 1) / kCell.height);
    CHECK(shown.page_rows.size() == static_cast<std::size_t>(rows) + 2);

    shown.sixels.clear();
    const ftxui::Screen screen = render_screen(f.state, shown);
    const auto separator = find_ascii(screen, "--- page 1/1 ---");
    REQUIRE(separator.has_value());
    const auto [x, y] = *separator;
    // Ayırıcının altındaki bütün görüntü satırları tek görüntü; hücreleri boş (FrameDiff oraya basar).
    REQUIRE(shown.sixels.size() == 1);
    const tui::SixelImage& image = shown.sixels[0];
    CHECK(image.pixels == shown.page_images[0].pixels);
    CHECK(image.cell == kCell);
    CHECK(image.x == x);
    CHECK(image.y == y + 1);
    CHECK(image.first_row == 0);
    CHECK(image.rows == rows);
    CHECK(image.columns == columns);
    for (int row = image.y; row < image.y + image.rows; ++row)
        for (int column = image.x; column < image.x + image.columns; ++column)
            CHECK(screen.PixelAt(column, row).character.empty());

    // Blok kipine dönünce sayfa yeniden düzenlenir, görüntü kaydı olmaz.
    f.cache.set_sixel_cell(std::nullopt);
    const tui::Preview& blocks = pump(f.cache, f.state, *f.store, [](const tui::Preview& p) {
        return page_drawn(p) && !p.sixel;
    });
    blocks.sixels.clear();
    render_screen(f.state, blocks);
    CHECK(blocks.sixels.empty());
    const int block_rows = tui::page_block_rows(kPageWidth, kPageHeight, columns);
    CHECK(blocks.page_rows.size() == static_cast<std::size_t>(block_rows) + 2);
}

TEST_CASE("PDF page view refuses copy, mouse selection and find with a notice pointing to R") {
    PdfFixture f(half_black_pdf());
    std::mutex mutex;
    std::string internal;
    tui::SystemClipboard system{[](std::string_view) {}};
    tui::StatusLine status;
    tui::ListActions actions{
        tui::ListHost{f.store, mutex, internal, system, f.state, f.cache, status, [] {}, [] {}}};
    tui::EditorSession editor{
        tui::EditorHost{*f.store, mutex, internal, tui::terminal_cell_width, 1024 * 1024,
                        core::kDefaultSlugMaxLength, [](std::string_view) { return std::string("md"); },
                        [](const core::NotePath&) {}, [] {}, [](std::optional<std::size_t>) {},
                        [](const core::Error&) {}, [] {}}};
    const std::string notice = core::tr("tui.pdf_page_no_text");
    REQUIRE(notice.find("R") != std::string::npos);

    // Metin görünümünde `/` arar.
    CHECK(f.state.handle(Key::Char, "/") == Intent::None);
    CHECK(f.state.focus() == tui::Focus::PreviewFind);
    f.state.handle(Key::Escape);
    f.state.focus_preview();

    REQUIRE(page_drawn(f.show_pages()));
    std::optional<tui::ConfirmState> confirm;
    const Intent copy = f.state.handle(Key::Char, "c");
    REQUIRE(copy == Intent::CopyNote);
    actions.run(copy, confirm);
    CHECK(status.message == notice);
    CHECK(internal.empty());

    status.message.clear();
    const Intent find = f.state.handle(Key::Char, "/");
    CHECK(find == Intent::PdfPageNoText);
    CHECK(f.state.focus() == tui::Focus::Preview);
    actions.run(find, confirm);
    CHECK(status.message == notice);

    status.message.clear();
    const int pane_x = f.state.layout().list_width(kWidth) + 5;
    const tui::MouseInput press{pane_x, 5, tui::MouseButton::Left, tui::MouseMotion::Press};
    CHECK(tui::route_mouse(press, f.state, editor) == tui::MouseResult::PdfPageNoText);
    CHECK_FALSE(f.state.preview_selection_dragging());
    actions.on_mouse(press, editor);
    CHECK(status.message == notice);
}

// Görünen sayfalar ve bir sonraki (3) önbellekten (cache_pages = 1) çok: sayfalar birbirini düşürüp yeniden
// istenmemeli, işçi durulmalı. Görünen sayfalar önbellek sınırına sayılmaz.
TEST_CASE("PDF page view settles when the render cache is smaller than the visible pages") {
    core::PdfLimits limits;
    limits.cache_pages = core::Limit{1, "1"};
    PdfFixture f(test::text_pdf({"One", "Two", "Three"}), limits);
    REQUIRE(f.state.handle(Key::Char, "R") == Intent::None);
    const auto all_drawn = [](const tui::Preview& shown) {
        return shown.pdf == tui::Preview::Pdf::Pages && shown.page_images.size() == 3 &&
               std::ranges::all_of(shown.page_images, [](const auto& image) { return image.pixels != nullptr; });
    };
    REQUIRE(all_drawn(pump(f.cache, f.state, *f.store, all_drawn)));
    // Durulma: art arda karelerde yeni istek yok, sonuç sürümü artmıyor.
    REQUIRE(f.cache.pdf() != nullptr);
    const std::uint64_t settled = f.cache.pdf()->version();
    for (int frame = 0; frame < 50; ++frame) {
        CHECK(all_drawn(f.cache.throttled(f.state, *f.store, std::chrono::steady_clock::now())));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(f.cache.pdf()->pending() == 0);
    CHECK(f.cache.pdf()->version() == settled);
}

// Kaydırma görünen aralığı değiştirir, bekleyen istekler (metin okumaları dahil) düşer: metin görünümü yine de
// bütün sayfaları okur.
TEST_CASE("PDF text still loads fully after page view scrolling dropped pending reads") {
    std::vector<std::string> texts;
    for (int page = 1; page <= 8; ++page)
        texts.push_back("Body" + std::to_string(page));
    PdfFixture f(test::text_pdf(texts));
    REQUIRE(page_drawn(f.show_pages()));
    for (int step = 0; step < 8; ++step) {
        f.state.handle(Key::PageDown);
        f.cache.throttled(f.state, *f.store, std::chrono::steady_clock::now());
    }
    f.state.handle(Key::Char, "G");
    REQUIRE(f.state.handle(Key::Char, "R") == Intent::None);
    const tui::Preview& text = pump(f.cache, f.state, *f.store, [](const tui::Preview& shown) {
        return shown.pdf == tui::Preview::Pdf::Text && shown.text.find("Body8") != std::string::npos &&
               shown.text.find("Loading") == std::string::npos;
    });
    CAPTURE(text.text);
    CHECK(text.text.find("--- page 8/8 ---") != std::string::npos);
    CHECK(text.text.find("Body8") != std::string::npos);
    CHECK(text.text.find("Loading") == std::string::npos);
}
