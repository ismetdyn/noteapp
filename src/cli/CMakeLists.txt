add_executable(note
    main.cpp
    args.cpp
    commands.cpp
    commands_complete.cpp
    commands_export.cpp
    commands_graph.cpp
    commands_import.cpp
    commands_query.cpp
    commands_view.cpp
    help.cpp
    pager.cpp
    sync_commands.cpp
    term.cpp)
target_include_directories(note PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(note PRIVATE noteapp_app noteapp_core noteapp_detect noteapp_search noteapp_sync noteapp_pdf noteapp_graph)
noteapp_target_defaults(note)

install(TARGETS note RUNTIME DESTINATION bin)
