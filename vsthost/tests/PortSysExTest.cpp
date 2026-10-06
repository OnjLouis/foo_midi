#include "../../Players/PortSysEx.h"
#include <iostream>
#include <stdexcept>

void Require(bool ok, const char * what)
{
    if (!ok) throw std::runtime_error(what);
}
int main()
{
    try
    {
        const std::vector<uint8_t> ports{0,2,1};
        auto route = [&](std::vector<uint8_t> bytes, unsigned port = 0, unsigned capacity = 4)
        { return PortSysEx::Route(bytes.data(), bytes.size(), port, ports, capacity); };
        auto part = route({0xF0,0x43,0x10,0x4C,8,27,7,3,0xF7});
        Require(part.size() == 1 && part[0].Port == 2 && part[0].Data[5] == 11, "MU part 28 must reach B12");
        auto receive = route({0xF0,0x43,0x10,0x4C,8,27,4,27,0xF7});
        Require(receive[0].Data[7] == 11, "MU receive channel must be local");
        auto reset = route({0xF0,0x43,0x10,0x4C,0,0,0x7E,0,0xF7});
        Require(reset.size() == 3, "XG reset must reach all declared ports");
        auto local = route({0xF0,0x43,0x10,0x4C,8,11,7,3,0xF7}, 2);
        Require(local.size() == 1 && local[0].Port == 2 && local[0].Data[5] == 11, "Local B part must stay on B");
        auto insert = route({0xF0,0x43,0x10,0x4C,3,1,0x0C,27,0xF7});
        Require(insert.size() == 3 && insert[2].Data[7] == 11 && insert[0].Data[7] == 0x7F, "Insertion assignment leaks across ports");
        auto variation = route({0xF0,0x43,0x10,0x4C,2,1,0x5A,0,27,0xF7});
        Require(variation.size() == 3 && variation[2].Data[8] == 11 && variation[0].Data[8] == 0x7F, "Packed variation assignment wrong");
        std::vector<uint8_t> gs{0xF0,0x41,0x10,0x42,0x12,0x50,0x19,0x15,2,0,0xF7};
        gs[9] = (128 - ((0x50 + 0x19 + 0x15 + 2) & 127)) & 127;
        auto block = route(gs);
        Require(block.size() == 1 && block[0].Port == 2 && block[0].Data[5] == 0x40, "GS B address not rebased");
        unsigned sum = 0;
        for (size_t i = 5; i + 1 < block[0].Data.size(); ++i) sum += block[0].Data[i];
        Require((sum & 127) == 0, "GS checksum wrong");
        auto gsLocal = route(block[0].Data, 2);
        Require(gsLocal.size() == 1 && gsLocal[0].Port == 2, "GS part message was broadcast");
        gs[9] ^= 1;
        auto malformed = route(gs);
        Require(malformed.size() == 1 && malformed[0].Port == 0 && malformed[0].Data == gs, "Invalid checksum rewritten");
        auto global = route({0xF0,0x41,0x10,0x42,0x12,0,0,0x7F,0,1,0xF7});
        Require(global.size() == 3, "GS reset not broadcast");
        Require(route({0xF0,0x7E,0x7F,9,1,0xF7}).size() == 3, "GM reset not broadcast");
        Require(route({0xF0,0x7D,1,2,3,0xF7}).size() == 1, "Unknown SysEx broadcast");
        Require(route({0xF0,0x43,0x10,0x4C,8,27,7,3,0xF7},0,2).empty(), "Unsupported port aliased");
        Require(route({0xF0,0x7D,1,2,3,0xF7},4).empty(), "Invalid source port accepted");
        std::cout << "Port SysEx regression tests passed\n";
        return 0;
    }
    catch (const std::exception & e) { std::cerr << e.what() << '\n'; return 1; }
}
