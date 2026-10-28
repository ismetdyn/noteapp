noteapp_add_test(gui
    pdf_smoke_test.cpp
    pdf_test.cpp
    note_list_model_test.cpp
    main_window_test.cpp
    save_test.cpp
    folders_test.cpp
    content_search_test.cpp)
# Ekransız ortamlarda (CI, Fedora container) da koşabilmek için pencere sistemi istemeyen platform eklentisi.
set_tests_properties(gui PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
# T33: başsız mod testleri gerçek notes-gui ikilisini alt süreçte çalıştırır.
add_dependencies(gui_tests notes-gui)
target_compile_definitions(gui_tests PRIVATE NOTEAPP_NOTES_GUI_EXE="$<TARGET_FILE:notes-gui>")
