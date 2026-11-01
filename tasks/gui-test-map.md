# GUI test haritası (Qt → SDL3/ImGui)

`tests/gui/*.cpp` içindeki 108 Qt `TEST_CASE`'in taşınma kaydı. Her görev taşıdığı satırı günceller.
Durum: `pending` (henüz taşınmadı), `ported` (son sütun yeni test adı), `retired` (son sütun `tasks/plan-imgui.md` "Davranış değişiklikleri" madde numarası + gerekçe).
Bir test yalnız numaralı davranış değişikliği yüzünden emekli olabilir.

| Eski test | Dosya | Durum | Yeni test / emeklilik gerekçesi |
|---|---|---|---|
| content mode lists matches with their line counts, best first | content_search_test.cpp | ported | content mode lists matches with their line counts, best first |
| toggle button, API and Tab from the search box switch the mode | content_search_test.cpp | ported | toggle button, API and Tab from the search box switch the mode |
| empty query in content mode lists every note without counts | content_search_test.cpp | ported | empty query in content mode lists every note without counts |
| selecting a result highlights every occurrence at correct UTF-16 positions | content_search_test.cpp | ported | selecting a result selects the first occurrence at correct byte offsets; Enter from the search box lands on the first match in the editor (plan davranış 3: tek seçim, UTF-8 bayt ofseti) |
| folding matches Turkish letters and phrases highlight as one range | content_search_test.cpp | ported | folding matches Turkish letters and phrases select as one range (plan davranış 3) |
| highlights clear on edit, query change and mode change | content_search_test.cpp | ported | selection clears on edit, query change and mode change (plan davranış 3) |
| highlight stays stable across rescans and uses a palette colour | content_search_test.cpp | ported | selection is not reissued across rescans (plan davranış 3: boyama ve palet rengi kalktı) |
| content index follows external changes after a rescan | content_search_test.cpp | ported | content index follows external changes after a rescan |
| content results respect the selected folder | content_search_test.cpp | ported | content results respect the selected folder; clicking a folder in the tree limits the content results |
| deleting a non-empty folder never happens without confirmation | folders_test.cpp | ported | deleting a non-empty folder never happens without confirmation |
| the delete confirmation counts hidden files the note list does not show | folders_test.cpp | ported | the delete confirmation counts hidden files the note list does not show |
| a folder that only looks empty never loses hidden files without a count | folders_test.cpp | ported | a folder that only looks empty never loses hidden files without a count |
| deleting an empty folder asks a simple confirmation | folders_test.cpp | ported | deleting an empty folder asks a simple confirmation |
| deleting an unknown folder reports the error and touches nothing | folders_test.cpp | ported | deleting an unknown folder reports the error and touches nothing |
| new folder is created under root or the clicked folder, Turkish names included | folders_test.cpp | ported | new folder is created under root or the clicked folder, Turkish names included |
| new folder: cancel and collision change nothing and report | folders_test.cpp | ported | new folder: cancel and collision change nothing and report |
| rename folder moves notes, offers the old leaf name and rejects collisions | folders_test.cpp | ported | rename folder moves notes, offers the old leaf name and rejects collisions |
| renaming the folder of the shown dirty note flushes first and keeps state consistent | folders_test.cpp | ported | renaming the folder of the shown dirty note flushes first and keeps state consistent |
| deleting the folder of the shown dirty note flushes first, then clears the editor | folders_test.cpp | ported | deleting the folder of the shown dirty note flushes first, then clears the editor |
| a failing flush aborts folder rename and delete | folders_test.cpp | ported | a failing flush aborts folder rename and delete |
| delete confirmation box defaults to Cancel | folders_test.cpp | ported | delete confirmation box defaults to Cancel |
| delete note asks confirmation, deletes only when confirmed, selects the neighbour | folders_test.cpp | ported | delete note asks confirmation, deletes only when confirmed, selects the neighbour |
| delete shortcut and context path go through the confirmation decider | folders_test.cpp | ported | delete shortcut and context path go through the confirmation decider |
| deleting the shown dirty note flushes first and keeps it on a failing flush | folders_test.cpp | ported | deleting the shown dirty note flushes first and keeps it on a failing flush |
| move note changes only the folder (name and type are locked) | folders_test.cpp | ported | move note changes only the folder (name and type are locked) |
| Ctrl+M shortcut opens the move dialog | folders_test.cpp | ported | Ctrl+M shortcut opens the move dialog |
| folder-only save dialog locks name and type | folders_test.cpp | ported | folder-only save dialog locks name and type; Ctrl+M dialog ignores clicks and typing on the locked name and type |
| window lists notes, selects the newest and shows its content | main_window_test.cpp | ported | window lists notes, selects the newest and shows its content |
| typing in the search box filters the list and keeps the selection valid | main_window_test.cpp | ported | typing in the search box filters the list and keeps the selection valid |
| Up/Down in the search box move the list selection and update the view | main_window_test.cpp | ported | Up/Down in the search box move the list selection and update the view |
| Enter in the search box focuses the editor | main_window_test.cpp | ported | Enter in the search box focuses the editor |
| focus_search selects the existing query; settings tab switches | main_window_test.cpp | ported | focus_search selects the existing query; settings tab switches |
| binary note shows the binary placeholder instead of its bytes | main_window_test.cpp | ported | binary note shows the binary placeholder instead of its bytes |
| folder tree limits the list to the subtree | main_window_test.cpp | ported | folder tree limits the list to the subtree; the folder tree selects by click and Enter under the all notes root |
| rescan picks up new notes, keeps selection and editor, resets only on change | main_window_test.cpp | ported | rescan picks up new notes, keeps selection and editor, resets only on change |
| selected note is restored from gui-state.ini after close | main_window_test.cpp | ported | selected note is restored from gui-state.toml after close (plan davranış 7) |
| config warnings are shown in the status bar at startup | main_window_test.cpp | ported | config warnings are shown in the status bar at startup |
| empty query lists all notes newest first | note_list_model_test.cpp | ported | empty query lists all notes newest first |
| empty query orders by mtime even when path lengths differ | note_list_model_test.cpp | ported | empty query orders by mtime even when path lengths differ |
| query ranks by fuzzy score and exposes highlight units | note_list_model_test.cpp | ported | query ranks by fuzzy score and exposes highlight offsets |
| highlight positions are UTF-16 units, not UTF-8 bytes (Turkish names) | note_list_model_test.cpp | ported | highlight offsets point at code point starts in Turkish names |
| highlight covers both UTF-16 units of a non-BMP character | note_list_model_test.cpp | ported | highlight offset skips a whole non-BMP character |
| folder filter limits to the subtree and not to sibling prefixes | note_list_model_test.cpp | ported | folder filter limits to the subtree and not to sibling prefixes |
| folder filter and query combine | note_list_model_test.cpp | ported | folder filter and query combine |
| index_of finds rows by path and survives refiltering | note_list_model_test.cpp | ported | index_of finds rows by path and survives refiltering |
| model resets only when the listing or filter actually changes | note_list_model_test.cpp | ported | model resets only when the listing or filter actually changes |
| same_listing compares path, mtime and size | note_list_model_test.cpp | ported | same_listing compares path, mtime and size |
| export_pdf renders a markdown note with Turkish text | pdf_smoke_test.cpp | ported | pdf: Turkish markdown reads back through pdftotext |
| export_pdf renders a plain text note with Turkish text | pdf_smoke_test.cpp | ported | pdf: Turkish plain text reads back through pdftotext |
| export_pdf reports a missing input and writes nothing | pdf_smoke_test.cpp | retired | Davranış 5: export_pdf artık yol değil bayt alır; karşılık cli: export with no matching note exits 1 |
| strip_images replaces every image fragment with its alt text | pdf_smoke_test.cpp | ported | pdf: image alt text replaces every image, with a placeholder when empty |
| export_pdf never embeds or requests images referenced by a note | pdf_smoke_test.cpp | ported | pdf: image references are never embedded and their targets never reach the PDF |
| notes-gui --export-pdf exports a Turkish-named markdown note | pdf_test.cpp | ported | cli: export writes a PDF into a subfolder via -o |
| notes-gui --export-pdf exports a Turkish-named plain text note | pdf_test.cpp | ported | cli: export of a txt note goes end to end through the plain text path; cli: export defaults to <stem>.pdf in the current directory; pdf: Turkish plain text reads back through pdftotext |
| notes-gui --export-pdf fails with exit 1 and a stderr message for a missing input | pdf_test.cpp | retired | Davranış 5: --export-pdf kalktı; karşılık cli: export with no matching note exits 1 |
| notes-gui --export-pdf fails with exit 1 for an unwritable output and leaves no partial file | pdf_test.cpp | retired | Davranış 5: --export-pdf kalktı; karşılık cli: unwritable output exits 3 and leaves no file, pdf: unwritable output reports an error and leaves no file behind |
| notes-gui --export-pdf with a wrong argument count exits 1 with the usage text | pdf_test.cpp | retired | Davranış 5: --export-pdf kalktı; karşılık cli: export without --pdf is a usage error (kullanım metni dahil) |
| export flushes a dirty buffer first, then writes the pdf and reports it | pdf_test.cpp | ported | export flushes a dirty buffer first, then writes the pdf and reports it |
| cancelled export path writes no pdf | pdf_test.cpp | ported | cancelled export path writes no pdf |
| export failure is shown in the status bar | pdf_test.cpp | ported | export failure is shown in the status bar |
| export does nothing for a binary note | pdf_test.cpp | ported | export does nothing for a binary note |
| export does nothing when no note is shown | pdf_test.cpp | ported | export does nothing when no note is shown |
| export refuses a note that now sits behind a folder link | pdf_test.cpp | ported | export refuses a note that now sits behind a folder link |
| editing an existing note is autosaved once after autosave_ms | save_test.cpp | ported | editing an existing note is autosaved once after autosave_ms |
| rescan reloads a clean open note silently when it changed on disk | save_test.cpp | ported | rescan reloads a clean open note silently when it changed on disk |
| rescan keeps a dirty buffer when the note changed on disk | save_test.cpp | ported | rescan keeps a dirty buffer when the note changed on disk |
| conflict: save mine as copy keeps both versions and shows the copy | save_test.cpp | ported | conflict: save mine as copy keeps both versions and shows the copy |
| conflict: a second copy gets the collision suffix | save_test.cpp | ported | conflict: a second copy gets the collision suffix |
| conflict: overwrite writes the buffer over the external change | save_test.cpp | ported | conflict: overwrite writes the buffer over the external change |
| conflict: reload drops the buffer and shows the disk version | save_test.cpp | ported | conflict: reload drops the buffer and shows the disk version |
| conflict: cancel keeps the buffer dirty and the disk untouched | save_test.cpp | ported | conflict: cancel keeps the buffer dirty and the disk untouched |
| the default conflict button is 'save mine as copy', Escape cancels | save_test.cpp | ported | the default conflict button is 'save mine as copy', Escape cancels |
| a vanished note is saved through the new-note dialog, nothing is lost | save_test.cpp | ported | a vanished note is saved through the new-note dialog, nothing is lost |
| new note: Ctrl+N opens an empty untitled buffer, first save asks the dialog | save_test.cpp | ported | new note: Ctrl+N opens an empty untitled buffer, first save asks the dialog |
| save dialog is prefilled with the detect suggestion and the tree folder | save_test.cpp | ported | save dialog is prefilled with the detect suggestion and the tree folder |
| chosen type becomes the extension | save_test.cpp | ported | chosen type becomes the extension |
| cancelling the save dialog keeps the new note and its buffer | save_test.cpp | ported | cancelling the save dialog keeps the new note and its buffer |
| a name collision in the dialog creates a suffixed note, never overwrites | save_test.cpp | ported | a name collision in the dialog creates a suffixed note, never overwrites |
| an invalid dialog result keeps the buffer and reports the error | save_test.cpp | ported | an invalid dialog result keeps the buffer and reports the error |
| SaveDialog shows prefilled fields and rejects invalid input inline | save_test.cpp | ported | SaveDialog shows prefilled fields and rejects invalid input inline; the save dialog draws the detect suggestion |
| switching notes saves the dirty note first | save_test.cpp | ported | switching notes saves the dirty note first |
| switching away from a new note asks the dialog; cancel stays on the note | save_test.cpp | ported | switching away from a new note asks the dialog; cancel stays on the note |
| a failed save while switching keeps the buffer and the selection | save_test.cpp | ported | a failed save while switching keeps the buffer and the selection |
| close with an unsaved new note: Cancel keeps the window and the buffer | save_test.cpp | ported | close with an unsaved new note: Cancel keeps the window and the buffer |
| close with an unsaved new note: Discard closes, Save creates the note | save_test.cpp | ported | close with an unsaved new note: Discard closes, Save creates the note |
| close with an empty new note does not ask | save_test.cpp | ported | close with an empty new note does not ask |
| close autosaves a dirty note; a conflict cancel aborts the close | save_test.cpp | ported | close autosaves a dirty note; a conflict cancel aborts the close |
| type combobox moves the note to the new extension | save_test.cpp | ported | type combobox moves the note to the new extension |
| type combobox refuses to overwrite an existing target | save_test.cpp | ported | type combobox refuses to overwrite an existing target |
| Ctrl+Shift+S moves with the dialog prefilled from the current note | save_test.cpp | ported | Ctrl+Shift+S moves with the dialog prefilled from the current note |
| Enter in the search box with no match starts a new note from the query | save_test.cpp | ported | Enter in the search box with no match starts a new note from the query |
| binary and non-UTF-8 notes stay read-only and are never rewritten | save_test.cpp | ported | binary and non-UTF-8 notes stay read-only and are never rewritten |
| CRLF line endings survive an edit | save_test.cpp | ported | CRLF line endings survive an edit |
| a large CRLF note loads with its content intact and keeps CRLF on save | save_test.cpp | ported | a large CRLF note loads with its content intact and keeps CRLF on save |
| saved value is visible through load_config and the token never reaches config.toml | settings_test.cpp | ported | saved value is visible through load_config and the token never reaches config.toml |
| an invalid value is shown under its field and nothing is saved | settings_test.cpp | ported | an invalid value is shown under its field and nothing is saved |
| a broken config file is never overwritten | settings_test.cpp | ported | a broken config file is never overwritten |
| changing notes_dir re-points the note list | settings_test.cpp | ported | changing notes_dir re-points the note list |
| the notes_dir browse button goes through the decider | settings_test.cpp | ported | the notes_dir browse button goes through the decider |
| the token row is enabled only for HTTPS remotes | settings_test.cpp | ported | the token row is enabled only for HTTPS remotes; the drawn token field never shows the typed value |
| enabling sync against a local bare remote saves the config and starts syncing | settings_test.cpp | ported | enabling sync against a local bare remote saves the config and starts syncing |
| an origin mismatch asks before replacing and declining saves nothing | settings_test.cpp | ported | an origin mismatch asks before replacing and declining saves nothing |
| the test connection button enables and runs one sync, showing the outcome | settings_test.cpp | ported | the test connection button enables and runs one sync, showing the outcome |
| the status label shows off, running, last success and last error | settings_test.cpp | ported | the status label shows off, running, last success and last error |
| F5 asks for a sync now and Ctrl+comma opens the settings tab | settings_test.cpp | ported | F5 asks for a sync now and Ctrl+comma opens the settings tab |
| changing language only announces a restart | settings_test.cpp | ported | changing language only announces a restart |
| revert restores the saved values | settings_test.cpp | ported | revert restores the saved values |
| saving unrelated settings keeps the running sync, a branch change restarts it | settings_test.cpp | ported | saving unrelated settings keeps the running sync, a branch change restarts it |
