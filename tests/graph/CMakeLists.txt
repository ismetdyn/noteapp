noteapp_add_test(graph
    app_api_test.cpp
    id_test.cpp
    op_test.cpp
    oplog_test.cpp
    merge_test.cpp
    sync_test.cpp
    sync_security_test.cpp
    graph_test.cpp
    store_test.cpp
    source_scan_test.cpp
    process_test.cpp)
# Dosya başlığını ve depolama planını doğrudan sınayan testler SQLite C API'sini kullanır; kaynak taraması da src/graph'ı okur.
target_link_libraries(graph_tests PRIVATE noteapp_sqlite)
target_include_directories(graph_tests PRIVATE ${PROJECT_SOURCE_DIR}/internal)
target_compile_definitions(graph_tests PRIVATE
    NOTEAPP_GRAPH_SOURCE_DIR="${PROJECT_SOURCE_DIR}/src/graph"
    NOTEAPP_GRAPH_INTERNAL_DIR="${PROJECT_SOURCE_DIR}/internal/graph")

add_subdirectory(cypher)
