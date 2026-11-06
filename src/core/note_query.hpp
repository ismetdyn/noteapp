#pragma once

// Kullanıcının Cypher sorgusu NoteStore üzerinden (SPEC-graph "Sorgu çubuğu"; harita sorgu çubuğu, TUI `Q`).
// Okuma ve onaylı yazma aynı bağlantıda, deponun kilidi altında çalışır; yazma görüntüyü yeniler.
//
// Yazma uygulama şemasının türettiği veriye dokunamaz (motorun QueryHooks denetimi, değişiklikten önce): not
// gövdesi, yolu ve üst verisi, bağlantı (`LINKS_TO`), etiket (`TAGGED`, `Tag`), klasör (`Folder`, `IN_FOLDER`,
// `CHILD_OF`) ve görünüm (`View`) yalnız kendi API'leriyle değişir. Kullanıcı verisi yazılır: kendi etiketli
// düğümleri ve kenarları, notun ve etiketin ayrılmamış özellikleri, nota kullanıcı etiketi, `RELATED_TO`
// (`name` ilişki adı kuralıyla, `directed` yalnız true). Reddedilen yazma hiçbir şey yazmaz.
#include "core/error.hpp"
#include "core/note_store.hpp"
#include "graph/query.hpp"

#include <cstddef>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace noteapp::core {

// Yazmanın özeti: Neo4j'nin sayaçları. Özellik sayacı her yazmayı sayar (null yazmak, yani kaldırmak da).
struct QueryChanges {
    std::size_t nodes_created = 0;
    std::size_t nodes_deleted = 0;
    std::size_t relationships_created = 0;
    std::size_t relationships_deleted = 0;
    std::size_t properties_set = 0;
    std::size_t labels_added = 0;
    std::size_t labels_removed = 0;
    std::size_t total() const {
        return nodes_created + nodes_deleted + relationships_created + relationships_deleted + properties_set +
               labels_added + labels_removed;
    }
    bool operator==(const QueryChanges&) const = default;
};

// Sonuçtaki düğüm ya da kenarın uygulama gözünden adı. Metinler güvenilmezdir (senkron, Cypher): arayüz kendi
// temizliğinden geçirir.
struct QueryElement {
    enum class Kind {
        Note,         // canlı not; key yol
        TrashedNote,  // çöpteki not; key atıldığı yol
        Tag,          // key etiket anahtarı (Tag.name)
        Node,         // başka düğüm; key kimlik (32 onaltılık), name ilk etiketi
        Link,         // LINKS_TO; from/to uçların key'i
        Relation,     // RELATED_TO; name ilişki adı (ham), directed
        Tagged,       // TAGGED; from not, to etiket anahtarı
        Edge,         // başka kenar; key tipi
    };
    Kind kind = Kind::Node;
    std::string key;
    std::string from = {};  // kenar: kaynağın key'i (not yolu, etiket anahtarı ya da kimlik)
    std::string to = {};
    std::string name = {};
    bool directed = false;
    bool operator==(const QueryElement&) const = default;
};

struct UserQuery {
    graph::QueryResult result;
    QueryChanges changes;  // okumada sıfır
    // Sonuçtaki her düğüm ve kenar (listelerin içi de). Silinmiş ya da önizlemede kurulmuş öğe Node/Edge
    // olarak kimliğiyle görünür.
    std::map<graph::NodeId, QueryElement> nodes;
    std::map<graph::EdgeId, QueryElement> edges;
    // Yazma commit edildi ama sonuç adlanamadı (okuma hatası, süre): veri yazılmıştır, arayüz yine tazeler;
    // nodes/edges eksik olabilir. Okuma ve önizlemede bu durum hatadır, burası boş kalır.
    std::optional<Error> naming_error = std::nullopt;
};

enum class QueryAction {
    Read,     // salt-okunur: yazan sorgu InvalidQuery (error.query_writes), işlem açılmaz
    Preview,  // yazma çalışır ve denetlenir, commit edilmez: sonuç ve özet döner, graf değişmez
    Write,    // aynı, commit edilir
};

// Sorgu yazıyor mu: ayrıştırılır ve planlanır, depoya dokunmaz. Bozuk sorgu InvalidQuery (motorun konumlu
// iletisi: line, col).
std::expected<bool, Error> query_writes(std::string_view cypher, const graph::QueryLimits& limits);

// Kullanıcının sorgusu, kullanıcı profiliyle (limits.query.*; çağıran config'ten verir). Hatalar: sorgunun
// kendi sorunu, sınır ve korunan veri InvalidQuery (motorun ya da error.query_protected_* anahtarı, konumla);
// depo hatası Io. Depo yoksa okuma boş sonuçtur, dosya kurulmaz; yazma depoyu kurar. Geri alma yoktur.
// `params`: `$ad` değerleri (CLI `--param`; arayüzler vermez).
std::expected<UserQuery, Error> run_user_query(NoteStore& store, std::string_view cypher, QueryAction action,
                                               const graph::QueryLimits& limits, const graph::Params& params = {});

// Kayıtlı görünüm aynı biçimde (run_view: salt-okunur, görünüm profili).
std::expected<UserQuery, Error> run_saved_view(const NoteStore& store, std::string_view name);

}  // namespace noteapp::core
