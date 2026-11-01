#include "tui/termination.hpp"

#include <csignal>

namespace noteapp::tui {

namespace {

constexpr int kSignalExitBase = 128;

volatile std::sig_atomic_t g_signal = 0;

// Sinyal işleyicide yalnız sig_atomic_t yazmak güvenlidir; asıl iş döngü bitince yapılır.
extern "C" void record_signal(int signal) {
    g_signal = signal;
}

}  // namespace

void install_termination_handlers() {
    std::signal(SIGTERM, record_signal);
    std::signal(SIGINT, record_signal);
#ifdef SIGHUP  // Windows'ta yok
    std::signal(SIGHUP, record_signal);
#endif
}

int termination_signal() {
    return g_signal;
}

int termination_exit_code() {
    return g_signal == 0 ? 0 : kSignalExitBase + g_signal;
}

void reset_termination_for_test() {
    g_signal = 0;
    std::signal(SIGTERM, SIG_DFL);
    std::signal(SIGINT, SIG_DFL);
#ifdef SIGHUP
    std::signal(SIGHUP, SIG_DFL);
#endif
}

}  // namespace noteapp::tui
