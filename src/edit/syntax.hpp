#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace noteapp::tui {

enum class SyntaxLanguage : std::uint8_t {
    Plain,  // bilinmeyen tip, txt, csv
    Java,
    Cpp,
    C,
    Python,
    Shell,
    JavaScript,
    TypeScript,
    Go,
    Rust,
    Sql,
    Json,
    Css,
    Html,
    Xml,
    Yaml,
    Toml,
    Ini,
    Env,
    Properties,
    Dockerfile,
    Makefile,
    Markdown,
};

enum class TokenKind : std::uint8_t {
    Plain,
    Comment,
    String,
    Number,
    Keyword,
    Type,
    Constant,
    Key,
    Value,
    Tag,
    Heading,
    Code,
    Emphasis,
};

// Türün renk ailesi; tek tablo (SPEC-tui "Sözdizimi vurgusu"). GUI token_color ve TUI token_styled yalnız
// aileyi renge çevirir: iki arayüzün eşlemesi ayrışamaz.
enum class SyntaxFamily : std::uint8_t {
    Plain,
    Muted,    // yorum
    Green,    // dizge, değer
    Orange,   // sayı, sabit (TUI: açık kırmızı, sarı aramanındır)
    Cyan,     // tip, kod
    Magenta,  // anahtar sözcük, anahtar, etiket
    Blue,     // başlık (kalın)
    Bold,     // vurgu
};

SyntaxFamily syntax_family(TokenKind kind);

// Satır içi bayt aralığı [begin, end); Plain aralık üretilmez, aralıklar sıralı ve örtüşmesizdir. Sınır UTF-8
// dizisinin ortasına düşebilir: boyama grapheme'in ilk baytına bakar, bayta dokunmaz.
struct SyntaxSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
    TokenKind kind = TokenKind::Plain;
};

// Satır sonundaki çok satırlı yorum/dizge durumu; 0 başlangıçtır.
using SyntaxState = std::uint8_t;
inline constexpr SyntaxState kInitialSyntaxState = 0;

// `limits.tui.highlight_bytes` ve `highlight_line_bytes` varsayılanları (defaults.toml ile aynı, test
// denetler). Bundan büyük not düz metin çizilir: durum taraması tuş bütçesine (50 ms) sığmayabilir. Ölçüm:
// SPEC-tui.
inline constexpr std::size_t kDefaultHighlightBytes = std::size_t{2} * 1024 * 1024;
// Bundan uzun satır boyanmaz ve durumu olduğu gibi geçirir: her karede satırı baştan taramak gereksiz, tek
// satırlık dev dosyanın gecikmesini de katlardı.
inline constexpr std::size_t kDefaultHighlightLineBytes = std::size_t{64} * 1024;

// Renklendirmenin sınırları; sınırsız SIZE_MAX.
struct SyntaxLimits {
    std::size_t note_bytes = kDefaultHighlightBytes;
    std::size_t line_bytes = kDefaultHighlightLineBytes;

    bool operator==(const SyntaxLimits&) const = default;
};

// Noktasız uzantıdan dil; büyük/küçük harf ayırmaz. Bilinmeyen uzantı Plain.
SyntaxLanguage syntax_language_for(std::string_view extension);

// Bir satırı işler, çıkış durumunu döner. `spans` nullptr ise yalnız durum ilerletilir (görünmeyen satırlar).
// Süre satır uzunluğuyla doğrusaldır; bayt içeriği ne olursa olsun çökmez. `max_line_bytes`tan uzun satır
// boyanmaz, durumu olduğu gibi geçirir.
SyntaxState highlight_line(SyntaxLanguage language, std::string_view line, SyntaxState entry,
                           std::vector<SyntaxSpan>* spans,
                           std::size_t max_line_bytes = kDefaultHighlightLineBytes);

// Satır giriş durumları: yalnız istenen satıra kadar hesaplanır, düzenleme o satırdan sonrasını düşürür.
class SyntaxStateCache {
public:
    void clear() { exit_states_.clear(); }
    void invalidate_from(std::size_t line) {
        if (line < exit_states_.size())
            exit_states_.resize(line);
    }
    std::size_t known_lines() const { return exit_states_.size(); }

    // `line_at(i)` i. satırın metnini döner; yalnız bilinmeyen satırlar için çağrılır.
    // `max_line_bytes` önbellek boyunca aynı kalmalıdır (oturumluk config).
    template <typename LineAt>
    SyntaxState entry_state(SyntaxLanguage language, std::size_t line, const LineAt& line_at,
                            std::size_t max_line_bytes = kDefaultHighlightLineBytes) {
        SyntaxState state = exit_states_.empty() ? kInitialSyntaxState : exit_states_.back();
        for (std::size_t i = exit_states_.size(); i < line; ++i) {
            state = highlight_line(language, line_at(i), state, nullptr, max_line_bytes);
            exit_states_.push_back(state);
        }
        return line == 0 ? kInitialSyntaxState : exit_states_[line - 1];
    }

private:
    std::vector<SyntaxState> exit_states_;  // i. satırın çıkış durumu
};

}  // namespace noteapp::tui
