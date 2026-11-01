#pragma once

namespace noteapp::tui {

// SIGTERM, SIGINT ve SIGHUP süreci hemen öldürmesin diye kurulur: FTXUI kendi işleyicisini üstüne koyar,
// döngü bitince sinyali bu işleyiciye yeniden yükseltir. Böylece döngü normal biter ve kirli metin
// kaydedilir.
void install_termination_handlers();
// Alınan sonlandırma sinyali; yoksa 0.
int termination_signal();
// Sinyalle bitişte kabuğun beklediği 128+N; aksi halde 0.
int termination_exit_code();
void reset_termination_for_test();

}  // namespace noteapp::tui
