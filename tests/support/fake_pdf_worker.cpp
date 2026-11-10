// Testlerde bozuk davranan PDF işçisi (`note __pdf-worker` yerine): istemcinin her işçi hatasını temiz bir hataya
// çevirdiğini, öldürüp topladığını ve loga kaçış dizisi sızdırmadığını sınar. İşçi boş ortamla başlar (PDF-SBX): kip,
// ikilinin (argv[0]) yanındaki `mode` dosyasındadır (support/fake_pdf_worker.hpp yazar); dosya yoksa varsayılan kip:
//   exit-mid      Open'dan sonra yarım başlık yazıp 0 ile çıkar
//   crash         Open'dan sonra abort (POSIX: SIGABRT)
//   oversize      Opened başlığında uzunluk 2^40
//   wrong-kind    Open'a TextResult cevabı
//   sleep         Open'dan sonra hiç cevap vermez
//   escape-detail Open'a Failed(Invalid) ve kaçış dizili ayrıntı
//   huge-render   Opened(1); Render'a w = h = 2^31-1 olan Rendered başlığı, sonra cevap vermez
//   bad-rgba      Opened(1); Render'a uzunluğu 8 + w·h·4'ten bir fazla Rendered başlığı
//   die-after-<n> Opened(n); sonraki ilk istekte 1 ile çıkar (yeniden başlatmada sayfa sayısı değişimi için)
//   too-many-pages  Opened(5) (istemcinin pages sınırını aşar)
//   text-over-cap   Opened(1); Text'e 64 baytlık TextResult (istemcinin text_bytes sınırını aşar)
//   render-over-cap Opened(1); Render'a eksiksiz 20x20 Rendered (istemcinin render_pixels sınırını aşar)
//   wide-render     Opened(1); Render'a w = 2^31, h = 0 olan geçerli uzunlukta Rendered (int'e sığmaz)
//   dirty-text      Opened(1); Text'e ham ESC, C1 ve geçersiz UTF-8 içeren TextResult
//   Structured isteğine (yapılı sayfa, 200x100 pt; varsayılan kipte "fake" tek parça):
//   structured-huge-count  blok sayısı 2^32-1, yük yalnız 12 bayt
//   structured-nan         karakter kenarı NaN
//   structured-off-page    karakter kenarı sayfanın 300 pt sağında
//   structured-bad-utf8    parça metni geçersiz UTF-8
//   structured-dirty       parça metninde ESC ve C1 (geçerli UTF-8)
//   structured-over-cap    64 karakterlik parça (istemcinin text_bytes sınırını aşar)
//   structured-over-frame  uzunluğu structured_length_cap(Open'daki text_bytes) + 1 olan başlık, sonra cevap yok
// Cevap vermeyen kipler stdin EOF'una dek bekler: ebeveyn ölünce ya da stdin'i kapatınca çıkar, öksüz kalmaz.
#include "pdfview/protocol.hpp"

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace proto = noteapp::pdfview::protocol;

