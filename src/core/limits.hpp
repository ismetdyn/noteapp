#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace noteapp::core {

// Sınır değerinin birimi: yazımda hangi eklerin geçerli olduğunu ve `Limit::value`'nun neyi saydığını
// belirler.
enum class LimitUnit {
    Count,         // yalnız tam sayı
    Bytes,         // B, KB, MB, GB, TB (10^3) ve KiB, MiB, GiB, TiB (2^10); değer bayt
    Milliseconds,  // ms, s, min, h; değer milisaniye
};

// "64MiB", "250ms", "1000" -> taban birimde değer. Ek büyük/küçük harf duyarlıdır, boşluk ve işaret kabul
// edilmez; sonuç int64 sınırını aşarsa (TOML tam sayısı) nullopt.
std::optional<std::uint64_t> parse_limit(std::string_view text, LimitUnit unit);

// Kullanıcının kapasite sınırı. `0 = sınırsız`: modüller 0 ile karşılaştırmaz, allows/cap kullanır.
struct Limit {
    std::uint64_t value{};  // taban birimde (adet, bayt, ms)
    std::string text;       // dosyadaki yazım ("64MiB", "0"); listede gösterilir, kayıtta korunur

    bool unlimited() const noexcept { return value == 0; }
    // `amount` kadarına izin var mı (sınır dahil).
    bool allows(std::uint64_t amount) const noexcept { return value == 0 || amount <= value; }
    // Sayaç/kesit üst değeri: sınırsızda size_t'nin en büyüğü.
    std::size_t cap() const noexcept {
        return value == 0 ? SIZE_MAX : static_cast<std::size_t>(std::min<std::uint64_t>(value, SIZE_MAX));
    }

    bool operator==(const Limit&) const = default;
};

// [limits.<modül>] tabloları. Anahtar ekleme: defaults.toml'a bir satır, buraya bir alan, config.cpp
// limit_keys()'e bir satır ve bir test (SPEC-core "Sınırlar"). Tablonun boyu satırlardan çıkar.
// [limits.app]: iki arayüzün ortak uygulama katmanı (src/app).
struct AppLimits {
    Limit recent_commands;   // komut paletinde hatırlanan son kullanılan komut (TUI ve GUI)
    Limit trash_undo_steps;  // oturumluk çöp geri alma yığınının derinliği (TUI ve GUI)
    Limit tabs;              // aynı anda açık editör sekmesi (TUI ve GUI)
    Limit map_nodes;         // haritada çizilen en çok düğüm (TUI ve GUI); aşan kısım uyarıyla düşer
    Limit map_depth;         // harita çevre kapsamının en çok adımı (TUI tuşları, GUI adımlayıcısı)

    bool operator==(const AppLimits&) const = default;
};

// [limits.core]: not deposu (NoteStore).
struct CoreLimits {
    // Görüntünün çözdüğü en uzun klasör yolu. Güvenilmez girdi koruması: görüntü her klasörün tam yolunu
    // tutar, senkronla gelen N düzeylik zincir ~N² bayt olurdu. Aşan klasörün notları raporlanır
    // (`invalid_folder`).
    Limit folder_path_bytes;

    bool operator==(const CoreLimits&) const = default;
};

struct SearchLimits {
    Limit content_results;        // içerik aramasında listelenen en çok not (TUI ve GUI)
    Limit global_results;         // genel aramada (ad + içerik) listelenen en çok not
    Limit content_hits;           // içerik sonucunda not başına kesiti üretilen eşleşen satır
    Limit fuzzy_candidate_chars;  // fuzzy eşleşmenin konum ürettiği en uzun aday (kod noktası)

    bool operator==(const SearchLimits&) const = default;
};

// [limits.sync]: senkron (src/sync).
struct SyncLimits {
    // Üç yönlü gövde birleştirmesinin en büyük girdisi; aşan çakışma kopya not olur. Her cihazda aynı olmalı:
    // farklıysa biri birleştirir, öteki kopya kurar (kayıp yok, fazladan kopya).
    Limit merge_input_bytes;

    bool operator==(const SyncLimits&) const = default;
};

// [limits.graph]: motorun sınırları (graph::GraphLimits). frame_bytes, frame_ops ve peers senkronda her
// cihazda aynı olmalıdır (SPEC-graph "Sınırlar"); diğerleri cihaza özgüdür, birleşimin sonucunu değiştirmez.
struct GraphLimits {
    Limit frame_bytes;         // günlükteki tek kaydın baytı; not ve işlem parçalara bölünür (L4b)
    Limit frame_ops;           // kayıt başına işlem; kullanıcı işlemi reddedilmez
    Limit group_bytes;         // bellekte birleştirilen bir uzak işlem (parçalı grup); aşan günlük bekler
    Limit group_ops;           // bir uzak işlemin çözülen işlem sayısı; çözülmüş belleği sınırlar
    Limit peers;               // tanınan uzak cihaz; çerçevenin deps sınırı 4 katıdır
    Limit hlc_skew;            // ileri saatli uzak HLC'nin kendi saate alınmadığı sapma
    Limit ingest_bytes;        // bir senkronda okunan yeni günlük baytı; kalan sonraki senkrona
    Limit merge_owners;        // birleşimin bellekte izlediği düğüm + kenar
    Limit report_entries;      // senkron raporundaki bozuk çerçeve ve çakışma kaydı
    Limit touched_properties;  // çakışma raporu için izlenen özellik
    Limit set_aside_logs;      // tutulan kenara alınmış yerel günlük kopyası
    Limit import_line_bytes;   // `note graph import` satırı
    Limit import_line_values;  // aynı satırın JSON değeri (ayrıştırılmış belleği sınırlar)

