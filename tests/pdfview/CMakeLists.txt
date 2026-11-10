noteapp_add_test(pdfview
    fixtures.cpp
    memory_budget_test.cpp
    page_text_test.cpp
    pdf_document_test.cpp
    pdf_engine_test.cpp
    protocol_test.cpp
    sandbox_test.cpp)
# Fixture PDF'leri testte üretilir (depoda ikili dosya yok, fixtures.hpp). Motor testleri MuPDF'i süreç içinde
# (noteapp_pdfengine), istemci testleri gerçek `note __pdf-worker`'ı ve bozuk davranan sahte işçiyi alt süreçte
# çalıştırır. Flate için MuPDF'in Z_PREFIX'li zlib'i; FZ_ERROR_* sabitleri için MuPDF başlıkları.
target_link_libraries(pdfview_tests PRIVATE noteapp_pdfengine noteapp_pdf hpdf noteapp_mupdf noteapp_mupdf_zlib)

add_executable(fake_pdf_worker ${PROJECT_SOURCE_DIR}/tests/support/fake_pdf_worker.cpp)
target_link_libraries(fake_pdf_worker PRIVATE noteapp_pdfview)
noteapp_target_defaults(fake_pdf_worker)

add_dependencies(pdfview_tests note fake_pdf_worker)
target_compile_definitions(pdfview_tests PRIVATE NOTEAPP_NOTE_EXE="$<TARGET_FILE:note>"
                                                 NOTEAPP_FAKE_PDF_WORKER="$<TARGET_FILE:fake_pdf_worker>")

# PDF-SBX kum havuzu probu: işçinin enter_sandbox()'ından sonra tek bir sistem çağrısı dener (sandbox_test.cpp).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(pdf_sandbox_probe ${PROJECT_SOURCE_DIR}/tests/support/pdf_sandbox_probe.cpp)
    target_link_libraries(pdf_sandbox_probe PRIVATE noteapp_pdfengine)
    noteapp_target_defaults(pdf_sandbox_probe)
    add_dependencies(pdfview_tests pdf_sandbox_probe)
    target_compile_definitions(pdfview_tests PRIVATE NOTEAPP_PDF_SANDBOX_PROBE="$<TARGET_FILE:pdf_sandbox_probe>")
endif()

# Bağlama sınırı: MuPDF yalnız `note`'ta (PDF işçisi); TUI (`notes`) ve GUI (`notes-gui`) onu bağlamaz. Pozitif
# denetimler: `note`'ta MuPDF'in bağlam kurucusu var (yoksa sembol adı değişmiş), her ikilide `main` var (dosya yok,
# nm hatası ya da strip edilmiş ikili denetimi boşa geçirmesin). İkililer pdfview_tests'le birlikte derlenir.
if(UNIX AND NOT CMAKE_CROSSCOMPILING)
    set(pdf_free_executables $<TARGET_FILE:notes>)
    add_dependencies(pdfview_tests notes)
    if(NOTEAPP_BUILD_GUI)
        list(APPEND pdf_free_executables $<TARGET_FILE:notes-gui>)
        add_dependencies(pdfview_tests notes-gui)
    endif()
    add_test(NAME pdfview_link_check
             COMMAND bash -c [=[
                 set -u
                 symbols_of() {
                     [ -f "$1" ] || { echo "$1: missing"; exit 1; }
                     nm --defined-only "$1" > "$tmp" || { echo "$1: nm failed"; exit 1; }
                     grep -q ' main$' "$tmp" || { echo "$1: no symbols (stripped?)"; exit 1; }
                 }
                 tmp="$(mktemp)" || exit 1
                 trap 'rm -f "$tmp"' EXIT
                 symbols_of "$1"
                 grep -q ' fz_new_context_imp$' "$tmp" || { echo "$1: MuPDF missing"; exit 1; }
                 shift
                 for exe in "$@"; do
                     symbols_of "$exe"
                     if grep -q ' fz_new_context_imp$' "$tmp"; then
                         echo "$exe links MuPDF"; exit 1
                     fi
                 done
                 echo "link check ok"]=] link-check $<TARGET_FILE:note> ${pdf_free_executables})
endif()
