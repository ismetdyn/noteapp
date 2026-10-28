# Qt sistemden gelir ve dinamik bağlanır (LGPLv3, SPEC.md §3); Windows'ta CMAKE_PREFIX_PATH preset'ten gelir.
find_package(Qt6 REQUIRED COMPONENTS Widgets Gui)

# PDF dışa aktarma kütüphanede: hem başsız `--export-pdf` modu hem offscreen testler aynı kodu çağırır.
add_library(noteapp_gui
    pdf_export.cpp
    note_list_model.cpp
    save_dialog.cpp
    settings_page.cpp
    main_window.cpp)
target_include_directories(noteapp_gui PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noteapp_gui PUBLIC noteapp_core noteapp_search noteapp_detect noteapp_sync Qt6::Widgets Qt6::Gui)
noteapp_target_defaults(noteapp_gui)

add_executable(notes-gui WIN32
    main.cpp)
target_link_libraries(notes-gui PRIVATE noteapp_gui Qt6::Widgets)
noteapp_target_defaults(notes-gui)

install(TARGETS notes-gui RUNTIME DESTINATION bin)
if(WIN32)
    # Qt DLL'leri, eklentiler ve MinGW çalışma zamanı kurulu notes-gui.exe'nin yanına konur;
    # qoffscreen: `note export --pdf` başsız çağırır; eklenti yoksa Qt (WIN32 alt sistemi) hata kutusunda asılır.
    # aynı DLL'ler `note` ve `notes` için de yeterlidir (hepsi bin/ içinde).
    find_program(NOTEAPP_WINDEPLOYQT windeployqt
        HINTS "${Qt6_DIR}/../../../bin" "$ENV{NOTEAPP_QT_DIR}/bin" REQUIRED)
    install(CODE "
        execute_process(
            COMMAND \"${NOTEAPP_WINDEPLOYQT}\" --release --no-translations --no-system-d3d-compiler
                    --no-opengl-sw --compiler-runtime --include-plugins qoffscreen
                    \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/notes-gui.exe\"
            RESULT_VARIABLE noteapp_deploy_result)
        if(NOT noteapp_deploy_result EQUAL 0)
            message(FATAL_ERROR \"windeployqt başarısız (çıkış: \${noteapp_deploy_result})\")
        endif()
    ")
endif()
