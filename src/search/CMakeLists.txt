add_library(noteapp_search fold.cpp folder_order.cpp fuzzy.cpp content_index.cpp global_search.cpp query.cpp relation_search.cpp)
target_include_directories(noteapp_search PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noteapp_search PUBLIC noteapp_core)
noteapp_target_defaults(noteapp_search)
