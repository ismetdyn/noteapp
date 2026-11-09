# Uygulama katmanı: TUI ve GUI aynı özelliğin iki ince arayüzüdür (plan-parity). Arayüzden bağımsız özellik
# mantığı (karar kuralları, durum geçişleri, ileti metni üretimi, depo işlemlerinin sırası) burada tek kez
# yazılır, iki arayüz onu çağırır. Çizim, tuş/fare eşlemesi ve widget durumu buraya girmez: FTXUI, ImGui ve SDL
# bağımlılığı yoktur. İki arayüzde de gereken bir yardımcı kopyalanmaz, buraya taşınır.
add_library(noteapp_app
    commands.cpp
    data_preview.cpp
    document.cpp
    export.cpp
    graph_layout.cpp
    import_files.cpp
    inspector.cpp
    ui_args.cpp
    item_ops.cpp
    label_fold.cpp
    listing.cpp
    map_camera.cpp
    map_query.cpp
    map_relations.cpp
    note_map.cpp
    notices.cpp
    settings.cpp
    sync_indicator.cpp
    sync_status_worker.cpp
    tabs.cpp
    trash_ops.cpp
    ui_prefs.cpp
    update_install.cpp
    updater.cpp)
if(WIN32)
    target_sources(noteapp_app PRIVATE update_install_win.cpp)
endif()
target_include_directories(noteapp_app PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noteapp_app PUBLIC noteapp_core noteapp_search noteapp_sync noteapp_detect
    PRIVATE noteapp_pdf noteapp_net tomlplusplus::tomlplusplus)
# Durum dosyaları (ui_prefs.cpp) ve sekmelerin `[[tabs]]` bloğu (tabs.cpp): ayrıştırma hatası parse_result ile
# döner (config.cpp ile aynı mod).
target_compile_definitions(noteapp_app PRIVATE TOML_EXCEPTIONS=0)
noteapp_target_defaults(noteapp_app)
