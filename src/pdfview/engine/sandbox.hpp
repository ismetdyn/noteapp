#pragma once

// PDF işçisinin kum havuzu (PDF-SBX, ADR 0002 yeniden değerlendirme). İşçi tek bayt okumadan önce çağırır; tek iş
// parçacığı varken çağrılmalıdır (filtre sonra açılan iş parçacıklarına miras kalır).
namespace noteapp::pdfview {

// Linux x86_64: 3 ve üstü fd'leri kapatır, stdin/stdout'a sabit tampon verir, NO_NEW_PRIVS ve seccomp-bpf izin
// listesini kurar. Biri başarısızsa (ya da Linux'ta desteklenmeyen mimaride) false: çağıran PDF okumadan çıkar.
// Windows'ta bir şey yapmaz (sınırlar ebeveynin başlatmasındadır) ve true döner.
bool enter_sandbox();

}  // namespace noteapp::pdfview
