#pragma once

// `note __pdf-worker`: PDF işçi süreci. stdin'den protocol.hpp istek çerçevelerini okur, MuPDF motoruyla (PdfEngine)
// işler, cevabı stdout'a yazar. Tek iş parçacıklıdır; süre ve iptali ebeveyn tutar (öldürerek). Config okumaz, log
// açmaz.
namespace noteapp::pdfview {

// Çıkış kodu: 0 stdin EOF (temiz), 2 stdin terminal (elle başlatılmış), 3 bozuk istek, 4 bellek yetmedi, 5 stdout'a
// yazılamadı (ebeveyn gitti), 6 kum havuzu kurulamadı (sandbox.hpp; tek bayt okunmadan). İzin listesi dışındaki
// sistem çağrısı süreci SIGSYS ile öldürür (ebeveyn 159 görür).
int run_pdf_worker();

}  // namespace noteapp::pdfview
