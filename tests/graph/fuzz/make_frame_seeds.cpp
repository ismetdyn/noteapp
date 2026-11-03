// fuzz_frame'in başlangıç derlemi: her işlem türü ve değer tipi için geçerli çerçeve. Mutasyon biçime buradan
// girer. İkinci dizin verilirse fuzz_log'unki de: v1 ve v2 (parçalı, yarım) günlükler.
#include "graph/op.hpp"
#include "graph/oplog.hpp"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace noteapp::graph;
using namespace noteapp::graph::detail;
namespace fs = std::filesystem;

namespace {

NodeId node(std::byte seed) {
    NodeId id;
    id.bytes.fill(seed);
    return id;
}

EdgeId edge(std::byte seed) {
    EdgeId id;
    id.bytes.fill(seed);
    return id;
}

void write_seed(const fs::path& dir, int index, const Frame& frame) {
    std::ofstream(dir / ("frame" + std::to_string(index)), std::ios::binary) << encode_frame(frame);
}

void write_log_seeds(const fs::path& dir, const Frame& all, const Frame& single) {
    fs::create_directories(dir);
    const DeviceId device{std::byte{7}};
    const std::string whole = encode_frame(all);
    const std::string small = encode_frame(single);
    std::ofstream(dir / "log-v1", std::ios::binary)
        << encode_log_header(device) + encode_log_frame(small) + encode_log_frame(whole);
    std::string parts = encode_log_header(device, kLogVersionParts) + encode_log_part(1, 0, true, small);
    constexpr std::size_t kPieces = 4;
    const std::size_t piece = whole.size() / kPieces + 1;
    for (std::size_t index = 0; index * piece < whole.size(); ++index)
        parts +=
            encode_log_part(all.seq, static_cast<std::uint32_t>(index), (index + 1) * piece >= whole.size(),
                            std::string_view(whole).substr(index * piece, piece));
    std::ofstream(dir / "log-v2", std::ios::binary) << parts;
    std::ofstream(dir / "log-v2-unfinished", std::ios::binary)
        << encode_log_header(device, kLogVersionParts) + encode_log_part(1, 0, false, small.substr(0, 10));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::fputs("usage: make_frame_seeds DIR [LOG_DIR]\n", stderr);
        return 2;
    }
    const fs::path dir = argv[1];
    fs::create_directories(dir);

    const NodeId a = node(std::byte{1});
    const NodeId b = node(std::byte{2});
    const EdgeId e = edge(std::byte{3});
    Frame all;
    all.seq = 7;
    all.hlc = 1'700'000'000'000ULL << 16U;
    all.deps.push_back(Dot{DeviceId{}, 3});
    all.ops.emplace_back(NodePut{a});
    all.ops.emplace_back(NodeDel{b});
    all.ops.emplace_back(LabelAdd{a, "Etiket"});
    all.ops.emplace_back(LabelDel{a, "L"});
    all.ops.emplace_back(EdgePut{e, a, "T", b});
    all.ops.emplace_back(EdgeDel{e});
    all.ops.emplace_back(PropSet{a, "null", Value{}});
    all.ops.emplace_back(PropSet{a, "bool", true});
    all.ops.emplace_back(PropSet{a, "int", std::int64_t{-5}});
    all.ops.emplace_back(PropSet{a, "double", 1.5});
    all.ops.emplace_back(PropSet{e, "text", std::string("yazı")});
    all.ops.emplace_back(PropSet{a, "bytes", Bytes{std::string("\xFF\x00", 2)}});
    all.ops.emplace_back(PropSet{a, "time", Timestamp{1000}});
    write_seed(dir, 0, all);

    int index = 1;
    for (const Op& op : all.ops) {
        Frame single;
        single.seq = 1;
        single.ops.push_back(op);
        write_seed(dir, index++, single);
    }
    Frame empty;
    empty.seq = 1;
    write_seed(dir, index, empty);
    if (argc == 3) {
        Frame single;
        single.seq = 1;
        single.ops.emplace_back(NodePut{a});
        all.seq = 2;
        write_log_seeds(argv[2], all, single);
    }
    return 0;
}
