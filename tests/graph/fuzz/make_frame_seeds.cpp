// fuzz_frame'in başlangıç derlemi: her işlem türü ve değer tipi için geçerli çerçeve. Mutasyon biçime buradan
// girer.
#include "graph/op.hpp"

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

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fputs("usage: make_frame_seeds DIR\n", stderr);
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
    return 0;
}
