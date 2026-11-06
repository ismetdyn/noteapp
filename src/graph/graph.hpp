#pragma once

#include "graph/error.hpp"
#include "graph/id.hpp"
#include "graph/query.hpp"
#include "graph/value.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace noteapp::graph {

namespace detail {
class Store;
}

enum class Direction { Out, In, Both };

// `type`, sonraki next() çağrısına kadar geçerlidir: gezinme satır başına dizge ayırmasın diye.
struct Neighbor {
    EdgeId edge;
    NodeId node;
    std::string_view type;
};

// Ömür sırası Graph > Txn > cursor: cursor, işleminden ve Graph'tan önce yok edilmelidir.
class NeighborCursor {
public:
    NeighborCursor(NeighborCursor&&) noexcept;
    NeighborCursor& operator=(NeighborCursor&&) noexcept;
    ~NeighborCursor();
    Result<std::optional<Neighbor>> next();

private:
    friend class ReadTxn;
    struct Impl;
    explicit NeighborCursor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

class NodeCursor {
public:
    NodeCursor(NodeCursor&&) noexcept;
    NodeCursor& operator=(NodeCursor&&) noexcept;
    ~NodeCursor();
    Result<std::optional<NodeId>> next();

private:
    friend class ReadTxn;
    struct Impl;
    explicit NodeCursor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Bir özelliğin eşzamanlı head'i: (device, seq) yazmanın kimliği, hlc sıralama anahtarı.
struct Version {
    std::array<std::byte, 16> device{};
    std::uint64_t seq = 0;
    std::uint64_t hlc = 0;
    Value value;
};

// Bir sahibin bir head'i, toplu okuma içindir: büyük değer yüklenmez (large true, value null), boyutu yine
// bilinir. size: dize ve bayt değerinde içerik uzunluğu, diğerlerinde kodlu uzunluk.
struct PropertyHead : Version {
    std::string key;
    std::uint64_t size = 0;
    bool large = false;
};

using OwnerId = std::variant<NodeId, EdgeId>;

// Kenarın kimliğine bağlı uçları ve tipi (WriteTxn::put_edge aynı kimlikle başkasını reddeder).
struct EdgeEnds {
    NodeId src;
    std::string type;
    NodeId dst;
    bool operator==(const EdgeEnds&) const = default;
};

// Tek bir anlık görüntü üzerinde okur. Biten veya taşınmış işlemdeki her çağrı hata döner.
class ReadTxn {
public:
    ReadTxn(ReadTxn&& other) noexcept;
    ReadTxn& operator=(ReadTxn&& other) noexcept;
    ~ReadTxn();

    Result<bool> has_node(NodeId id);
    // Ayaktaki kenarın uçları ve tipi; kenar yoksa (hiç kurulmadı ya da silindi) nullopt.
    Result<std::optional<EdgeEnds>> edge(EdgeId id);
    Result<std::vector<std::string>> labels(NodeId id);
    // Başlangıç kısıtı olmayan desen içindir; maliyeti tüm düğümlerdir.
    Result<NodeCursor> nodes();
    Result<NodeCursor> nodes_with_label(std::string_view label);
    // Sıra (tip, karşı düğüm, kenar). Both'ta önce çıkan, sonra giren kenarlar; öz döngü bir kez görünür.
    Result<NeighborCursor> neighbors(NodeId id, Direction direction,
                                     std::optional<std::string_view> type = {});
    // En büyük (hlc, device) head'i; özellik yoksa veya head null ise null.
    Result<Value> property(OwnerId owner, std::string_view key);
    // Tüm eşzamanlı head'ler. Sıra (device, seq).
    Result<std::vector<Version>> versions(OwnerId owner, std::string_view key);
    // Sahibin tüm anahtarlarının head'leri, tek aralık okumasıyla. Sıra (key, device, seq).
    Result<std::vector<PropertyHead>> properties(OwnerId owner);
    // Tek yazmanın (cihaz, seq) bu sahibe koyduğu özellikler, işlem günlüğünden: artık head olmasa da.
    // Çözümler (üç yönlü birleştirme tabanı) geçmişe böyle bakar. Yazma bilinmiyorsa boş. Çerçevenin tamamını
    // çözer: maliyeti gövde kadardır.
    Result<std::vector<PropertyHead>> properties_at(const Version& write, OwnerId owner);
    // `later` yazması yazıldığında `earlier` yazmasını görmüş müydü? Uygulama, özellikler arası nedenselliği
    // (ör. bir silme, düzenlemeyi görmüş mü) bununla sorar. Bilinmeyen yazma görülmemiş sayılır.
    Result<bool> saw(const Version& later, const Version& earlier);

protected:
    friend class Graph;
    explicit ReadTxn(detail::Store& store);
    detail::Store* store_;
};

// Graph başına tek açık işlem. Her yazma, aynı depolama işleminde işlem günlüğüne girer. commit çağrılmazsa
// yıkıcı geri alır. Bir yazma depolama hatasıyla düşerse işlem zehirlenir: kısmi durum commit edilemez,
// commit de hata döner.
class WriteTxn : public ReadTxn {
public:
    WriteTxn(WriteTxn&&) noexcept = default;
    WriteTxn& operator=(WriteTxn&&) noexcept = default;
    ~WriteTxn() = default;

