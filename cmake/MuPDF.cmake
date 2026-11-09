# MuPDF 1.28.6'nın yalnız PDF okuyan alt kümesi (`noteapp_mupdf`); arşivi Dependencies.cmake indirir.
#
# Neden CMake hedefi, ExternalProject(make) değil: MuPDF'in Makefile'ı derleyiciyi, bayrakları ve platformu kendisi
# seçer (uname, pkg-config); MinGW çapraz derleme kipi yoktur ve CMake araç zincirini (mingw-w64-x86_64.cmake,
# NOTEAPP_BUILD_FUZZ/ASan bayrakları, -static) görmez. Kaynakları doğrudan hedefe almak her platformda aynı
# derleyiciyi ve bayrakları kullanır, ninja paralelliğine girer, ek araç (make, python) istemez. Bedel: kaynak
# listesi elle tutulur; aşağıdaki listeler 1.28.6'nın Makelists/Makefile'ından birebir alınmıştır, sürüm
# yükseltmede yeniden karşılaştırılır.
#
# Kapalı özellikler (Makerules'un `html=no xps=no svg=no mujs=no brotli=no extract=no barcode=no tofu=yes
# tofu_cjk=yes` karşılığı, ek olarak CBZ/görsel belge, OCR/DOCX/ODT çıktısı ve ICC):
# - Yalnız PDF açılır; XPS/SVG/HTML/EPUB/FB2/MOBI/TXT/Office/Markdown/CBZ/görsel dosya işleyicileri derlenmez.
# - JavaScript (mujs) yok: güvenilmez PDF'in betiği çalışmaz. HarfBuzz/Gumbo/cmark yalnız HTML motoru içindir.
# - ICC kapalı (lcms2 yok): renk yönetimi yerine aygıt uzayına çevrilir; önizleme için yeterli, saldırı yüzeyi küçük.
# - Brotli (PDF 2.0 dışı uzantı), OCR (tesseract/leptonica), barkod (zxing), DOCX/ODT çıktısı (extract) yok.
# - Gömülü fontlar yalnız Base14 (URW, 0,6 MB); Noto (13 MB), CJK (Source Han + Droid, 33 MB) ve SIL yok: gömülmemiş
#   bu fontları kullanan PDF'te o glifler kutu çizilir, metin çıkarma etkilenmez (ToUnicode/CMap'ler derlenir).
# Kalan gömülü üçüncü partiler PDF'in temel filtreleridir: zlib (Flate), libjpeg (DCT), openjpeg (JPX), jbig2dec
# (JBIG2), FreeType (font).
#
# zlib çakışması: libgit2 kendi zlib'ini (1.3.1, önek yok) libgit2.a'ya gömer, MuPDF'inki 1.3.2'dir. İkisi aynı
# adlarla (inflate, crc32, ...) bağlansaydı bağlayıcı ya çift tanım verir ya da sessizce birinin sürümünü ötekine
# kullandırırdı. MuPDF'in zlib'i ve onu çağıran MuPDF kaynakları Z_PREFIX ile derlenir: zconf.h tüm dış adları
# `z_` önekine çevirir (z_inflate, z_crc32, ...). Tek istisna `z_errmsg`: zlib onu zaten `z_` ile başladığı için
# yeniden adlandırmaz, iki zlib de tanımlar; aynı exe'de iki zutil.o birden bağlanırsa çift tanım olurdu. MuPDF'inki
# yalnız zlib'in iç dosyalarında (zutil.h ERR_MSG) geçer, PRIVATE tanımla yeniden adlandırılır. Denetim: `nm -g
# --defined-only` ile MuPDF arşivleri ve libgit2.a'nın ortak tanımlı sembolü yoktur.

set(mupdf_dir ${mupdf_SOURCE_DIR})
set(mupdf_third ${mupdf_dir}/thirdparty)

# Üçüncü parti C kodunun uyarıları bizim -Werror'umuza girmez (sqlite ile aynı).
set(mupdf_quiet $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)

add_library(noteapp_mupdf_zlib STATIC
    ${mupdf_third}/zlib/adler32.c ${mupdf_third}/zlib/compress.c ${mupdf_third}/zlib/crc32.c
    ${mupdf_third}/zlib/deflate.c ${mupdf_third}/zlib/inffast.c ${mupdf_third}/zlib/inflate.c
    ${mupdf_third}/zlib/inftrees.c ${mupdf_third}/zlib/trees.c ${mupdf_third}/zlib/uncompr.c
    ${mupdf_third}/zlib/zutil.c)
