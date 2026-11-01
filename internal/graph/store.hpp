#pragma once

// İç başlık: SQLite sınırı. SQL metni yalnız store.cpp'de, derleme anında sabit `Sql` olarak yaşar. Dışarıya
// kapalı `StatementId` üzerinden erişilir; çalışma anında SQL üretmenin yolu yoktur (SPEC-graph ilke 2).
#include "graph/error.hpp"
#include "graph/graph.hpp"
#include "graph/op.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace noteapp::graph::detail {

enum class StatementId : std::size_t {
    MetaGet,
    MetaPut,
    NodeGet,
    NodePut,
    NodeDelete,
    NodesAll,
    LabelGet,
    LabelPut,
    LabelDelete,
    LabelsOfNode,
    LabelsDeleteOfNode,
    LabelScan,
    EdgeGet,
    EdgePut,
    EdgeDelete,
    EdgesOut,
    EdgesOutOfType,
    EdgesIn,
    EdgesInOfType,
    PropVersions,
    PropPut,
    PropDeleteVersion,
    PropValuesOfOwner,
    PropsOfOwner,
    PropsDeleteOfOwner,
    LargeGet,
    LargePut,
    LargeDelete,
    OpPut,
    PeersOfDevice,
    OpById,
    OpLastId,
    OpGet,
    OpsOfDevice,
    PeerGet,
    PeerPut,
    PeerCount,
    Count
};

// Plan ve kaynak testleri içindir: tüm depolama SQL'i.
std::vector<std::string_view> statement_texts();

class Store;

// Havuzdan kiralanmış hazır ifade: yıkıcı sıfırlayıp havuza geri verir. İç içe gezinme aynı metni aynı anda
// kiralayabilir. bind_* çağrılarındaki veri step boyunca yaşamalıdır; kopyalanmaz.
class Statement {
public:
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    ~Statement();

    Statement& bind_blob(int index, std::span<const std::byte> blob);
    Statement& bind_text(int index, std::string_view text);
    Statement& bind_int(int index, std::int64_t value);
    // true: satır var. false: bitti.
    Result<bool> step();
    // Satırsız yazma ifadesi için step + reset.
    Result<void> run();

    std::span<const std::byte> column_blob(int index) const;
    std::string_view column_text(int index) const;
    std::int64_t column_int(int index) const;

private:
    friend class Store;
    Statement(Store& store, StatementId id, sqlite3_stmt* handle);
    void release() noexcept;

    Store* store_;
    StatementId id_;
    sqlite3_stmt* handle_;
};

// Bir çerçevenin uygulanma bağlamı: kim yazdı, hangi HLC'yle, neleri görmüştü.
class CausalContext {
public:
    CausalContext() = default;
    explicit CausalContext(std::vector<Dot> seen);
    // Bağlam bu yazmayı görmüş mü: dot.seq <= seen[dot.dev].
    bool covers(const Dot& dot) const;

private:
    std::vector<Dot> seen_;
};

struct ApplyContext {
    Dot dot;
    std::uint64_t hlc = 0;
    CausalContext seen;
};

class Store {
public:
    static Result<std::unique_ptr<Store>> open(const std::filesystem::path& path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    Result<Statement> statement(StatementId id);

    Result<void> begin_read();
    Result<void> begin_write();
    Result<void> commit_write();
    // Açık işlemi bitirir; açık değilse etkisizdir.
    void end_txn() noexcept;
    bool has_open_txn() const { return txn_ != TxnKind::None; }
    bool is_writing() const { return txn_ == TxnKind::Write; }
    // Salt-okunur sorgunun ikinci savunması: açıkken bağlantı veri yazamaz (checkpoint ve COMMIT'i
    // durdurmaz).
    Result<void> set_query_only(bool enabled);

    // Tek yazma yolu: işlemi uygular ve çerçeveye ekler. Depolama hatası işlemi zehirler.
    Result<void> record(Op op);
    // Register ve add-wins kuralı. Yerel yazma ve G2'nin uzak çerçeveleri buradan geçer; çerçeve kaydı
    // dışındadır.
    Result<void> apply(const ApplyContext& context, const Op& op);
    // Teslim kapısının sayacı: bu cihaz için meta.seq, başka cihaz için peers. Çerçeve uygulanmadıysa 0.
    Result<std::uint64_t> applied_seq(const DeviceId& device);
    Result<void> set_applied(const DeviceId& device, std::uint64_t seq);
    Result<std::uint64_t> peer_count();
    // Yeniden kurma batch'ler arasında ertelenir; çökmede sonraki ingest onu tamamlar.
    Result<bool> rebuild_pending();
    Result<void> set_rebuild_pending(bool pending);
    // Uzak HLC'yi yerel saate katar: görülen yazmanın üstüne yazan, onu HLC'de de geçsin. now+24h ötesi
    // alınmaz.
    Result<void> observe_hlc(std::uint64_t hlc);
    // Toplu ingest için NORMAL, sonra FULL. İşlem dışında çağrılır: SQLite işlem içinde reddeder.
    Result<void> set_bulk_sync(bool enabled);
    // Uzak çerçeve bütün uygulanır ya da iz bırakmaz.
    Result<void> begin_savepoint();
    Result<void> release_savepoint();
    Result<void> rollback_savepoint();
    // Tüm graph durumunu siler; ops ve meta kalır. Yeniden kurmadan önce çağrılır.
    Result<void> clear_graph();

    // Saklanan değeri çözer; büyük değer yalnız burada yüklenir.
    Result<Value> load_value(std::span<const std::byte> stored);
    // Büyük değeri yüklemeden: value null, large true, size içerik uzunluğu.
    struct StoredValue {
        Value value;
        std::uint64_t size = 0;
        bool large = false;
    };
    Result<StoredValue> peek_value(std::span<const std::byte> stored);
    Result<std::uint64_t> data_version();
    Result<void> set_lowest_priority();
    std::size_t pending_ops() const { return frame_.ops.size(); }
    const DeviceId& device() const { return device_; }
    std::int64_t last_insert_id() const;
    // Yalnız bağlantı ayarlarını sınayan test içindir.
    sqlite3* connection_for_test() const { return db_; }

private:
    friend class Statement;
    enum class TxnKind { None, Read, Write };

    explicit Store(sqlite3* db);
    Result<sqlite3_stmt*> prepare(StatementId id);
    void give_back(StatementId id, sqlite3_stmt* handle) noexcept;
    Result<void> configure();
    Result<void> initialize();
    Result<void> check_identity();
    Result<void> check_identity_in_snapshot();
    Result<void> enable_wal();
    Result<void> migrate();
    Result<void> load_device();
    Result<void> start_frame();
    Result<void> store_frame();

    sqlite3* db_;
    std::vector<std::vector<sqlite3_stmt*>> pool_;
    std::vector<std::size_t> created_;
    DeviceId device_{};
    TxnKind txn_ = TxnKind::None;
    bool poisoned_ = false;
    bool low_priority_ = false;
    ApplyContext apply_context_;
    Frame frame_;
};

// `sqlite3` hatasını motor hatasına çevirir.
Error storage_error(int code, std::string_view where);

}  // namespace noteapp::graph::detail
