# G5 test eşlemesi: SPEC-sync senaryoları oplog üzerinde

Yeni testler `tests/sync/oplog_sync_test.cpp`: iki klon, her birinin kendi `graph.db`'si, gerçek günlük ve gerçek git (yerel bare uzak). Dosya düzeyi birleştirme testleri emekli; karşılığı not düzeyindedir.

## SPEC-sync kabul kriterleri

| Kabul | Senaryo | Test |
|---|---|---|
| 1 | Hızlı ileri, yerel kayıp yok | `acceptance 1: a note from A reaches B…` |
| 2 | Çakışmasız birleşme | `acceptance 2: changes to different notes…` |
| 3 | Aynı not iki tarafta | `acceptance 3: the same note changed on both sides…` |
| 4 | Silme/değiştirme | `acceptance 4: delete against edit keeps the edit…`, `a delete that saw the edit…` |
| 5 | Push yarışı | `acceptance 5: the remote advancing…`, `a push rejected twice is reported…` |
| 6 | Kilit | `acceptance 6: sync_now is sync.busy…`, `SyncLock: …`, `sync_now takes the stale age…` |
| 7 | Token sızıntısı | `acceptance 7: the token never reaches files, git objects, the log…` (notlar artık günlükte de taranır: `objects_contain`) |
| 8 | `enable` tablosu | `enable row 1–5`, `sync with a clean clone of the remote`, `enable with old files waiting for migration…` |
| 9 | AutoSync | `acceptance 9`, `stop()…`, `end to end: AutoSync runs the real note sync…` |

## Yeni gereksinimler

| Gereksinim | Test |
|---|---|
| Yalnız `graph/*.oplog`, `.gitattributes`, `.gitignore` commit'lenir; diğer dosyalara dokunulmaz | `only carriers are committed…` |
| Önce kendi günlüğü (seq çatallanmaz) | `own log first: a restored database…` |
| `graph.db` kaybı | `a lost graph.db comes back from the logs` |
| Uyuşmayan yerel günlük kenara | `a local log that does not match…` |
| Görünürlük: `stalled`, `bad_frames`, `partial_tails`, `heads`, `omitted` | `sync-status.toml surfaces…`, `a damaged remote log…` |
| `note_status`: new, modified, synced, conflict_copy, sync_off | `note status: …` |
| Uzak adın doğrulanması | `a hostile name from the remote…` |
| `core.symlinks=false`, uzak yol doğrulaması | `remote symlinks…`, `a repository opened for sync…`, `a clone of a remote path invalid…`, `two remote names…` |
| Günlük dizini bağ olamaz | `a log folder that is a link is not used` |
| Depodaki düz dosyanın git birleşmesi, geri alma | `old files changed on both sides…` |
| Eski dosyalar ve iki cihaz geçişi | `two-clone migration…`, `migration after the other device edited…` |
| Yeni cihaz ≤ 10 sn | `new device: clone and first sync…` (perf) |

## Emekli ya da taşınan testler

| Eski test | Dosya | Karşılık ya da neden |
|---|---|---|
| `acceptance 1…`, `deletions are committed…`, `a push the remote keeps rejecting…`, `CRLF and LF files round-trip…` | sync_basic | Yukarıdaki not düzeyi karşılıklar; `git bytes stay exact…` |
| `acceptance 2/3/4/5` | sync_conflict | Yukarıda |
| `add/add with different content…` | sync_conflict | `add/add on one path…`: iki not da kalır, ikincisi `-2` |
| `binary conflict…`, `CRLF content survives conflicts…` | sync_conflict | `acceptance 3` baytları CRLF, BOM, NUL ve geçersiz UTF-8 içerir |
| `several conflicts…`, `conflicts on Turkish file names…` | sync_conflict | `several conflicts in one sync…; Turkish names` |
| `a conflict copy never overwrites an existing file…` | sync_conflict | Kopya ayrı düğümdür; yol çakışması `-2` (`add/add`) |
| `a conflict copy's name fits 255 bytes…` | sync_conflict | Aynı adla, not düzeyinde |
| `unrelated histories…` | sync_conflict | Aynı adla |
| `an unfinished manual merge/rebase…` | sync_conflict | Aynı davranış; `old files changed on both sides…` kapsamında git birleşmesi korunur |
| `non-overlapping edits of the same note merge into one file` | sync_conflict | Geri geldi: `non-overlapping edits merge into one note on both devices…`, `same merge on both devices…`, `three devices edit different lines…`, `overlapping edits, binary and invalid UTF-8…`, `no common ancestor…` |
| `a failure while writing conflict copies undoes the merge…`, `a note the user edits while a merge fails…`, `a sync killed in the middle of a conflicted merge…` | sync_conflict | Emekli: not baytı merge'de yazılmaz. `abort_merge` `an interrupted merge with conflicts…` (recovery) ve `old files changed on both sides…` ile sınanır |
| 9 test | note_status | `note status: …`, `is_conflict_copy_name…`. Git status geçişi yok, `.gitignore`'a takılan not kavramı yok, index dokunulmaz |
| 7 test | trash_sync | `trash, restore, purge and empty_trash travel…`, `acceptance 4`, `the same version trashed…` |
| `acceptance 8: A enables…` | enable | `sync with a clean clone of the remote…` |

**Sayı:** 39 eski test kalktı (sync_conflict 18, note_status 9, trash_sync 7, sync_basic 4, enable 1). 35'inin not düzeyi karşılığı var; 4'ü karşılıksız emekli: merge hatası üçlüsü, `ignored notes are sync_off…` (graph'ta `.gitignore`'a takılan not yok).

## Ölçüm

Yeni cihaz, 5.000 not × ~5 KB, gerçek git: 1,94 sn (sınır 10 sn), uzak 9,9 MiB. 200 düzenleme, 20 senkron, 2 KiB not: günlük 443 KiB, uzak git 102 KiB. Durum 5.050 notta 0,45 ms.
