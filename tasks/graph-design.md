# Tasarım: `noteapp_graph`

> Spec: [SPEC-graph.md](../SPEC-graph.md) · Plan: [plan-graph.md](plan-graph.md) · Kaynak: `cpp-architect` tasarımı (2026-11-01), lead onaylı.
> Süre ve satır sayıları tahmindir. Pragma ve SQLite bayrakları 3.54.1 belgesinden G1'de doğrulanacak.

## Graph ruhunu koruyan mekanik kurallar

1. SQL yalnız `consteval` sabit metindir, tek tabloda nokta veya aralık okur. `JOIN` ve `WITH RECURSIVE` yasaktır; test kaynakta tarar.
2. Tablo kümesi kapalıdır; yeni etiket veya tip eklenince `sqlite_master` değişmez (test).
3. Graph'ın izdüşümü olan durum tablosu yoktur; birleştirme meta verisi graph satırındadır.
4. Yol saklanmaz: `Note.name` + `IN_FOLDER`, `CHILD_OF` gezilerek türetilir. Klasör adı değişimi tek yazmadır.

## API (`src/graph/{error,id,value,graph}.hpp`, pimpl, başlıkta SQLite ve "not" yok)

```cpp
struct Error { enum class Code { InvalidArgument, NotFound, Busy, Corrupt, TooNew, Syntax, Unsupported, ReadOnlyViolation, LimitExceeded, BadFrame, Io } code; std::string message_key; std::vector<std::pair<std::string, std::string>> args; };
template <class T> using Result = std::expected<T, Error>;
struct NodeId { std::array<std::byte, 16> bytes; auto operator<=>(const NodeId&) const = default; };  // EdgeId aynı
NodeId new_node_id(); EdgeId new_edge_id();  // UUIDv7
NodeId derive_node_id(std::string_view ns, std::span<const std::byte> canonical);  // UUIDv8: SHA-256 ilk 16 bayt; derive_edge_id aynı
struct Bytes { std::string data; }; struct Timestamp { std::int64_t unix_ms; };
using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string, Bytes, Timestamp>;  // NaN ve geçersiz UTF-8 string reddedilir
enum class Direction { Out, In, Both };  struct Neighbor { EdgeId edge; NodeId node; std::string_view type; };  // view sonraki next()'e kadar geçerli
class NeighborCursor { public: Result<std::optional<Neighbor>> next(); };  class NodeCursor { public: Result<std::optional<NodeId>> next(); };
struct Version { std::array<std::byte, 16> device; std::uint64_t seq, hlc; Value value; };  using OwnerId = std::variant<NodeId, EdgeId>;
class ReadTxn { public: Result<bool> has_node(NodeId); Result<std::vector<std::string>> labels(NodeId); Result<NodeCursor> nodes_with_label(std::string_view); Result<NeighborCursor> neighbors(NodeId, Direction, std::optional<std::string_view> type = {}); Result<Value> property(OwnerId, std::string_view key); Result<std::vector<Version>> versions(OwnerId, std::string_view key); };
class WriteTxn : public ReadTxn { public: Result<void> put_node(NodeId), delete_node(NodeId, bool detach), add_label(NodeId, std::string_view), remove_label(NodeId, std::string_view), put_edge(EdgeId, NodeId src, std::string_view type, NodeId dst), delete_edge(EdgeId), set_property(OwnerId, std::string_view key, Value), commit(); };  // ~WriteTxn: commit yoksa rollback
class Graph { public: static Result<Graph> open(std::filesystem::path); Result<ReadTxn> read(); Result<WriteTxn> write(); Result<QueryResult> query(std::string_view cypher, const Params&, QueryMode = QueryMode::ReadOnly, QueryLimits = {}); Result<ExportReport> export_log(const std::filesystem::path& directory); Result<MergeReport> ingest_logs(std::span<const std::filesystem::path>); };  // MergeReport: conflicts, stalled, bad_frames, partial_tails, heads, omitted. ExportReport: set_aside, reason
```

- **Yaşam süresi:** `Graph` taşınabilir, kopyalanamaz. Ömürler Graph > Txn > cursor. Graph başına tek açık Txn.
- **Thread ve süreç:** `Graph` thread-safe değildir, her thread kendi `Graph`'ını açar. Süreçler arası WAL; `write()` `BEGIN IMMEDIATE`. `seq` ve HLC `meta`'dan işlem içinde okunur.
- **Hata:** API istisna fırlatmaz. `Error`, `core::Error` ile aynı şekildedir; G4'te çevrilir. Cypher konumu `args`'ta (`line`, `col`). İhlal edilen iç invariant abort eder.

