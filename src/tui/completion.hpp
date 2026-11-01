#pragma once

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
WordTable build_word_table(const std::vector<NoteText>& notes);

// İmleçten önceki sözcük parçası; imleç sözcüğün sonunda değilse (arkasında sözcük harfi varsa) boş.
std::string_view word_prefix_at(std::string_view text, std::size_t cursor);

// Gölge öneri: önce bu nottan (imlece en yakın), sonra `table`dan; önerinin imleçten sonra eklenecek kısmı.
// En az iki baytlık önek ister. Süre not boyutundan bağımsızdır: not içinde imleç çevresinde sınırlı pencere.
std::optional<std::string> suggest_completion(std::string_view text, std::size_t cursor,
                                              std::string_view type, const WordTable* table);

// Açılır liste adayları: bu notun penceresindeki sözcükler, sonra tablo; tekil ve sınırlı.
std::vector<std::string> completion_candidates(std::string_view text, std::size_t cursor,
                                               std::string_view type, const WordTable* table);

// `query` ile süzülmüş ve fzf puanına göre sıralı en çok `limit` aday. Boş sorguda gelen sıra.
std::vector<std::string> filter_candidates(const std::vector<std::string>& candidates, std::string_view query,
                                           std::size_t limit);

// Notları arka planda okuyup tabloyu kurar. Kendi NoteStore'unu kullanır: UI'nin store'u ayar değişince
// yeniden kurulabilir. `enable`a dek (ilk editör açılışı) hiçbir şey okunmaz: düzenlemeyen kullanıcı bedel
// ödemez.
class WordTableBuilder {
public:
    explicit WordTableBuilder(std::chrono::milliseconds idle);
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
