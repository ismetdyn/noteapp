# Projedeki başka hiçbir kütüphaneye bağımlı değildir (SPEC-graph ilke 5). SQLite PRIVATE: başlıkları dışarı sızmaz.
add_library(noteapp_graph id.cpp graph.cpp store.cpp op.cpp oplog.cpp merge.cpp secure_file.cpp)
# internal/: SQLite türü taşıyan iç başlık. src/ PUBLIC olduğundan oradaki her başlık tüketiciye açıktır.
target_include_directories(noteapp_graph PUBLIC ${PROJECT_SOURCE_DIR}/src PRIVATE ${PROJECT_SOURCE_DIR}/internal)
target_link_libraries(noteapp_graph PRIVATE noteapp_sqlite)
noteapp_target_defaults(noteapp_graph)

add_subdirectory(cypher)
