add_library(noteapp_pdf
    markdown_blocks.cpp
    pdf_export.cpp)
target_include_directories(noteapp_pdf PUBLIC ${PROJECT_SOURCE_DIR}/src)
noteapp_embed_fonts(noteapp_pdf
    fonts/DejaVuSans.ttf
    fonts/DejaVuSans-Bold.ttf
    fonts/DejaVuSans-Oblique.ttf
    fonts/DejaVuSans-BoldOblique.ttf
    fonts/DejaVuSansMono.ttf)
target_link_libraries(noteapp_pdf PUBLIC noteapp_core PRIVATE hpdf md4c)
noteapp_target_defaults(noteapp_pdf)