target_include_directories(noteapp_mupdf_zlib SYSTEM PUBLIC ${mupdf_third}/zlib)
target_compile_definitions(noteapp_mupdf_zlib PUBLIC Z_PREFIX
    PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:HAVE_UNISTD_H> HAVE_STDARG_H z_errmsg=noteapp_mupdf_z_errmsg)
target_compile_options(noteapp_mupdf_zlib PRIVATE ${mupdf_quiet})

set(mupdf_freetype_sources base/ftbase.c base/ftbbox.c base/ftbitmap.c base/ftdebug.c base/ftfstype.c base/ftgasp.c
    base/ftglyph.c base/ftinit.c base/ftstroke.c base/ftsynth.c base/ftsystem.c base/fttype1.c cff/cff.c
    cid/type1cid.c psaux/psaux.c pshinter/pshinter.c psnames/psnames.c raster/raster.c sfnt/sfnt.c smooth/smooth.c
    truetype/truetype.c type1/type1.c)
list(TRANSFORM mupdf_freetype_sources PREPEND ${mupdf_third}/freetype/src/)
add_library(noteapp_mupdf_freetype STATIC ${mupdf_freetype_sources})
target_include_directories(noteapp_mupdf_freetype SYSTEM PUBLIC
    ${mupdf_third}/freetype/include ${mupdf_dir}/scripts/freetype)
target_compile_definitions(noteapp_mupdf_freetype PRIVATE
    "FT_CONFIG_MODULES_H=\"slimftmodules.h\"" "FT_CONFIG_OPTIONS_H=\"slimftoptions.h\"" FT2_BUILD_LIBRARY)
target_compile_options(noteapp_mupdf_freetype PRIVATE ${mupdf_quiet})

set(mupdf_jpeg_sources jaricom.c jcapimin.c jcapistd.c jcarith.c jccoefct.c jccolor.c jcdctmgr.c jchuff.c jcinit.c
    jcmainct.c jcmarker.c jcmaster.c jcomapi.c jcparam.c jcprepct.c jcsample.c jdapimin.c jdapistd.c jdarith.c
    jdatadst.c jdatasrc.c jdcoefct.c jdcolor.c jddctmgr.c jdhuff.c jdinput.c jdmainct.c jdmarker.c jdmaster.c
    jdmerge.c jdpostct.c jdsample.c jdtrans.c jerror.c jfdctflt.c jfdctfst.c jfdctint.c jidctflt.c jidctfst.c
    jidctint.c jmemmgr.c jquant1.c jquant2.c jutils.c)
list(TRANSFORM mupdf_jpeg_sources PREPEND ${mupdf_third}/libjpeg/)
add_library(noteapp_mupdf_jpeg STATIC ${mupdf_jpeg_sources})
target_include_directories(noteapp_mupdf_jpeg SYSTEM PUBLIC ${mupdf_third}/libjpeg ${mupdf_dir}/scripts/libjpeg)
target_compile_options(noteapp_mupdf_jpeg PRIVATE ${mupdf_quiet})

set(mupdf_jbig2_sources jbig2.c jbig2_arith.c jbig2_arith_iaid.c jbig2_arith_int.c jbig2_generic.c jbig2_halftone.c
    jbig2_huffman.c jbig2_hufftab.c jbig2_image.c jbig2_mmr.c jbig2_page.c jbig2_refinement.c jbig2_segment.c
    jbig2_symbol_dict.c jbig2_text.c)
list(TRANSFORM mupdf_jbig2_sources PREPEND ${mupdf_third}/jbig2dec/)
add_library(noteapp_mupdf_jbig2 STATIC ${mupdf_jbig2_sources})
target_include_directories(noteapp_mupdf_jbig2 SYSTEM PUBLIC ${mupdf_third}/jbig2dec PRIVATE ${mupdf_dir}/include)
target_compile_definitions(noteapp_mupdf_jbig2 PRIVATE HAVE_STDINT_H "JBIG_EXTERNAL_MEMENTO_H=\"mupdf/memento.h\"")
target_compile_options(noteapp_mupdf_jbig2 PRIVATE ${mupdf_quiet})

