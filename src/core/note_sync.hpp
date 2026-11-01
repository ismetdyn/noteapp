#pragma once

// Senkronun not yarısı: günlük alma, verme ve gövde çakışmasının kopyaya dönüşmesi. sync modülü git'i bilir,
// not semantiği burada kalır (SPEC-graph ilke 5).
#include "core/error.hpp"
#include "core/note_store.hpp"
#include "core/text_merger.hpp"
#include "graph/graph.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::core {

struct LogIngest {
    graph::MergeReport report;
    std::vector<NotePath> conflict_copies;  // bu çağrıda oluşan kopyalar
    std::vector<Error> unresolved;          // çözülemeyen çakışma: head'ler durur, senkron sürer
    int changed_notes = 0;                  // canlı listede eklenen ya da gövdesi değişen not
};

// Günlükleri uygular ve gövdede farklı değerli head'i olan HER notu çözer: rapor sınıra takılsa (omitted) ya
// da önceki süreç iki adım arasında ölse de kaybeden sürüm kopyaya çıkar. Hata yalnız yerel depolamadandır.
std::expected<LogIngest, Error> ingest_note_logs(NoteStore& store,
                                                 std::span<const std::filesystem::path> logs,
                                                 const TextMerger& merger = {});
std::expected<graph::ExportReport, Error> export_note_log(NoteStore& store,
                                                          const std::filesystem::path& directory);
// Bu cihazın günlük dosyasının adı: `<32 hex>.oplog`.
std::expected<std::string, Error> own_log_name(NoteStore& store);
// Bu cihazın since'tan sonra yazdığı, canlı ya da çöpte duran not sayısı ("gönderilmemiş değişiklik").
std::expected<int, Error> count_local_changes(NoteStore& store, std::uint64_t since);
// Bu cihazın son işlem numarası (0: hiç yazmadı): "gönderildi" karşılaştırması içindir.
std::expected<std::uint64_t, Error> own_last_seq(NoteStore& store);

}  // namespace noteapp::core
