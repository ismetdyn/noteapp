#pragma once

// cli testlerinin ortak kum havuzu (NOTEAPP_NOTE_EXE ile derlenmeli).
#include "support/note_seed.hpp"
#include "support/run_process.hpp"

#include <chrono>
#include <fstream>
#include <string>
#include <vector>

namespace noteapp::test {

// Her test kendi NOTEAPP_HOME'unu alır; notlar onun altındaki notes/ dizinine düşer.
class Sandbox {
public:
    explicit Sandbox(const std::string& language = "tr") : env_("NOTEAPP_HOME", utf8(home_.path())) {
        fs::create_directories(home_.path() / "config");
        std::ofstream config(home_.path() / "config" / "config.toml", std::ios::binary);
        config << "language = \"" << language << "\"\n"
               << "notes_dir = \"" << toml_path(notes()) << "\"\n";
    }

    fs::path notes() const { return home_.path() / "notes"; }
    fs::path config_file() const { return home_.path() / "config" / "config.toml"; }
    RunResult run(const std::vector<std::string>& args, const std::string& stdin_bytes = "") const {
        return run_process(NOTEAPP_NOTE_EXE, args, stdin_bytes);
    }
    RunResult run_null_stdin(const std::vector<std::string>& args) const {
        return run_process(NOTEAPP_NOTE_EXE, args, "", true);
    }
    fs::path data() const { return home_.path() / "data"; }
    bool has_any_note() const { return !core::NoteStore(data()).list().empty(); }
    // Notlar graph.db'dedir: testin baytları depodan girer, depodan okunur. age_seconds büyük = eski.
    void put(std::string_view path, std::string_view bytes, int age_seconds = 0) const {
        upsert_note(data(), path, bytes,
                    std::chrono::system_clock::now() - std::chrono::seconds(age_seconds));
    }
    std::string body(std::string_view path) const { return note_body(data(), path); }
    bool exists(std::string_view path) const { return note_exists(data(), path); }

private:
    // TOML'da ters bölü kaçış sayılır; ileri bölü Windows'ta da geçerli bir yol ayracıdır.
    static std::string toml_path(const fs::path& p) {
        std::string s = utf8(p);
        for (char& c : s)
            if (c == '\\')
                c = '/';
        return s;
    }

    TempDir home_;
    ScopedEnv env_;
};

}  // namespace noteapp::test