namespace {

bool read_exact(std::string& out, std::size_t size) {
    out.resize(size);
    std::size_t got = 0;
    while (got < size) {
        const std::size_t n = std::fread(out.data() + got, 1, size - got, stdin);
        if (n == 0)
            return false;
        got += n;
    }
    return true;
}

// Bir istek çerçevesi; EOF ya da bozuksa false.
bool read_request(proto::Header& header, std::string& payload) {
    std::string bytes;
    if (!read_exact(bytes, proto::kHeaderBytes))
        return false;
    const auto decoded = proto::decode_header(bytes);
    if (!decoded)
        return false;
    header = *decoded;
    return read_exact(payload, static_cast<std::size_t>(header.length));
}

void send(std::string_view bytes) {
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
}

// Cevap vermeden stdin EOF'una dek okur (boru ebeveyn ölünce de kapanır), sonra çıkar.
[[noreturn]] void wait_for_eof() {
    std::array<char, 4096> sink{};
    while (std::fread(sink.data(), 1, sink.size(), stdin) != 0) {
    }
    std::exit(0);
}

std::string rendered_head(std::uint32_t width, std::uint32_t height) {
    std::string head = proto::encode_header(proto::Kind::Rendered, *proto::rendered_length(width, height));
    proto::put_u32(head, width);
    proto::put_u32(head, height);
    return head;
}

constexpr float kPageWidth = 200.0F;
constexpr float kPageHeight = 100.0F;

// Tek blok, tek satır, tek parça; her kod noktası 10 pt genişliğinde.
noteapp::pdfview::PageText one_span(std::string text, std::size_t chars) {
    noteapp::pdfview::TextSpan span{.text = std::move(text), .style = 0, .size = 10.0F, .edges = {}};
    for (std::size_t i = 0; i < chars; ++i) {
        span.edges.push_back(static_cast<float>(i));
        span.edges.push_back(static_cast<float>(i) + 1.0F);
    }
    noteapp::pdfview::TextLine line{.top = 10.0F, .bottom = 20.0F, .joined = false, .spans = {std::move(span)}};
    return {.width = kPageWidth, .height = kPageHeight, .blocks = {{.lines = {std::move(line)}}}};
}

std::string structured_reply(std::string_view mode) {
    if (mode == "structured-huge-count") {
        std::string payload;
        proto::put_u32(payload, std::bit_cast<std::uint32_t>(kPageWidth));
        proto::put_u32(payload, std::bit_cast<std::uint32_t>(kPageHeight));
        proto::put_u32(payload, 0xFFFFFFFFU);
        return proto::encode_header(proto::Kind::StructuredResult, payload.size()) + payload;
    }
    if (mode == "structured-nan") {
        auto page = one_span("ab", 2);
        page.blocks[0].lines[0].spans[0].edges[3] = std::numeric_limits<float>::quiet_NaN();
        return proto::encode_structured_result(page);
    }
    if (mode == "structured-off-page") {
        auto page = one_span("ab", 2);
        page.blocks[0].lines[0].spans[0].edges[3] = kPageWidth + 300.0F;
        return proto::encode_structured_result(page);
    }
    if (mode == "structured-bad-utf8")
        return proto::encode_structured_result(one_span("a\xFF", 2));
    if (mode == "structured-dirty")
        return proto::encode_structured_result(one_span("a\x1B" "b\xC2\x9B" "c", 5));
    if (mode == "structured-over-cap")
        return proto::encode_structured_result(one_span(std::string(64, 'x'), 64));
    return proto::encode_structured_result(one_span("fake", 4));
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::string mode;
    if (argc > 0) {
        std::ifstream file(std::filesystem::path{argv[0]}.parent_path() / "mode", std::ios::binary);
        std::getline(file, mode);
    }
    proto::Header header;
    std::string payload;
    if (!read_request(header, payload) || header.kind != proto::Kind::Open)
        return 3;
    const std::optional<proto::OpenFields> open = proto::decode_open(payload);
    if (!open)
        return 3;
    if (mode == "exit-mid") {
        send(proto::encode_opened(1).substr(0, 7));
        return 0;
    }
    if (mode == "crash")
        std::abort();
    if (mode == "oversize") {
        send(proto::encode_header(proto::Kind::Opened, std::uint64_t{1} << 40U));
        wait_for_eof();
    }
    if (mode == "wrong-kind") {
        send(proto::encode_text_result("x"));
        wait_for_eof();
    }
    if (mode == "sleep")
        wait_for_eof();
    if (mode == "escape-detail") {
        send(proto::encode_failed({.reason = proto::FailReason::Invalid,
                                   .a = 0,
                                   .b = 0,
                                   .detail = "evil\x1B]0;pwned\x07\x1B[2J\r\nforged=1 \xE2\x82"}));
        wait_for_eof();
    }
    std::uint32_t count = 1;
    if (mode == "too-many-pages")
        count = 5;
    if (mode.starts_with("die-after-"))
        count = static_cast<std::uint32_t>(std::stoul(std::string(mode.substr(10))));
    send(proto::encode_opened(count));
    while (read_request(header, payload)) {
        if (mode.starts_with("die-after-"))
            return 1;
        if (mode == "bad-rgba" && header.kind == proto::Kind::Render) {
            std::string head = proto::encode_header(proto::Kind::Rendered, *proto::rendered_length(2, 2) + 1);
            proto::put_u32(head, 2);
            proto::put_u32(head, 2);
            send(head + std::string(17, '\0'));
            wait_for_eof();
        }
        if (mode == "huge-render" && header.kind == proto::Kind::Render) {
            constexpr std::uint32_t kSide = 0x7FFFFFFFU;
            send(rendered_head(kSide, kSide));
            wait_for_eof();
        }
        if (mode == "render-over-cap" && header.kind == proto::Kind::Render) {
            constexpr std::uint32_t kSide = 20;
            send(rendered_head(kSide, kSide) + std::string(std::size_t{kSide} * kSide * 4, '\x7F'));
            continue;
        }
        if (mode == "wide-render" && header.kind == proto::Kind::Render) {
            send(rendered_head(0x80000000U, 0));
            continue;
        }
        if (mode == "text-over-cap" && header.kind == proto::Kind::Text) {
            send(proto::encode_text_result(std::string(64, 'x')));
            continue;
        }
        if (mode == "dirty-text" && header.kind == proto::Kind::Text) {
            send(proto::encode_text_result("a\x1B[2Jb\xC2\x9B" "c\xFF" "d"));
            continue;
        }
        if (mode == "structured-over-frame" && header.kind == proto::Kind::Structured) {
            send(proto::encode_header(proto::Kind::StructuredResult,
                                      proto::structured_length_cap(open->text_bytes) + 1));
            wait_for_eof();
        }
        if (header.kind == proto::Kind::Structured) {
            send(structured_reply(mode));
            continue;
        }
        const proto::Failure invalid{.reason = proto::FailReason::Invalid, .a = 0, .b = 0, .detail = {}};
        send(header.kind == proto::Kind::Text ? proto::encode_text_result("fake") : proto::encode_failed(invalid));
    }
    return 0;
}
