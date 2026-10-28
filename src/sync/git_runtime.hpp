#pragma once

namespace noteapp::sync {

// libgit2'nin global durumu (TLS, ayarlar, transport kayıtları) init/shutdown çiftine bağlıdır; her libgit2
// çağrısı bir GitRuntime ömrü içinde yapılmalı. libgit2 sayaç tuttuğu için iç içe örnekler güvenlidir.
class GitRuntime {
public:
    // Başarısız init yalnız kaynak tükenmesinde olur ve kurtarılamaz: std::runtime_error fırlatır.
    GitRuntime();
    ~GitRuntime();

    GitRuntime(const GitRuntime&) = delete;
    GitRuntime& operator=(const GitRuntime&) = delete;
};

}  // namespace noteapp::sync
