add_library(noteapp_core
    atomic_file.cpp
    i18n.cpp
    log.cpp
    config.cpp
    console.cpp
    editor.cpp
    note_path.cpp
    note_store.cpp
    paths.cpp
    process.cpp
    text.cpp
    version.cpp)
target_include_directories(noteapp_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(noteapp_core PRIVATE NOTEAPP_VERSION="${PROJECT_VERSION}")
# toml++ yalnız config.cpp'de kullanılır; istisnasız mod: ayrıştırma hatası parse_result ile döner.
target_link_libraries(noteapp_core PRIVATE tomlplusplus::tomlplusplus)
target_compile_definitions(noteapp_core PRIVATE TOML_EXCEPTIONS=0)
noteapp_embed_resources(noteapp_core defaults.toml i18n/en.toml i18n/tr.toml)
noteapp_target_defaults(noteapp_core)