    bool operator==(const GraphLimits&) const = default;
};

// [limits.query] kullanıcının kendi sorgusu (`note query`), [limits.view] görünüm (`view:`, `note view run`).
// Görünüm başka cihazdan gelebilir ve kökeni izlenmez: kendi korumalı profilini kullanır.
struct QueryLimits {
    Limit rows;
    Limit expansions;
    Limit duration;
    Limit memory;
    Limit memo;
    Limit memo_value;
    Limit text;
    Limit nesting;
    Limit pattern_elements;
    Limit clauses;
    Limit var_length_hops;
    Limit plan_nodes;

    bool operator==(const QueryLimits&) const = default;
};

// [limits.tui]: terminal arayüzü (src/tui). Değerler arayüzün tuş bütçesini (SPEC-tui ≤ 50 ms) korur; aşan
// iş reddedilmez, düz metne düşer ya da soluk satırla söylenir. 0'da büyük not o işin süresini uzatır.
struct TuiLimits {
    Limit view_bytes;               // REVIEW biçimli görünümünün (md, csv, json) en büyük notu
    Limit view_lines;               // biçimli görünümün satırı
    Limit table_rows;               // biçimli tablonun (csv, md) satırı
    Limit table_columns;            // biçimli tablonun saklanan sütunu
    Limit highlight_bytes;          // sözdizimi renklendirilen en büyük not
    Limit highlight_line_bytes;     // renklendirilen en uzun satır
    Limit spellcheck_line_bytes;    // yazım denetlenen en uzun satır
    Limit completion_window_bytes;  // tamamlamanın imleç çevresinde taradığı pencere (iki yana)
    Limit completion_word_bytes;    // önerilen en uzun sözcük
    Limit completion_table_words;   // diğer notların sözcük tablosu
    Limit completion_candidates;    // açılır listenin aday havuzu
    Limit completion_note_words;    // açılır listeye bu nottan giren sözcük
    Limit completion_note_bytes;    // sözcük tablosuna okunan en büyük not
    Limit completion_table_bytes;   // sözcük tablosuna okunan toplam bayt
    Limit paste_bytes;              // sistem panosundan yapıştırılan metin
    Limit search_history;           // hatırlanan arama sorgusu
    Limit search_entry_bytes;       // hatırlanan en uzun sorgu
    Limit undo_bytes;               // editörde not başına geri alma geçmişi
    Limit save_suggestions;         // taşıma diyaloğunda gösterilen klasör önerisi
    Limit stray_notice_files;       // açılışta geçiş dışı dosya taramasının dosya bütçesi
    Limit stray_notice_bytes;       // aynı taramanın bayt bütçesi

    bool operator==(const TuiLimits&) const = default;
};

// [limits.lsp]: dil sunucusu (TUI). Sunucu çıktısı güvenilmezdir: varsayılanlar korumadır, 0'ın riski
// defaults.toml'da yazılıdır.
struct LspLimits {
    Limit note_bytes;                // sunucuya gönderilen ve yansıtılan en büyük not
    Limit header_bytes;              // çerçeve başlığı
    Limit body_bytes;                // çerçeve gövdesi
    Limit message_depth;             // gövdenin JSON iç içeliği (özyinelemeli ayrıştırıcı)
    Limit message_values;            // gövdenin JSON değer sayısı
    Limit diagnostics;               // belge başına tanı
    Limit diagnostic_message_bytes;  // tanı iletisi
    Limit completion_items;          // öneri cevabındaki öğe
    Limit completion_insert_bytes;   // öğenin ekleme metni; aşan öğe atlanır
    Limit completion_label_bytes;    // öğenin etiketi; aşan öğe atlanır
    Limit pending_reply_bytes;       // sunucu isteklerine bekleyen cevapların toplamı
    Limit initialize_timeout;        // initialize cevabı
    Limit write_timeout;             // sunucunun stdin'i okumaması
    Limit shutdown_timeout;          // kapanışta shutdown cevabı

    bool operator==(const LspLimits&) const = default;
};

struct Limits {
    AppLimits app{};
    CoreLimits core{};
    SearchLimits search{};
    SyncLimits sync{};
    GraphLimits graph{};
    QueryLimits query{};
    QueryLimits view{};
    TuiLimits tui{};
    LspLimits lsp{};

    bool operator==(const Limits&) const = default;
};

}  // namespace noteapp::core
