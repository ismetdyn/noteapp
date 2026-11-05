#pragma once

#include <functional>

namespace noteapp::cli {

// Uzun çıktıyı sayfalar (SPEC-cli "Pager"). `body` std::cout'a yazar ve çıkış kodunu döner. stdout TTY değilse
// ya da pager kapalıysa (`NOTEAPP_PAGER=cat`) gövde doğrudan yazar. Aksi halde çıktı belleğe alınır: terminale
// sığıyorsa doğrudan, sığmıyorsa pager'a (NOTEAPP_PAGER > PAGER > `less -FIRX`) gider; pager bulunamaz ya da
// başlamazsa düz yazılır (hata değil). Windows'ta pager yok. Çıkış kodu her durumda gövdeninkidir.
int run_paged(const std::function<int()>& body);

}  // namespace noteapp::cli