## SQLite şeması (`user_version=1`, sabit `application_id`)

```sql
meta(k TEXT PRIMARY KEY, v BLOB) WITHOUT ROWID;
nodes(id BLOB PRIMARY KEY, dots BLOB NOT NULL) WITHOUT ROWID;
labels(node BLOB, label TEXT, dots BLOB NOT NULL, PRIMARY KEY(node, label)) WITHOUT ROWID;
CREATE INDEX labels_scan ON labels(label, node);
edges(id BLOB PRIMARY KEY, src BLOB NOT NULL, type TEXT NOT NULL, dst BLOB NOT NULL, dots BLOB NOT NULL) WITHOUT ROWID;
CREATE INDEX edges_out ON edges(src, type, dst);
CREATE INDEX edges_in ON edges(dst, type, src);
props(owner BLOB, key TEXT, dev BLOB, seq INT, hlc INT, value BLOB NOT NULL, PRIMARY KEY(owner, key, dev, seq)) WITHOUT ROWID;
large_values(id INTEGER PRIMARY KEY, data BLOB NOT NULL);
ops(id INTEGER PRIMARY KEY, dev BLOB NOT NULL, seq INT NOT NULL, frame BLOB NOT NULL, UNIQUE(dev, seq));
peers(dev BLOB, other BLOB, seq INT, PRIMARY KEY(dev, other)) WITHOUT ROWID;
```

