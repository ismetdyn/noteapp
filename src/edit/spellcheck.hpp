#pragma once

#include "edit/editor_buffer.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace noteapp::tui {

// `tr` -> tr_TR, diğerleri en_US. Yapılandırma yok.
std::string dictionary_for_language(std::string_view language);
// Yalnız md ve txt; kod ve config'te sözcüklerin çoğu "yanlış" çıkardı.
bool spellcheck_applies_to(std::string_view type);

// `hunspell -a` üzerinden yazım denetimi; içerik makineden çıkmaz. Süreç ilk istekte başlar, nesneyle ölür.
// Tuşu asla bekletmez: `misspelled` önbelleğe bakar, bilinmeyen satırı kuyruğa atar ve boş döner; işçi boşta
// kalınca (`idle` kadar yeni istek yok) sorar, sonuç gelince `on_ready` çizimi tetikler. Program yoksa,
// takılırsa ya da ölürse oturum boyunca kapanır: yeniden denenmez.
class SpellChecker {
public:
    // `limits.tui.spellcheck_line_bytes` varsayılanı: bundan uzun satır denetlenmez.
    static constexpr std::size_t kDefaultMaxLineBytes = std::size_t{8} * 1024;
    static constexpr std::chrono::milliseconds kDefaultIdle{250};
    static constexpr std::chrono::milliseconds kDefaultTimeout{2000};

    SpellChecker(std::string dictionary, std::function<void()> on_ready,
                 std::chrono::milliseconds idle = kDefaultIdle,
                 std::chrono::milliseconds timeout = kDefaultTimeout,
                 std::size_t max_line_bytes = kDefaultMaxLineBytes);
    ~SpellChecker();
    SpellChecker(const SpellChecker&) = delete;
    SpellChecker& operator=(const SpellChecker&) = delete;

    // Satır içi bayt aralıkları; UI iş parçacığından çizim sırasında çağrılır.
    std::vector<ByteRange> misspelled(std::string_view line);
    bool disabled() const;

private:
    void run(const std::stop_token& stop);

    std::string dictionary_;
    std::function<void()> on_ready_;
    std::chrono::milliseconds idle_;
    std::chrono::milliseconds timeout_;
    std::size_t max_line_bytes_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::unordered_map<std::string, std::vector<ByteRange>> cache_;
    std::vector<std::string> pending_;
    std::chrono::steady_clock::time_point requested_at_{};
    bool disabled_ = false;
    std::jthread worker_;  // son üye: önce o durur, çocuk süreç onunla ölür
};

}  // namespace noteapp::tui
