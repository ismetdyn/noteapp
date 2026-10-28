# Qt sistemden gelir ve dinamik bağlanır (LGPLv3, SPEC.md §3); Windows'ta CMAKE_PREFIX_PATH preset'ten gelir.
find_package(Qt6 REQUIRED COMPONENTS Widgets Gui)

# PDF dışa aktarma kütüphanede: hem başsız `--export-pdf` modu hem offscreen testler aynı kodu çağırır.
add_library(noteapp_gui
    pdf_export.cpp
    note_list_model.cpp
    save_dialog.cpp
    main_window.cpp)
target_include_directories(noteapp_gui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noteapp_gui PUBLIC noteapp_core noteapp_search noteapp_detect Qt6::Widgets Qt6::Gui)
noteapp_target_defaults(noteapp_gui)

add_executable(notes-gui WIN32
    main.cpp)
target_link_libraries(notes-gui PRIVATE noteapp_gui Qt6::Widgets)
noteapp_target_defaults(notes-gui)
