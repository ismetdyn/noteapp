#pragma once

// Kaydedilmiş Cypher görünümleri (SPEC-graph "Görünümler"): `(:View {name, query, created_at, updated_at})`.
// Görünüm grafta durur, notlarla aynı günlükle senkronlanır. Başka cihazdan gelen görünüm güvenilmezdir:
// sorgusu kayıtta da çalıştırmada da salt-okunur denetimden geçer, yazan görünüm hiçbir şey değiştirmeden
// reddedilir.
#include "core/error.hpp"
#include "core/note_path.hpp"
#include "core/note_store.hpp"
#include "graph/query.hpp"

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noteapp::core {

struct SavedView {
    std::string name;   // ilk kaydedildiği yazım; senkronla gelebilir, güvenilmez
    std::string query;  // güvenilmez
    std::optional<std::chrono::system_clock::time_point> created_at = std::nullopt;
    std::optional<std::chrono::system_clock::time_point> updated_at = std::nullopt;
};

// Ad kuralı klasör adınınkidir (NotePath bileşeni), `/` yok. Eşleşme büyük/küçük harf katlamalıdır (tag_key).
std::expected<void, Error> validate_view_name(std::string_view name);

// Ada göre (katlanmış anahtar) sıralı. Depo yoksa boş.
std::expected<std::vector<SavedView>, Error> list_views(const NoteStore& store);
// Yoksa NotFound (error.view_not_found).
std::expected<SavedView, Error> find_view(const NoteStore& store, std::string_view name);
// Sorgu çalıştırılmadan ayrıştırılır ve planlanır: yazan, parametre isteyen ya da bozuk sorgu InvalidQuery,
// hiçbir şey yazılmaz. Aynı adda görünüm varsa overwrite olmadan AlreadyExists.
std::expected<void, Error>
save_view(NoteStore& store, std::string_view name, std::string_view query, bool overwrite,
          std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
std::expected<void, Error> remove_view(NoteStore& store, std::string_view name);

// Görünümü salt-okunur modda ve görünüm profiliyle (config limits.view.*; kökeni izlenmez, korumalı kalır)
// çalıştırır. Yazan sorgu InvalidQuery (error.view_writes) döner; işlem açılmaz, graf değişmez.
std::expected<graph::QueryResult, Error> run_view(const NoteStore& store, std::string_view name);

// `view:` süzgecinin notları: sonuçtaki canlı Note düğümleri ve canlı not yolunu taşıyan metin hücreleri.
// Satırı olan ama hiçbir sütunu düğüm ya da metin olmayan sonuç InvalidQuery (error.view_no_notes). Sonuç
// deponun görüntüsü değişene dek saklanır: tuş başına süzme sorguyu yeniden çalıştırmaz.
std::expected<std::vector<NotePath>, Error> view_notes(const NoteStore& store, std::string_view name);

}  // namespace noteapp::core
