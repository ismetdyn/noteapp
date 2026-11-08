// sync testlerinde PATH'e konan sahte `ssh-keygen`: kullanıcının gerçek ~/.ssh/known_hosts'una dokunulmaz.
// Yalnız `-F known.example` ve `-F [known.example]:2222` "kayıt var" (0) döner, gerisi "yok" (1): böylece
// argv'nin biçimi de sınanır. Senkron token'ı ortamda gelirse 2 döner: ssh-keygen token'ı görmemeli.
#include <cstdlib>
#include <string_view>

int main(int argc, char** argv) {
    if (std::getenv("NOTEAPP_GIT_TOKEN") != nullptr || std::getenv("NOTEAPP_GIT_TOKEN_HOST") != nullptr)
        return 2;
    if (argc != 3 || std::string_view(argv[1]) != "-F")
        return 1;
    const std::string_view host = argv[2];
    return host == "known.example" || host == "[known.example]:2222" ? 0 : 1;
}