set(mupdf_openjpeg_sources bio.c cio.c dwt.c event.c function_list.c ht_dec.c image.c invert.c j2k.c jp2.c mct.c
    mqc.c openjpeg.c pi.c sparse_array.c t1.c t2.c tcd.c tgt.c thread.c)
list(TRANSFORM mupdf_openjpeg_sources PREPEND ${mupdf_third}/openjpeg/src/lib/openjp2/)
add_library(noteapp_mupdf_openjpeg STATIC ${mupdf_openjpeg_sources})
target_include_directories(noteapp_mupdf_openjpeg SYSTEM PUBLIC ${mupdf_third}/openjpeg/src/lib/openjp2)
target_compile_definitions(noteapp_mupdf_openjpeg PUBLIC OPJ_STATIC OPJ_HAVE_INTTYPES_H OPJ_HAVE_STDINT_H
    PRIVATE MUTEX_pthread=0)
target_compile_options(noteapp_mupdf_openjpeg PRIVATE ${mupdf_quiet})

# MuPDF'in kendisi: Makefile'ın `source/fitz/*.c source/pdf/*.c` kalıbı (sabit arşivde glob deterministiktir;
# sıralı). fitz/*.cpp yalnız OCR (tesseract) ve barkod (zxing) içindir, ikisi de kapalı. Makefile'ın her zaman
# eklediği source/cbz ve source/reflow belge işleyicisidir: PDF doğrudan açıldığından gerekmez.
file(GLOB mupdf_sources ${mupdf_dir}/source/fitz/*.c ${mupdf_dir}/source/pdf/*.c)
list(SORT mupdf_sources)
# Base14 PDF fontları; TOFU ile font-table.h yalnız bunları ister (NimbusBoxes Noto kümesindedir).
set(mupdf_fonts Dingbats NimbusMonoPS-Bold NimbusMonoPS-BoldItalic NimbusMonoPS-Italic NimbusMonoPS-Regular
    NimbusRoman-Bold NimbusRoman-BoldItalic NimbusRoman-Italic NimbusRoman-Regular NimbusSans-Bold
    NimbusSans-BoldItalic NimbusSans-Italic NimbusSans-Regular StandardSymbolsPS)
list(TRANSFORM mupdf_fonts PREPEND ${mupdf_dir}/generated/resources/fonts/urw/)
list(TRANSFORM mupdf_fonts APPEND .cff.c)
add_library(noteapp_mupdf STATIC ${mupdf_sources} ${mupdf_fonts})
target_include_directories(noteapp_mupdf SYSTEM PUBLIC ${mupdf_dir}/include)
# Özellik anahtarları PUBLIC: fitz/config.h başlıkları okuyan bizim kodumuzda da aynı değerleri görsün.
target_compile_definitions(noteapp_mupdf
    PUBLIC FZ_ENABLE_XPS=0 FZ_ENABLE_SVG=0 FZ_ENABLE_CBZ=0 FZ_ENABLE_IMG=0 FZ_ENABLE_HTML=0 FZ_ENABLE_EPUB=0
           FZ_ENABLE_FB2=0 FZ_ENABLE_MOBI=0 FZ_ENABLE_TXT=0 FZ_ENABLE_OFFICE=0 FZ_ENABLE_MD=0
           FZ_ENABLE_HTML_ENGINE=0 FZ_ENABLE_OCR_OUTPUT=0 FZ_ENABLE_DOCX_OUTPUT=0 FZ_ENABLE_ODT_OUTPUT=0
           FZ_ENABLE_JS=0 FZ_ENABLE_BROTLI=0 FZ_ENABLE_BARCODE=0 FZ_ENABLE_ICC=0
    PRIVATE TOFU TOFU_CJK
            # system.h MSVC makrolarına (_M_X64) bakıp SSE4.1 yolunu açar; MinGW bu makroları tanımlar ama derleyici
            # -msse4.1'siz SSE4.1 iç işlevlerini derleyemez (deskew.c). Taban x86-64 kalır: Linux'ta da 0'dır.
            ARCH_HAS_SSE=0)
target_compile_options(noteapp_mupdf PRIVATE ${mupdf_quiet})
target_link_libraries(noteapp_mupdf PRIVATE
    noteapp_mupdf_freetype noteapp_mupdf_jpeg noteapp_mupdf_jbig2 noteapp_mupdf_openjpeg noteapp_mupdf_zlib)
if(NOT WIN32)
    target_link_libraries(noteapp_mupdf PRIVATE m)
endif()
