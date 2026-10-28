// sync testlerinde PATH'e konan sahte `ssh-keygen`: kullanıcının gerçek ~/.ssh/known_hosts'una dokunulmaz.
// Yalnız `-F known.example` ve `-F [known.example]:2222` "kayıt var" (0) döner, gerisi "yok" (1): böylece
// argv'nin biçimi de sınanır.
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 3 || std::string_view(argv[1]) != "-F")
        return 1;
    const std::string_view host = argv[2];
    return host == "known.example" || host == "[known.example]:2222" ? 0 : 1;
}
