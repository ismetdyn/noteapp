# detect saf bir modüldür: core'a bağımlılığı yok, G/Ç yapmaz.
add_library(noteapp_detect detect.cpp structure.cpp)
target_include_directories(noteapp_detect PUBLIC ${PROJECT_SOURCE_DIR}/src)
noteapp_target_defaults(noteapp_detect)
