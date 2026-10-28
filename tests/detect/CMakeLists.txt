noteapp_add_test(detect detect_test.cpp)
# Korpus kaynak ağacından okunur; kopyalamaya gerek yok.
target_compile_definitions(detect_tests PRIVATE
    NOTEAPP_DETECT_SAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/samples")
