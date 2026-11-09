# PDF okuma. İki kütüphane: MuPDF'i yalnız motor bağlar.
# noteapp_pdfview: istemci (MuPDF'siz): PdfDocument işçi süreci (`note __pdf-worker`) üzerinden okur; tel protokolü,
# metin temizliği. TUI ve GUI yalnız bunu bağlar.
add_library(noteapp_pdfview page_text.cpp pdf_document.cpp protocol.cpp)
target_include_directories(noteapp_pdfview PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(noteapp_pdfview PUBLIC noteapp_core)
noteapp_target_defaults(noteapp_pdfview)

# noteapp_pdfengine: MuPDF motoru ve işçi döngüsü; yalnız `note` (işçiyi barındırır) ve motor testleri bağlar. MuPDF
# başlıkları yalnız .cpp'dedir.
add_library(noteapp_pdfengine engine/memory_budget.cpp engine/pdf_engine.cpp engine/worker.cpp)
target_link_libraries(noteapp_pdfengine PUBLIC noteapp_pdfview PRIVATE noteapp_mupdf)
noteapp_target_defaults(noteapp_pdfengine)
