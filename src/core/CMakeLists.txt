add_library(noteapp_core
    note_path.cpp
    note_store.cpp
    text.cpp
    version.cpp)
target_include_directories(noteapp_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(noteapp_core PRIVATE NOTEAPP_VERSION="${PROJECT_VERSION}")
noteapp_target_defaults(noteapp_core)
