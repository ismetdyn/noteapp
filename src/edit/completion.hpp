#pragma once

#include "core/limits.hpp"
#include "core/note_store.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace noteapp::tui {

// Tamamlamanın sınırları: `limits.tui.completion_*` ve `tuning.completion_min_prefix_bytes`
// (completion_limits_of; sınırsız SIZE_MAX). Üye varsayılanları defaults.toml ile aynıdır, bir test denetler.
// Tuş başına süre pencereyle sınırlıdır, not boyutundan bağımsızdır.
struct CompletionLimits {
    std::size_t min_prefix_bytes = 2;
    std::size_t window_bytes = std::size_t{256} * 1024;  // imleç çevresinde iki yana taranan
    std::size_t word_bytes = 64;
    std::size_t table_words = 200'000;
    std::size_t candidates = 5'000;
    std::size_t note_words = 2'000;
    std::uintmax_t note_bytes = std::uintmax_t{2} * 1024 * 1024;    // tabloya okunan en büyük not
    std::uintmax_t table_bytes = std::uintmax_t{64} * 1024 * 1024;  // tabloya okunan toplam

    bool operator==(const CompletionLimits&) const = default;
};
CompletionLimits completion_limits_of(const core::TuiLimits& limits, std::size_t min_prefix_bytes);

// Diğer notların sözcükleri: sıralı, yalnız bellekte; indeks dosyası yoktur. env/properties/ini/toml/yaml
// notlarından yalnız anahtarlar girer: değerler sır olabilir, başka nota sızmaz.
struct WordTable {
    std::vector<std::string> words;     // bayt sırasıyla, tekil
    std::vector<std::uint32_t> counts;  // `words` ile aynı sırada geçiş sayısı
};

struct NoteText {
    std::string type;
    std::string text;
};
WordTable build_word_table(const std::vector<NoteText>& notes, const CompletionLimits& limits = {});

// İmleçten önceki sözcük parçası; imleç sözcüğün sonunda değilse (arkasında sözcük harfi varsa) boş.
// `word_bytes`tan uzun sözcükte de boş.
std::string_view word_prefix_at(std::string_view text, std::size_t cursor,
                                std::size_t word_bytes = CompletionLimits{}.word_bytes);

// Gölge öneri: önce bu nottan (imlece en yakın), sonra `table`dan; önerinin imleçten sonra eklenecek kısmı.
// En az `min_prefix_bytes` baytlık önek ister. Süre not boyutundan bağımsızdır: not içinde imleç çevresinde
// sınırlı pencere.
std::optional<std::string> suggest_completion(std::string_view text, std::size_t cursor,
                                              std::string_view type, const WordTable* table,
                                              const CompletionLimits& limits = {});

// Açılır liste adayları: bu notun penceresindeki sözcükler, sonra tablo; tekil ve sınırlı.
std::vector<std::string> completion_candidates(std::string_view text, std::size_t cursor,
                                               std::string_view type, const WordTable* table,
                                               const CompletionLimits& limits = {});

// `query` ile süzülmüş ve fzf puanına göre sıralı en çok `limit` aday. Boş sorguda gelen sıra.
std::vector<std::string> filter_candidates(const std::vector<std::string>& candidates, std::string_view query,
                                           std::size_t limit);

// Notları arka planda okuyup tabloyu kurar. Kendi NoteStore'unu kullanır: UI'nin store'u ayar değişince
// yeniden kurulabilir. `enable`a dek (ilk editör açılışı) hiçbir şey okunmaz: düzenlemeyen kullanıcı bedel
// ödemez.
class WordTableBuilder {
public:
    explicit WordTableBuilder(std::chrono::milliseconds idle, CompletionLimits limits = {});
    ~WordTableBuilder();
    WordTableBuilder(const WordTableBuilder&) = delete;
    WordTableBuilder& operator=(const WordTableBuilder&) = delete;

    void set_store(core::NoteStore store);
    void enable();
    void request();
    // Son kurulan tablo; henüz yoksa boş işaretçi. Hemen döner: tuş işlemeyi bekletmez.
    std::shared_ptr<const WordTable> latest() const;

private:
    void run(const std::stop_token& stop);

    std::chrono::milliseconds idle_;
    CompletionLimits limits_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<core::NoteStore> store_;
    bool enabled_ = false;
    bool pending_ = false;
    std::chrono::steady_clock::time_point requested_at_{};
    std::shared_ptr<const WordTable> table_;
    std::jthread worker_;  // son üye: önce o durur
};

}  // namespace noteapp::tui
