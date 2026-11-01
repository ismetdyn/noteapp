noteapp_add_test(pdf pdf_export_test.cpp)
target_link_libraries(pdf_tests PRIVATE noteapp_core)
# Türkçe geri okuma ve yerleşim doğrulaması gerçek PDF okuyucuyla yapılır; Fedora imajı poppler-utils'i kurar.
# Eksikse durulur: testleri sessizce derlememek yanlış yeşil verirdi.
find_program(NOTEAPP_PDFTOTEXT pdftotext)
if(NOTEAPP_PDFTOTEXT AND CMAKE_CROSSCOMPILING)
    # Test Wine'da koşar; host pdftotext'ine köprü gerekir (pdftotext_wine.cpp).
    add_executable(pdftotext_wine pdftotext_wine.cpp)
    target_compile_definitions(pdftotext_wine PRIVATE NOTEAPP_HOST_PDFTOTEXT="${NOTEAPP_PDFTOTEXT}"
                               NOTEAPP_PDFTOTEXT_WINE_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/pdftotext_wine.sh")
    target_link_libraries(pdftotext_wine PRIVATE shell32)
    noteapp_target_defaults(pdftotext_wine)
    add_dependencies(pdf_tests pdftotext_wine)
    target_compile_definitions(pdf_tests PRIVATE NOTEAPP_PDFTOTEXT="$<TARGET_FILE:pdftotext_wine>")
elseif(NOTEAPP_PDFTOTEXT)
    target_compile_definitions(pdf_tests PRIVATE NOTEAPP_PDFTOTEXT="${NOTEAPP_PDFTOTEXT}")
else()
    message(FATAL_ERROR "pdftotext bulunamadı; PDF testleri onsuz doğrulanamaz. Kur: Fedora 'poppler-utils', "
                        "Windows: poppler'ın pdftotext.exe'si PATH'te olsun. "
                        "Testsiz derleme için: -DNOTEAPP_BUILD_TESTS=OFF")
endif()
