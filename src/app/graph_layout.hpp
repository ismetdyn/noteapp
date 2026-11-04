#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace noteapp::app {

struct LayoutPoint {
    float x = 0.0F;
    float y = 0.0F;
};

// Fruchterman–Reingold kuvvet yerleşimi, makalenin ızgara sürümü: itme yalnız 2k yakınındaki düğümler
// arasında, adım maliyeti düğüm ve kenar sayısıyla doğrusal kalır (2.000 düğümde O(n²) kare bütçesini yerdi);
// bağsız notlar dağılmasın diye zayıf merkez çekimi. Saf: ImGui bilmez, aynı girdi aynı sonucu verir.
// Sıcaklık her adımda soğur; eşiğin altında step iş yapmaz (boşta CPU yok).
class GraphLayout {
public:
    struct Edge {
        std::size_t from{};
        std::size_t to{};
        bool operator==(const Edge&) const = default;
    };
    // İdeal kenar boyu k (yerleşim birimi); kamera ölçeği bunun üstüne gelir.
    static constexpr float kIdealEdge = 100.0F;

    // Düğümler anahtarla tanınır: önceki çizgede olan anahtar konumunu ve iğnesini korur. Yeni düğüm, eski
    // bir komşusu varsa onun yanına, yoksa anahtarının özetinden türeyen konuma konur (tohum sabit, sıra
    // bağımsız). Çizge aynıysa hiçbir şey değişmez (false); değiştiyse sıcaklık yeniden yükselir (true).
    bool set_graph(std::vector<std::string> keys, std::vector<Edge> edges);
    // En çok `iterations` adım atar; yerleşim hâlâ hareket ediyorsa true.
    bool step(int iterations);
    bool settled() const;

    std::size_t size() const { return positions_.size(); }
    const std::vector<LayoutPoint>& positions() const { return positions_; }
    bool pinned(std::size_t node) const { return pinned_[node] != 0; }
    // Sürüklenen düğüm: konumu verilir ve sabitlenir. Komşular yeniden yerleşsin diye çizge ılınır.
    void pin(std::size_t node, LayoutPoint at);

    // FR potansiyeli (kenarda d³/3k, kesme yarıçapı içindeki çiftte k²·ln(2k/d), merkez çekiminde g·d²/2):
    // testler azaldığını ölçer.
    // O(n²); kare yolunda çağrılmaz.
    double energy() const;
    std::size_t iterations_done() const { return iterations_done_; }  // test kancası

private:
    void iterate();

    std::vector<std::string> keys_;
    std::vector<Edge> edges_;
    std::vector<LayoutPoint> positions_;
    std::vector<char> pinned_;  // vector<bool>'un bit vekili yerine düz bayt
    std::vector<LayoutPoint> displacement_;
    // (ızgara hücresi, düğüm); adımlar arası yeniden kullanılır.
    std::vector<std::pair<std::uint64_t, std::size_t>> cells_;
    float temperature_ = 0.0F;
    std::size_t iterations_done_ = 0;
};

}  // namespace noteapp::app
