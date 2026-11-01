# G4 test eşlemesi: düz dosyadan graph'a

Yalnız emekli ya da konusu değişen testler. Davranışı aynı kalan testler baytları `NoteStore` üzerinden yazacak şekilde taşındı (`tests/support/note_seed.hpp`); burada listelenmez. Yeni testler: `tests/core/note_graph_test.cpp`, `tests/perf/note_store_perf_test.cpp`.

| # | Eski test | Dosya | Yeni test ya da neden |
|---|---|---|---|
| 1 | `read, write and create through a directory link…` | core/note_store_test | Emekli: not yolu dosya sistemine inmez. Eşdeğer: `note export --all never overwrites and never goes through a link` |
| 2 | `8.3 short names of hidden entries…` | core/note_store_test | Emekli: kısa ad yok. Gizli ad `NotePath`'te reddedilir (`note_path_test`); dışa aktarım dışlayıcı oluşturur |
| 3 | `absolute_path applies the same checks as read…` | core/note_store_test | API kalktı. `edit_in_temp_file: private file, saved through the conflict check, removed after` |
| 4 | `a link to a folder inside the root is refused too` | core/note_store_test | Emekli: bağ kavramı yok. Eski ağaçta `link in the old tree is never followed and is reported` |
| 5 | `list skips directory links…` | core/note_store_test | Aynı neden, aynı karşılık (4) |
| 6 | `list skips files whose names fail NotePath validation` | core/note_store_test | `a hostile name from another device never becomes a path and is not listed` |
| 7 | `failed write leaves the original intact and no .tmp behind` | core/note_store_test | Emekli: geçici dosya yok. Geri alma: `rollback_leaves_neither_state_nor_op` (graph), `session: a failed write keeps the buffer…` |
| 8 | `successful write leaves no .tmp behind` | core/note_store_test | Emekli: geçici dosya yok |
| 9 | `write keeps the file's attributes` (Windows) | core/note_store_test | Emekli: not dosyası yok |
| 10 | `write keeps the file's permission bits` | core/note_store_test | Emekli: not dosyası yok. Geçici dosya izni `edit_in_temp_file` testinde |
| 11 | `mtimes are sub-second precise…` | core/note_store_test | Emekli: karşılaştırma mtime'la değil. `list(), read() and write() report the same revision` |
| 12 | `write refuses a same-size external edit made within the same second` | core/note_store_test | `a same-size edit within the same millisecond window still trips the revision` |
| 13 | `move and remove reject paths through a directory link` | core/note_store_ops_test | Emekli: (1) |
| 14 | `remove_folder never deletes through a directory link` | core/note_store_ops_test | Emekli: silme düğüm üzerinedir, dosya sistemine inmez |
| 15 | `rename_folder refuses to move a link…` | core/note_store_ops_test | Emekli: (14) |
| 16 | `create_folder refuses a link…` | core/note_store_ops_test | Emekli: (14). Yeni: `a note can't get a path below another note` |
| 17 | `create_folder writes a hidden .keep…` | core/note_store_ops_test | `create_folder makes empty folders`, `empty folders are nodes and travel to another device` |
| 18 | `remove_folder without force also refuses unknown hidden files` | core/note_store_ops_test | Emekli: klasör düğümünde gizli dosya olamaz |
| 19 | `every trash operation refuses a .trash folder that is a link` | core/trash_test | Emekli: `.trash` yok, `deleted_at` var |
| 20 | `trash operations refuse links inside the trash` | core/trash_test | Emekli: (19) |
| 21 | `trash and restore refuse a live path through a link` | core/trash_test | Emekli: (19) |
| 22 | `trash operations refuse a file link in the trash` | core/trash_test | Emekli: (19) |
| 23 | `a skipped file name with control bytes reaches the log escaped` | core/trash_test | `a skipped hostile name with control bytes reaches the log escaped` |
| 24 | `export refuses a note that now sits behind a folder link` | gui/pdf_test | Emekli: PDF baytı depodan okunur, yol dosya sistemine inmez |
| 25 | `a folder swapped for a link during the path dialog…` | gui/pdf_test | Emekli: (24) |
| 26 | `the delete confirmation counts hidden files…` | gui/folders_test | Emekli: klasörde gizli dosya yok |
| 27 | `a folder that only looks empty never loses hidden files…` | gui/folders_test | Emekli: (26) |
| 28 | `changing notes_dir re-points the note list` | gui/settings_test | Değişti (karar): `changing notes_dir flushes first and keeps the notes`. Notlar `graph.db`'dedir |
| 29 | `session: a failed write…`, `session: on exit, when the store refuses…` | tui/editor_session_test | Taşındı: bağ yerine başka bağlantının tuttuğu yazma kilidi; Windows'ta da koşar |
| 30 | `export stops on a read error and writes no output` | cli/cli_export_test | Emekli: bayt depodan gelir, kilitlenecek ya da izni kapatılacak not dosyası yok. Depo hatası `session: a failed write…` ile sınanır |

**Sayı:** 23 emekli (1, 2, 4, 5, 7–11, 13–16, 18–22, 24–27, 30), 7 değişen (3, 6, 12, 17, 23, 28, 29).

**Bağ kaçışı artık nerede:** `note export --all` (bağdan geçmez, üzerine yazmaz, uzak adla kaçmaz), geçici dosya (`create_private_temp_file`, 0600, dışlayıcı; editörün bıraktığı bağ okunmaz), geçiş (`walk_legacy` bağ izlemez).
