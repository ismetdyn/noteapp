noteapp_add_test(gui
    pdf_smoke_test.cpp
    pdf_test.cpp
    note_list_model_test.cpp
    main_window_test.cpp
    save_test.cpp
    folders_test.cpp
    content_search_test.cpp
    settings_test.cpp)
# Ekransız ortamlarda (CI, Fedora container) da koşabilmek için pencere sistemi istemeyen platform eklentisi.
set_property(TEST gui APPEND PROPERTY ENVIRONMENT QT_QPA_PLATFORM=offscreen)
# T33: başsız mod testleri gerçek notes-gui ikilisini alt süreçte çalıştırır.
add_dependencies(gui_tests notes-gui)
# T32: ayarlar testleri gerçek `note` ikilisiyle arka plan senkronunu uçtan uca koşar.
add_dependencies(gui_tests note)
target_compile_definitions(gui_tests PRIVATE NOTEAPP_NOTE_EXE="$<TARGET_FILE:note>")
target_compile_definitions(gui_tests PRIVATE NOTEAPP_NOTES_GUI_EXE="$<TARGET_FILE:notes-gui>")
