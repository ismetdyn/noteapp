// e2e_editor.sh'in depo yardımcısı: notlar artık dosya değil. `notes`la aynı graph.db'ye ayrı süreçten
// dokunur; dış değişiklik senaryosu tam olarak budur. Kullanım: put YOL [YYYY-AA-GG] (bayt stdin'den) | cat
// YOL | clear | count | has YOL | in-trash YOL | revision YOL
#include "core/note_store.hpp"
#include "core/paths.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <iostream>
#include <iterator>
#include <string>

namespace core = noteapp::core;

namespace {

std::chrono::system_clock::time_point at_date(const std::string& text) {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (std::sscanf(text.c_str(), "%d-%u-%u", &year, &month, &day) != 3)
        return std::chrono::system_clock::now();
    return std::chrono::sys_days(std::chrono::year(year) / std::chrono::month(month) / std::chrono::day(day));
}

int put(core::NoteStore& store, const std::string& path, std::chrono::system_clock::time_point when) {
    const std::string bytes{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
    const auto parsed = core::NotePath::parse(path);
    if (!parsed)
        return 2;
    if (const auto current = store.read(*parsed))
        return store.write(*parsed, bytes, current->revision, when).has_value() ? 0 : 1;
    const auto slash = path.rfind('/');
    const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash);
    const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto dot = file.rfind('.');
    return store.create_exact(core::NewNote{folder, file.substr(0, dot), file.substr(dot + 1), bytes}, when)
                   .has_value()
               ? 0
               : 1;
}

int clear(core::NoteStore& store) {
    for (const core::NoteInfo& info : store.list())
        if (!store.remove(info.path))
            return 1;
    if (!store.empty_trash())
        return 1;
    for (const std::string& folder : store.folders())
        static_cast<void>(store.remove_folder(folder, true));  // alt klasör üstüyle gitmiş olabilir
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2)
        return 2;
    const std::string command = argv[1];
    const std::string path = argc > 2 ? argv[2] : "";
    core::NoteStore store(core::data_dir());
    if (command == "put")
        return put(store, path, argc > 3 ? at_date(argv[3]) : std::chrono::system_clock::now());
    if (command == "clear")
        return clear(store);
    if (command == "count") {
        std::cout << store.list().size() << '\n';
        return 0;
    }
    const auto parsed = core::NotePath::parse(path);
    if (!parsed)
        return 2;
    if (command == "in-trash") {
        const auto items = store.list_trash();
        return items && std::ranges::any_of(*items,
                                            [&](const core::TrashItem& item) { return item.path == *parsed; })
                   ? 0
                   : 1;
    }
    const auto content = store.read(*parsed);
    if (!content)
        return 1;
    if (command == "cat")
        std::cout.write(content->text.data(), static_cast<std::streamsize>(content->text.size()));
    else if (command == "revision")
        std::cout << content->revision.seq << '\n';
    else if (command != "has")
        return 2;
    return 0;
}
