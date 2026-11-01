# Plan: Gömülü graph veritabanı (SQLite üzerinde)

> Durum: **ONAYLANDI** (2026-11-01) · Spec: [SPEC-graph.md](../SPEC-graph.md) · Dal: `feat/imgui` üzerinden çalışma ağaçları
> Kritik iş: önce düşen test, yeni kodda ASan/UBSan. G1–G3 T58 ve T55 ile paralel yürür; G4'ten itibaren onlardan sonra.

## Görevler

### G1: 🔴 Graph çekirdeği
- **Scope:** `src/graph/**` (`cypher` hariç), `tests/graph/**`, `cmake/Dependencies.cmake`, `CMakeLists.txt`, `tests/CMakeLists.txt`
- **Kabul:** SPEC-graph kabul 1–3, 9 · SQLite amalgamation pinli · kalıcı kimlik · şemasız model · iki yönlü komşuluk · işlem · şema sürümü · tek yazma yolu `ops` ile aynı işlemde · `src/graph/cypher` iskeleti. Tasarım: [graph-design.md](graph-design.md).

### G2: 🔴 Birleşim ve işlem günlüğü
- **Depends:** G1 · **Scope:** `src/graph/merge.*`, `src/graph/oplog.*`, `src/graph/graph.*`, `tests/graph/**`
- **Kabul:** SPEC-graph kabul 4 ve 5 (oplog) · aynı işlemde `ops` · HLC · idempotent uygulama · yarım satır · çökme noktaları.

### G3: Cypher alt kümesi
- **Depends:** G1 · **Scope:** `src/graph/cypher/**`, `tests/graph/cypher/**`, `src/cli/**`, `tests/cli/**`
- **Kabul:** SPEC-graph kabul 5 (Cypher) ve 6, ilke 3 · plan graph işleçleridir, dev SQL değildir · `note query`, `--json`, `--write`, sınırlar.

### G3b: Fuzz
- **Depends:** G2, G3 · **Scope:** `tests/graph/fuzz/**`, `CMakeLists.txt`
- **Kabul:** clang ile `NOTEAPP_BUILD_FUZZ`; `decode_frame` ve `query` hedefleri, ASan/UBSan, en az 2M girdi, crash ve hang yok.

### G4: 🔴 `NoteStore` graph üzerine
- **Depends:** G2, G3, T55a, T55b, T58 · **Scope:** `src/core/**`, `src/search/**`, `tests/core/**`, `tests/search/**`, `SPEC-core.md`
- **Kabul:** SPEC-graph kabul 7 · uygulama şeması · `.trash` yerine `deleted_at` · `created_at` ve `origin` gerçek değer · `LINKS_TO` üretimi.

### G5: 🔴 Git senkronu oplog üzerinde
- **Depends:** G4 · **Scope:** `src/sync/**`, `tests/sync/**`, `SPEC-sync.md`
- **Kabul:** SPEC-sync senaryoları yeni modelde karşılanır; emekli olanlar gerekçesiyle listelenir · güvenilmez uzak günlük · token sızıntı testi · yeni cihaz kurulumu süresi.

### G6: 🔴 Geçiş ve dışa aktarma
- **Depends:** G5 · **Scope:** `src/cli/**`, `tests/cli/**`, `README.md`
- **Kabul:** SPEC-graph kabul 10 · `note graph migrate/export/import` · `note export --all`.

### G7: Son doğrulama
- **Depends:** G6 · **Scope:** `SPEC*.md`, `README.md`, `tasks/todo.md`, `packaging/**`, `scripts/**`
- **Kabul:** SPEC-graph kabul 8 ve 9 · temiz klonda `check-fedora`, AppImage smoke, Windows çapraz derleme · `cpp-reviewer` ve `security-reviewer` bulguları kapalı.

### T59: LSP istemcisi
- **Depends:** G7 · Varsayılan kapalı, ayrı plan.

## Tahmin

Yaklaşık 1 hafta ajan çalışması. En belirsiz parçalar G2 ve G3. G1 sonunda kabul 1 tutmazsa durulup raporlanır.

## Elle (kullanıcı)

- İki gerçek cihaz arasında senkron: Fedora ve Windows.
- Gerçek notlarla geçiş: önce yedek, sonra `note export --all` karşılaştırması.
