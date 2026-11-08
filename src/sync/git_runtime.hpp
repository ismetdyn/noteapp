#pragma once

namespace noteapp::sync {

// libgit2'nin global durumu (TLS, ayarlar, transport kayıtları) init/shutdown çiftine bağlıdır; her libgit2
// çağrısı bir GitRuntime ömrü içinde yapılmalı. libgit2 sayaç tuttuğu için iç içe örnekler güvenlidir.
//
// Kurucu ayrıca kullanıcının global/XDG/sistem git config'ini devre dışı bırakır (SPEC-sync "Uygulama
// kararları" 1): core.autocrlf satır sonlarını, url.*.insteadOf remote adresini sessizce değiştirmesin.
// Yalıtım süreçte bir kez yapılır ve libgit2 süreç sonuna dek başlatılmış kalır (git_runtime.cpp): ayar
// kilitsiz bir globaldir, her kurulumda yazmak iş parçacıkları arasında yarışıyordu. Kurulum ve yıkım her
// iş parçacığından aynı anda güvenlidir.
class GitRuntime {
public:
    // Başarısız init yalnız kaynak tükenmesinde olur ve kurtarılamaz: std::runtime_error fırlatır.
    GitRuntime();
    ~GitRuntime();

    GitRuntime(const GitRuntime&) = delete;
    GitRuntime& operator=(const GitRuntime&) = delete;
};

}  // namespace noteapp::sync
