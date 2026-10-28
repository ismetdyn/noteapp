# Görünüm kütüphanede: ekranı metne render eden testler (SPEC-tui kabul 1) terminal açmadan aynı kodu çağırır.
add_library(noteapp_tui
    view.cpp
    modals.cpp
    save_dialog.cpp
    tui_state.cpp)
target_include_directories(noteapp_tui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noteapp_tui PUBLIC noteapp_core noteapp_detect noteapp_search ftxui::dom)
noteapp_target_defaults(noteapp_tui)

add_executable(notes
    main.cpp)
target_link_libraries(notes PRIVATE noteapp_tui ftxui::component)
noteapp_target_defaults(notes)
