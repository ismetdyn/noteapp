// Yamalı FTXUI ayrıştırıcısının bracketed paste sınırları (cmake/patches/ftxui-7.0.3-frame-writer.patch).
// Uçtan uca karşılıkları tui_pty'dedir; burada bayt bayt ve zaman aşımı adım adım sürülür.
#include <chrono>
#include <cstddef>
#include <doctest/doctest.h>
#include <ftxui/component/event.hpp>
#include <ftxui/component/terminal_input_parser.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view kStart = "\x1B[200~";
constexpr std::string_view kEnd = "\x1B[201~";
constexpr std::string_view kTooLarge = "\x1B[noteapp-paste-too-large";
constexpr std::size_t kLimit = 16;
// Zaman aşımı sınanmayacaksa hiç dolmayacak kadar uzun; sınanacaksa kısa ve aşılır.
constexpr auto kNever = std::chrono::hours(1);
constexpr auto kShort = std::chrono::milliseconds(1);
constexpr auto kPastShort = std::chrono::milliseconds(20);

struct Parser {
    std::vector<std::string> events;
    ftxui::TerminalInputParser parser{[this](ftxui::Event event) { events.push_back(event.input()); }};

    explicit Parser(std::chrono::milliseconds timeout = kNever) {
        parser.SetPasteTimeout(timeout);
        parser.SetPasteLimit(kLimit);
    }
    void add(std::string_view bytes) {
        for (const char c : bytes)
            parser.Add(c);
    }
    // Uygulama döngüsü okumalar arasında Timeout çağırır; ilk çağrı sessizliği başlatır, süre dolduktan sonraki
    // çağrı yapıştırmayı bitirir.
    void silence() {
        parser.Timeout(0);
        std::this_thread::sleep_for(kPastShort);
        parser.Timeout(0);
    }
};

std::string pasted(std::string_view text) {
    return std::string(kStart) + std::string(text);
}

}  // namespace

TEST_CASE("paste parser: exactly the limit is pasted, one byte more is too large") {
    Parser at_limit;
    at_limit.add(std::string(kStart) + std::string(kLimit, 'x') + std::string(kEnd));
    CHECK(at_limit.events == std::vector{pasted(std::string(kLimit, 'x'))});

    Parser over;
    over.add(std::string(kStart) + std::string(kLimit + 1, 'x') + std::string(kEnd));
    CHECK(over.events == std::vector{std::string(kTooLarge)});
    over.add("a");  // yapıştırma bitti: sonraki bayt yine tuştur
    REQUIRE(over.events.size() == 2);
    CHECK(over.events[1] == "a");
}

// Sınır, metnin ve bitiş işaretinin toplamına göre denetlenir; işaretin hangi baytı sınırı geçerse geçsin karar
// yalnız metnin uzunluğuna bağlıdır. Okumalar arasındaki Timeout (süre dolmadan) sonucu değiştirmez.
TEST_CASE("paste parser: an end mark straddling the limit keeps the verdict of the text length") {
    for (std::size_t length = kLimit - kEnd.size(); length <= kLimit + kEnd.size(); ++length) {
        for (std::size_t split = 0; split <= kEnd.size(); ++split) {
            CAPTURE(length);
            CAPTURE(split);
            Parser p;
            const std::string text(length, 'x');
            p.add(std::string(kStart) + text + std::string(kEnd.substr(0, split)));
            p.parser.Timeout(0);  // okuma sınırı
            p.add(kEnd.substr(split));
            CHECK(p.events == std::vector{length <= kLimit ? pasted(text) : std::string(kTooLarge)});
        }
    }
}

// Bitiş işareti gelmeden zaman aşımı: ayrıştırıcı en çok işaretin uzunluğu kadar fazlasını tutar (sınır işareti
// de sayar). Metin sınırın biraz üstündeyse yapıştırma olarak çıkar; main'deki terminal_paste onu reddeder.
TEST_CASE("paste parser: a paste that times out just over the limit leaves the size verdict to the app") {
    Parser p(kShort);
    p.add(std::string(kStart) + std::string(kLimit + 1, 'x'));
    p.silence();
    CHECK(p.events == std::vector{pasted(std::string(kLimit + 1, 'x'))});
}

// Bitiş işareti sessizlikten önce yarım kaldıysa yarısı metindir: zaman aşımı toplananı olduğu gibi verir, geç
// gelen kalan da (ikinci yapıştırma) metindir. Sonuç bozulur: notta ESC baytı ve `[201~` kalır. Gerçek terminalde
// işaretin ortasında paste_timeout (1 sn) kadar susma beklenmez; bozulma tuş çalıştırmaz, metni değiştirir.
TEST_CASE("paste parser: an end mark split by a timeout lands as text in two pastes, no key runs") {
    Parser p(kShort);
    p.add(std::string(kStart) + "ab" + std::string(kEnd.substr(0, 4)));
    p.silence();
    CHECK(p.events == std::vector{pasted("ab\x1B[20")});
    p.add(kEnd.substr(4));
    p.silence();
    CHECK(p.events == std::vector{pasted("ab\x1B[20"), pasted("1~")});
    p.add("q");  // geç gelen kalan teslim edildi: tuşlar yeniden tuştur
    REQUIRE(p.events.size() == 3);
    CHECK(p.events[2] == "q");
}

// Kayıp bitiş işaretinin bedeli (SPEC-tui Yapıştırma): paste_timeout dolmadan basılan tuşlar yapıştırmanın
// metnidir, komut çalıştırmaz.
TEST_CASE("paste parser: keys typed before the timeout of a paste with a lost end mark become its text") {
    Parser p(kShort);
    p.add(std::string(kStart) + "lost");
    p.parser.Timeout(0);
    p.add("q\x11");  // tuş olsaydı: harf ve Ctrl+Q
    p.silence();
    CHECK(p.events == std::vector{pasted("lostq\x11")});
}