    Result<void> put_node(NodeId id);
    // detach=false iken kenarı olan düğüm silinmez; detach=true kenarları da siler.
    Result<void> delete_node(NodeId id, bool detach);
    Result<void> add_label(NodeId id, std::string_view label);
    Result<void> remove_label(NodeId id, std::string_view label);
    // Kenarın uçları ve tipi kimliğine bağlıdır: aynı kimlikle farklı uç veya tip reddedilir.
    Result<void> put_edge(EdgeId id, NodeId src, std::string_view type, NodeId dst);
    Result<void> delete_edge(EdgeId id);
    Result<void> set_property(OwnerId owner, std::string_view key, Value value);
    // Bu işlemin yazmaları eşzamanlı head'ler arasında en düşük önceliği alır: gören her yazma onları zaten
    // geçer, görmeyen de sıralamada önde kalır. İlk yazmadan önce çağrılır. Toplu içe aktarma içindir: eski
    // veri, cihazın saati ne olursa olsun, kimsenin yeni yazmasını geçemez.
    Result<void> set_lowest_priority();
    // Bu işlemin biriken işlem sayısı: çağıran işi kendi bütçesiyle işlemlere bölebilsin. Çerçeve sınırı
    // işlemi sınırlamaz (L4b: export işlemi kayıtlara böler).
    std::size_t pending_ops() const;
    Result<void> commit();

private:
    friend class Graph;
    using ReadTxn::ReadTxn;
};

// Birleşimin raporu. conflicts: bu ingest'te dokunulan, farklı değerli çok head'li özellikler. bad_frames:
// reddedilen veya etkisiz kılınan çerçeve. stalled: bu ingest sonunda ilerleyemeyen günlük.
struct MergeConflict {
    OwnerId owner;
    std::string key;
};

struct BadFrameReport {
    std::filesystem::path log;
    std::array<std::byte, 16> device{};
    std::uint64_t seq = 0;  // 0: çerçeve çözülemeden düştü
    std::string reason;
};

struct StalledLog {
    std::filesystem::path log;
    std::array<std::byte, 16> device{};
    std::uint64_t applied_seq = 0;
    std::string reason;
};

// Sondaki yarım çerçeve hata değildir ama görünür olmalı: dosya sonsuza dek yarım kalabilir.
struct PartialTail {
    std::filesystem::path log;
    std::array<std::byte, 16> device{};
    std::uint64_t valid_bytes = 0;
    std::uint64_t file_bytes = 0;
};

// log_seq < applied_seq: cihazın günlüğü bu veritabanının gördüğünden geride, geri alınmış olabilir.
struct DeviceHead {
    std::array<std::byte, 16> device{};
    std::uint64_t log_seq = 0;
    std::uint64_t applied_seq = 0;
};

struct MergeReport {
    std::vector<MergeConflict> conflicts;
    std::vector<BadFrameReport> bad_frames;
    std::vector<StalledLog> stalled;
    std::vector<PartialTail> partial_tails;
    std::vector<DeviceHead> heads;
    std::uint64_t omitted = 0;  // liste sınırına takılan bad_frames ve conflicts kaydı
};

// Yerel günlük bu veritabanının geçmişi değilse kenara alınır, silinmez; yerine ops'tan üretilir.
struct ExportReport {
    std::optional<std::filesystem::path> set_aside;
    std::string reason;
    std::uint64_t last_seq = 0;  // dosyaya giren son işlem
};

// Motorun kapasite sınırları (SPEC-graph "Sınırlar"). Uygulama değerleri config'ten alır (limits.graph.*);
// varsayılanlar oradakilerle aynıdır. Sınırsız: SIZE_MAX (hlc_skew için milliseconds::max()).
// Çerçeve sınırları ve peers senkronda her cihazda aynı olmalıdır: büyük değerli cihazın kaydını küçük
// değerli cihaz bad_frame/peer_limit ile bekletir (veri kaybı yok; değer yükselince sonraki ingest sürer).
// Çerçeve sınırları yalnız günlükteki tek kaydı sınırlar; yerel işlem ve değer onlara takılmaz (L4b).
struct GraphLimits {
    std::size_t frame_bytes = 64U * 1024U * 1024U;  // tek kaydın baytı; depolamanın tek değer sınırına kırpılır
    std::size_t frame_ops = 65536;                  // kayıt başına işlem (parçalı çerçevede parça başına)
    // Bir uzak işlemin (parçalı çerçeve grubunun) bellekte birleştirilen baytı; depolamanın tek değer sınırı
    // ayrıca geçerlidir. Bellekte en çok bir yarım grup tutulur.
    std::size_t group_bytes = std::size_t{1} << 30U;
    // Bir uzak işlemin çözülen işlem sayısı. Çözülmüş işlem 104 bayttır (sizeof(Op), x86-64): 4Mi işlem
    // ~436 MB; değer ve ad baytları ayrıca en çok group_bytes kadar. Kayıt sayısını gönderen seçtiğinden
    // parça x frame_ops tek başına belleği sınırlamaz.
    std::size_t group_ops = std::size_t{1} << 22U;
    // Tanınan uzak cihaz. Çerçevenin bağımlılık (deps) sınırı bunun 4 katıdır (1024 = 4 × 256): başka
    // cihazın tanıdığı ama bu cihazın henüz tanımadığı cihazlar için pay.
    std::size_t peers = 256;
    // now + bu süreden ileri HLC kendi saatine alınmaz (sıralama için yine kabul edilir).
    std::chrono::milliseconds hlc_skew = std::chrono::hours{24};
    std::uint64_t ingest_bytes = std::uint64_t{8} * 1024U * 1024U * 1024U;  // bir ingest'in okuduğu yeni bayt
    std::size_t merge_owners = std::size_t{1} << 23U;        // birleşimin durum tuttuğu düğüm + kenar
    std::size_t report_entries = 4096;                       // MergeReport'taki bad_frames ve conflicts kaydı
    std::size_t touched_properties = std::size_t{1} << 20U;  // çakışma raporu için izlenen özellik
    std::size_t set_aside_logs = 3;                          // tutulan kenara alınmış yerel günlük kopyası

