#pragma once

// `note update` akışı (SPEC-cli "Güncelleme"): komut satırı ayrıştırması, sinyal ve üretim
// bağımlılıkları commands_update.cpp'de; bu akış G/Ç'yi parametre olarak alır, cli testleri sahte HTTP ve
// kurulumla süreç içinde koşar.
#include "app/update_worker.hpp"

#include <functional>
#include <iosfwd>
#include <optional>
#include <string>

namespace noteapp::cli {

struct UpdateOptions {
    bool check_only = false;  // --check: yalnız denetle
    bool yes = false;         // --yes: sormadan kur
};

struct UpdateConsole {
    std::ostream& out;  // sonuç (betikler okur)
    std::ostream& err;  // istem, hata, ret
    bool stdin_tty = false;
    // Onay yanıtı (bir satır); okunamazsa nullopt.
    std::function<std::optional<std::string>()> read_line;
    // Kullanıcı iptal istedi mi (Ctrl-C). İşçi beklenirken yoklanır.
    std::function<bool()> interrupted;
    // Denetim bittikten sonra, onaydan ve kurulumdan önce çağrılır: denetim bitince gelen eski bir Ctrl-C
    // sonradan onaylanan kurulumu iptal etmesin.
    std::function<void()> reset_interrupt;
};

// Çıkış: 0 güncel, (--check) yeni sürüm var ya da kuruldu; 1 onay yok, TTY'siz --yes'siz, kendini
// güncelleyemeyen kurulum; 3 ağ, doğrulama ya da kurulum hatası; 130 Ctrl-C ile iptal.
int run_update_flow(const UpdateOptions& options, app::UpdateWorker& worker, UpdateConsole& console);

}  // namespace noteapp::cli
