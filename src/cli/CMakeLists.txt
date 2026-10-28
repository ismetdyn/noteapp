add_executable(note
    main.cpp
    args.cpp
    commands.cpp
    commands_export.cpp
    sync_commands.cpp)
target_include_directories(note PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(note PRIVATE noteapp_core noteapp_detect noteapp_search noteapp_sync)
if(WIN32)
    target_link_libraries(note PRIVATE shell32)  # CommandLineToArgvW
endif()
noteapp_target_defaults(note)
