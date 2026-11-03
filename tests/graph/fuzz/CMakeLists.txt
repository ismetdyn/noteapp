# SPEC-graph kabul 5: libFuzzer hedefleri. Koşturma: tests/graph/fuzz/run_fuzz.sh.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "NOTEAPP_BUILD_FUZZ needs clang: libFuzzer ships with compiler-rt")
endif()

# Kapsam yalnız motorda: SQLite'ın iç dalları derlemi şişirir, sorgu dili yüzeyini büyütmez.
target_compile_options(noteapp_graph PRIVATE -fsanitize=fuzzer-no-link)

foreach(target fuzz_query fuzz_frame fuzz_log)
    add_executable(${target} ${target}.cpp)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/internal)
    target_link_libraries(${target} PRIVATE noteapp_graph)
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
    noteapp_target_defaults(${target})
endforeach()

# Yalnız başlık: motora değil core'un JSON ayrıştırıcısına bakar.
add_executable(fuzz_jsonl fuzz_jsonl.cpp)
target_include_directories(fuzz_jsonl PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_options(fuzz_jsonl PRIVATE -fsanitize=fuzzer)
target_link_options(fuzz_jsonl PRIVATE -fsanitize=fuzzer)
noteapp_target_defaults(fuzz_jsonl)

# LSP çerçeveleyicisi ve sunucu mesajı ayrıştırıcıları (core). Ayrıştırıcılar noteapp_core'da derlenir: kütüphane
# kapsam izlemesiz kalırsa libFuzzer yalnız hedef dosyayı görür, girdi rastgele kalır.
target_compile_options(noteapp_core PRIVATE -fsanitize=fuzzer-no-link)
add_executable(fuzz_lsp fuzz_lsp.cpp)
target_link_libraries(fuzz_lsp PRIVATE noteapp_core)
target_compile_options(fuzz_lsp PRIVATE -fsanitize=fuzzer)
target_link_options(fuzz_lsp PRIVATE -fsanitize=fuzzer)
noteapp_target_defaults(fuzz_lsp)

add_executable(make_frame_seeds make_frame_seeds.cpp)
target_include_directories(make_frame_seeds PRIVATE ${PROJECT_SOURCE_DIR}/internal)
target_link_libraries(make_frame_seeds PRIVATE noteapp_graph)
noteapp_target_defaults(make_frame_seeds)