- **Kimlik:** 16 baytlık kimlik doğrudan anahtardır; tip ve anahtar doğrudan metindir, sözlük tablosu yoktur. `INTEGER` surrogate yalnız G1 performans kapısı düşerse `store.cpp` içinde değerlendirilir.
- **Değer kodu:** `value` BLOB: `[tag][payload]`. Oplog ile aynı codec. Karşılaştırma C++'tadır.
- **Tembel yükleme:** > 200 bayt değer `large_values`'a gider, `props`'ta `[tag|0x80][id][len]` kalır.
- **Bağlantı:** `journal_mode=WAL`, `synchronous=FULL` (toplu ingest'te `NORMAL`), `busy_timeout=5000`, `trusted_schema=OFF`, `DEFENSIVE=1`, `cache_size=-8000`. `foreign_keys` kapalı; invariant'ı tek yazma yolu korur. Derleme: `SQLITE_THREADSAFE=2`, `SQLITE_DQS=0`, `SQLITE_OMIT_LOAD_EXTENSION`.
- **Migration:** Tek işlemde adım adım. `user_version` büyükse `TooNew`, yabancı `application_id` ise `Corrupt`.
- **Prepared statement:** Sabit `kStatements[]`, tembel `sqlite3_prepare_v3(PERSISTENT)`, metin başına kiralanan havuz (iç içe Expand için).
- **Maliyet:** Expand başına bir B-tree seek + komşu başına bir step: O(gezilen × log E).
- **Kararlılık:** `ANALYZE` ve `PRAGMA optimize` çalışmaz. Adjacency SQL'i `INDEXED BY` ile sabittir. Golden test: `sqlite_stat1` varken ve yokken `EXPLAIN QUERY PLAN` aynı.

## Sorgu hattı (`src/graph/cypher/`, `std::regex` yok)

- **Lexer/parser:** El yazımı recursive descent; ≤ 64 KiB sorgu, ≤ 64 derinlik, ≤ 256 desen öğesi. `LOAD CSV`, `CALL`, `UNWIND` tanınır, konumlu `Unsupported` döner.
- **Planner:** Maliyet modeli yok. Başlangıç: bağlı değişken > `id(n)=…` > etiket + eşitlik > etiket > tüm düğüm; eşitlikte soldaki. `WHERE`, `AND`'den bölünüp değişkenin bağlandığı en erken işlece iner.
- **İşleçler (`next(Row&)`):** `NodeScan`, `LabelScan`, `Expand`, `ExpandInto`, `VarLengthExpand`, `Filter`, `OptionalApply`, `Project`, `Aggregate`, `Sort` (LIMIT varsa top-K), `Skip`, `Limit`, `Distinct`, `Create`, `Merge`, `Set`, `Remove`, `Delete`. Yazma işleci girdisini önce materyalize eder.
- **VarLength:** DFS, kesin `*1..N`, N ≤ 10. "Visited" yalnız o anki yolun kenar kümesidir (openCypher trail). Bellek O(N); `max_rows`, 1M expansion ve 5 s bütçesi. Aşım `LimitExceeded`'dır, kesilmiş sonuç değil.
- **Salt-okunur:** Plan ağacında yazma işleci varsa `ReadOnlyViolation`, işlem açılmaz. İkinci savunma `PRAGMA query_only=ON`.
- **Semantik:** Bir `MATCH`'te kenar benzersiz, düğüm tekrarlanabilir. Null üç değerli: `WHERE` yalnız `true` geçirir, tip uyuşmazlığı null verir, `SET x=null` kaldırır, aggregate'ler null'ı atlar (`count(*)` hariç), `sum` boşta 0. `MERGE` deseninde null hatadır.
- **Yakınsayan `MERGE`:** Oluşturma kimliği `derive_id(etiketler + özellikler)` (kenarda src, tip, dst, özellikler).

## Op log

- **İşlemler (7):** `node_put`, `node_del`, `label_add`, `label_del`, `edge_put(id, src, type, dst)`, `edge_del`, `prop_set(owner, key, value)`; null kaldırmadır. Kimlik UUIDv7; doğal anahtarlı varlıkta türetilmiş UUIDv8.
- **HLC:** 48 bit ms + 16 bit sayaç, çerçeve başına. `now+24h` ötesi kendi saatine alınmaz, sıralama için kabul edilir.
- **Register kuralı:** Çerçevenin kimliği `dot=(dev, seq)`, nedensel bağlamı `C`. Yazma, `v.seq ≤ C[v.dev]` olan sürümleri siler, kendini ekler. Kalanlar head'dir; sonuç uygulama sırasından bağımsızdır. Özellik okuması `max(hlc, dev)` head'idir. Etiket ve kenarda eşzamanlı ekle/sil add-wins'tir.
- **Teslim:** Çerçeve ancak `applied[dev]=seq-1` ve her `dep` karşılanmışsa uygulanır. Tekrarı `peers` eler.
- **Silme:** Silme, sahibin her yazmasını görmüşse ya da her yazma onu görmüşse etkilidir; eşzamanlı tek yazma onu sonsuza dek etkisiz kılar (düzenleme kazanır). Kenarda da aynı kural geçerlidir; kenar özelliği kenarın yazmasıdır. Düğüme yazma, satır yoksa düğümü var eder: görmediği düğüme yazan uzak çerçeve her sırada aynı sonucu verir. Yerel API olmayan düğüme yazmayı reddeder.
- **Yeniden kurma:** Yerinde uygulama, sonucu op kümesinin saf fonksiyonuyla (`History`) aynı olduğu kanıtlanabilen çerçevede kullanılır. Kuşkuda çerçeve yalnız kaydedilir, ingest sonunda tüm graph `ops`'tan kurulur (`ponytail:` O(ops); nadir yol). Bekleyen kurma `meta.rebuild_pending` ile kalıcıdır, çökmede sonraki ingest tamamlar. Kararlar O(işlem × cihaz) çalışır.
- **Kenar ve ölü düğüm:** `edge_put` düğümü canlandırmaz; ucu ölüyse kenar state'e girmez, `ops`'ta kalır. Sarkan kenar oluşmaz. Ucunun hiçbir yazmasını görmemiş veya etkili silmesini görmemiş kenar işlemi düşer.
- **Kenar kimliği:** Kimlik `(src, type, dst)` değişemez. Farklı kimliği görmüş put aday olamaz; adaylardan en küçüğü kazanır, kaybeden işlem her cihazda etkisizdir ve `bad_frames`'e `edge_identity_conflict` olarak girer. Çerçeve durmaz: durmak cihazları ayrıştırırdı.
- **`body` çakışması:** `ingest_logs`, farklı değerli çok head'li `(owner, key)` listesini `MergeReport.conflicts`'te verir, `versions()` değerleri sunar. G4 kopya düğümü `derive_id("conflict", note, kaybeden dot)` ile kurar. Eşzamanlı `deleted_at` ve `body` görülürse G4 `deleted_at`'ı temizler.
- **Dosya (ikili):** Başlık `"NGOL" ver dev[16]`; çerçeve `u32 len | SHA-256 ilk 8 bayt | payload`; payload `seq, hlc, deps, ops`. Ad `<32 küçük hex>.oplog`. v2 (L4b): başlık sürümü 2, her kayıt bir parça `tür=1 | seq u64 | index u32 | final u8 | baytlar`; mantıksal çerçeve ardışık parçalardan birleşir (SPEC-graph "Parçalı çerçeve ve çok çerçeveli işlem"). Sınırlar (config, kayıt başına): kayıt ≤ 64 MiB ve ≤ 65.536 işlem (parçalı çerçevede parça başına), anahtar/etiket/tip ≤ 256 bayt geçerli UTF-8, dep ≤ 1024, tanınan cihaz ≤ 256. `.gitattributes`: `*.oplog -text -merge`.
- **Hata:** Ingest yalnız yerel depolama ve bellek hatasında hata döner; çerçeve içeriği döndüremez. Dosya sonundaki eksik çerçeve ve kısa başlık `partial_tails`'e girer. Tam uzunlukta bozuk çerçeve bildirilir, o cihazın günlüğü orada durur (`stalled`). Başlıktaki `dev` dosya adıyla uyuşmalıdır. Aynı `(dev, seq)` farklı özetle gelirse `bad_frames`'e girer. `heads` cihaz başına `log_seq` ve `applied_seq` verir: `log_seq < applied_seq` geri alınmış cihazdır. `stalled.reason`: `bad_frame`, `missing_dependency`, `peer_limit`, `ingest_budget`.
- **Uzak op:** `decode_frame(span) → Frame` doğrular, yerel yazmayla aynı `apply_op` sabit prepared statement'larla çalışır. Kendi günlüğü de ingest edilir. **Sıra:** Sync önce kendi günlüğünü ingest eder, sonra export: geri yüklenen `graph.db` böyle yetişir. Sıra bozulursa günlük kenara alınır, kaybolmaz; ama yeni yazma aynı `seq`'i farklı içerikle üretip diğer cihazlarda `seq_digest_mismatch` doğurabilir.
- **Dosya güvenliği:** Depo içeriği güvenilmezdir. Geçici dosya tahmin edilemez adla ve özel oluşturulur (`O_CREAT|O_EXCL|O_NOFOLLOW`, Windows `CREATE_NEW`), `.oplog` bağ izlemeden okunup eklenir, normal olmayan dosya reddedilir, sembolik bağ olan günlük dizini kullanılmaz.
- **Export:** Dosyanın her çerçevesi `ops` ile karşılaştırılır. `ops`'un öneki olmayan dosya `<ad>.aside-<rastgele>` olarak kenara alınır, `ExportReport`'ta bildirilir, silinmez; yerine `ops`'tan üretilir. Yarım kuyruk önektir, kenara alınmaz.
- **Bütçe:** Ingest dosyayı akışla okur, bellekte en fazla bir çerçeve tutar. Bir ingest ≤ 8 GiB yeni çerçeve okur (sırayla tekrar sayılmaz, `ops` ile sınırlıdır), aşınca `ingest_budget` ile durur ve sonraki ingest sürer; günlük bundan büyürse ilerleyemez (sıkıştırma v1'de yok). Yazma kilidi 5.000 çerçevede, 64 MiB'de veya 1 sn'de bırakılır. Bellek biterse API `graph.oom` döner. `ops` taraması ingest başına bir kez ve kilit dışındadır; `Graph::open` ve yerel yazma taramaz.
- **Yeni cihaz:** İkili okuma, prepared satır yazımı, `NORMAL`, batch commit. Perf testi kapıdır (≤ 10 sn).

## Tehdit modeli

Varsayım: özel depo, imza yok. Git'e yazabilen her cihaz güvenilir kabul edilir; bu kabul edilmiş bir sınırdır.

- **Engellenen:** Depo içeriğiyle yerel dosyanın üzerine yazma (sembolik bağ), süreci düşürme, belleği veya süreyi sınırsız tüketme, ingest'i kalıcı durdurma, yerel yazmayı ve export'u rehin alma, cihazları sessizce ayrıştırma.
- **Ele geçirilmiş cihazın hâlâ yapabildikleri:**
  - Geçerli işlem yazmak: her düğümü silmek veya değiştirmek, başka cihaz kimliğiyle çerçeve üretmek. Başka cihazın günlüğünü ilk gören kazanır; farklı içerikli tekrar `seq_digest_mismatch` olur.
  - 256 sahte cihazla sınırı doldurmak: yerel yazma sürer. Kabul nedensel ihtiyaçla olduğundan (L4c) kabul edilmiş bir cihazın bağımlı olduğu dürüst cihaz sahtelerden önce gelir; kimsenin bağımlı olmadığı yeni dürüst cihaz, yerler önceki ingest'lerde dolduysa `peer_limit` ile durur. Kabul sırası günlüklerin özgünlüğüne dayanır; günlükler imzasız olduğundan depoya yazan, kabul edilmiş bir cihazın sıradaki çerçevesini taklit edip sahte cihazları gerekli kılabilir (L4d). Çözüm depoyu ve cihaz kimliklerini yenilemek.
  - Yeniden kurmayı her ingest'te tetiklemek: O(ops) maliyet, kilit bu sürece tutulur. Tek kurma süresi perf testinde ölçülür.
  - Diski doldurmak: `ops` ve git geçmişi büyür; sıkıştırma v1'de yok.
  - Depoya ana kolu bozan `graph` blob'u koymak: uzak düzeltilene dek senkron durur. Kabul edildi; yerel yazma ve export sürer.
  - Birleşim durumunu doldurmak: 8.388.608 düğüm + kenar sahibini (`max_merge_owners`) aşan günlük `merge_limit` ile durur, bellek bitmez; sonraki dürüst çerçeveler de bekler. `peer_limit` ile aynı sınıf, kabul edildi.
  - Seçici teslim: `peer_limit` veya bozuk dosya farklı cihazlarda farklı görülürse o cihazlar raporlu olarak ayrışır.
  - Sırları okumak: depoya erişimi olan herkes tüm notları okur, şifreleme yok.
- **Görünürlük:** Yukarıdakilerin hepsi `MergeReport` ve `ExportReport` üzerinden izlenir. Uygulama bunları kullanıcıya göstermelidir (G5).

## Dosyalar (test hariç ≈ 4.100 satır)

G1: `error/id/value.hpp`, `id.cpp` (UUID + SHA-256), `graph.{hpp,cpp}`, `store.{hpp,cpp}`, `op.{hpp,cpp}`. G2: `merge.{hpp,cpp}`, `oplog.{hpp,cpp}`, `secure_file.{hpp,cpp}`. `store.hpp` SQLite türü taşıdığından public kökün dışında: `internal/graph/`. G3: `cypher/{lexer,ast,parser,planner,operators,expr}`.

## İlk düşen testler

- **G1:** `refuses_newer_user_version`, `bytes_roundtrip_preserves_nul_crlf_bom_invalid_utf8_10mib`, `rollback_leaves_neither_state_nor_op`, `neighbors_keep_parallel_edges_by_direction_and_type`, `new_label_leaves_sqlite_master_unchanged`, `query_plan_same_with_and_without_stat1`, `two_hop_ratio_5k_50k`.
- **G2:** `concurrent_set_keeps_both_heads_in_any_order`, `delete_vs_edit_converges_in_any_order`, `partial_tail_applies_on_next_ingest`, `crash_between_commit_and_export_loses_nothing`, `fresh_device_5000_notes_under_10s`.
- **G3:** `load_csv_rejected_with_position`, `read_only_rejects_every_write_clause_and_file_bytes_unchanged`, `pattern_edge_used_once_per_match`, `null_three_valued_where`, `merge_on_two_devices_yields_one_node`.

## Riskler

1. **50 ms, tüm notlar:** 5.000 başlangıç × 4 çıkış ≈ 100k satır. G1 kapısı; gerekirse sorgu içi memo veya surrogate.
2. **Nedensel durma:** Bozuk çerçeve bir cihazın ilerisini bekletir. `MergeReport.stalled` görünür; kaynak cihaz `ops`'tan yeniden üretir.
3. **Silme ve canlanma:** Yakınsamanın en ince yeri. Sonuç op kümesinin saf fonksiyonu; 4 cihazlı rastgele sıra, kanonik dump eşitliği.
4. **Günlük büyümesi:** Her kayıtta tam `body` hem `ops`'a hem git'e gider. G4'te otomatik kayıtlar birleştirilir, G5'te boyut ölçülür.
5. **Fuzz altyapısı:** Yalnız clang'lı `NOTEAPP_BUILD_FUZZ`; `decode_frame` ve `query` için iki hedef, 2M koşu ayrı iş.
6. **G4:** Aynı adlı klasör kimliği `derive_id(parent, name)`. Eşzamanlı taşıma iki `IN_FOLDER` ve `CHILD_OF` döngüsü üretebilir; deterministik okuma kuralı gerekir. Tuş başına liste belleğe alınmış sonucu süzer.