    bool operator==(const GraphLimits&) const = default;
};

// Thread-safe değildir: her thread kendi Graph'ını açar. Süreçler aynı dosyayı birlikte kullanabilir.
class Graph {
public:
    // Dosya yoksa kurar. Daha yeni şema TooNew, yabancı dosya Corrupt döner; reddedilen dosyaya yazılmaz.
    static Result<Graph> open(const std::filesystem::path& path, const GraphLimits& limits = {});

    Graph(Graph&&) noexcept;
    Graph& operator=(Graph&&) noexcept;
    ~Graph();

    Result<ReadTxn> read();
    // Yazıcılar süreçler arası sıralanır; kilit busy_timeout içinde alınamazsa Busy döner.
    Result<WriteTxn> write();
    // Cypher alt kümesi (SPEC-graph "Sorgu dili"). Kendi işlemini açar: açık işlem varken hata döner. Hata
    // veren yazma sorgusunun hiçbir etkisi kalmaz.
    Result<QueryResult> query(std::string_view cypher, const Params& params = {},
                              QueryMode mode = QueryMode::ReadOnly, QueryLimits limits = {});

    // Bu cihazın işlemlerini `<directory>/<cihaz>.oplog` dosyasına ekler. Dosya bozuk veya yarımsa ops'tan
    // yeniden üretir. Dosya bu veritabanından ileride veya farklıysa dokunmaz, hata döner.
    Result<ExportReport> export_log(const std::filesystem::path& directory);
    // Günlükleri (kendininki dahil) nedensel sırayla uygular. Girdi güvenilmezdir: bozukluk bildirilir, hata
    // dönmez; hata yalnız yerel depolama içindir. Aynı girdiyle tekrar çağrı etkisizdir.
    Result<MergeReport> ingest_logs(std::span<const std::filesystem::path> logs);

    // Bu veritabanının cihaz kimliği (`<cihaz>.oplog` dosyasının adı).
    std::array<std::byte, 16> device() const;
    // Başka bağlantının commit'lerinde değişir; kendi yazmalarında değişmez. Tüm grafı okumadan "dışarıdan
    // değişti mi" sorusunun cevabıdır. Açık işlem içinde de sorulabilir.
    Result<std::uint64_t> data_version();
    // Bu cihazın son işlem numarası; 0: hiç yazmadı.
    Result<std::uint64_t> last_seq();
    // Etkin kayıt sınırı: limits.frame_bytes ile depolamanın tek değer sınırının küçüğü. Toplu yazanların
    // işlem başına bayt payı bundan türer (SPEC-graph "Toplu yazanlar").
    std::size_t max_frame_bytes() const;
    // Depolamanın tek değer sınırı: bir değer ve bir işlemin kodlu toplamı buna sığmalıdır; aşan yazma
    // LimitExceeded döner. Çerçeve sınırından bağımsızdır.
    std::size_t max_stored_bytes() const;

private:
    explicit Graph(std::unique_ptr<detail::Store> store);
    std::unique_ptr<detail::Store> store_;
};

// Sorguyu çalıştırmadan ayrıştırır ve planlar: sözdizimi, eksik parametre ve salt-okunur ihlali query()'yle
// aynı hatayı verir. Depo gerekmez; saklanıp sonra çalıştırılacak sorgunun kayıttaki denetimidir. `limits`
// çalıştırılacağı profilinkidir (görünümde limits.view.*).
Result<void> check_query(std::string_view cypher, QueryMode mode, const Params& params = {},
                         const QueryLimits& limits = {});

}  // namespace noteapp::graph
