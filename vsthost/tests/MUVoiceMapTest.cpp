#include "../../Players/MUVoiceMap.h"
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
        using MUVoiceMap::Mode;
        const std::vector<uint8_t> ports{0,2,1,3};
        const std::vector<uint8_t> basic{0xF0,0x43,0x10,0x49,0,0,0x12,0,0xF7};
        const std::vector<uint8_t> native{0xF0,0x43,0x10,0x49,0,0,0x12,1,0xF7};
        auto route = [&](const std::vector<uint8_t> & data, Mode mode, unsigned source = 0)
        { return MUVoiceMap::Route(data.data(), data.size(), source, ports, 4, mode); };
        Require(MUVoiceMap::FromConfig(-1) == Mode::None && MUVoiceMap::FromConfig(3) == Mode::None &&
            MUVoiceMap::FromConfig(0x100000001LL) == Mode::None, "Invalid configuration must fail to no injection");
        Require(MUVoiceMap::FromConfig(0) == Mode::None && MUVoiceMap::FromConfig(1) == Mode::Basic &&
            MUVoiceMap::FromConfig(2) == Mode::Native, "Saved settings changed meaning");
        Require(MUVoiceMap::Message(Mode::None) == nullptr, "Default must not inject a map");
        Require(MUVoiceMap::Message(static_cast<Mode>(99)) == nullptr, "Invalid mode injects a map");
        for (const auto mode : {Mode::Basic, Mode::Native})
        {
            const auto & expected = mode == Mode::Basic ? basic : native;
            const auto * message = MUVoiceMap::Message(mode);
            Require(message && std::equal(message->begin(), message->end(), expected.begin()), "Incorrect MU map bytes");
            auto startup = route(expected, mode);
            Require(startup.size() == 4, "Startup map must reach exactly four contexts");
            for (unsigned i = 0; i < 4; ++i)
                Require(startup[i].Port == i && startup[i].Data == expected, "Startup map destination changed");
            const std::vector<std::vector<uint8_t>> resets{
                {0xF0,0x7E,0x7F,9,1,0xF7}, {0xF0,0x7E,0x7F,9,3,0xF7},
                {0xF0,0x43,0x10,0x4C,0,0,0x7E,0,0xF7},
                {0xF0,0x41,0x10,0x42,0x12,0x40,0,0x7F,0,0x41,0xF7}};
            for (const auto & reset : resets)
            {
                auto unchanged = route(reset, Mode::None);
                Require(unchanged.size() == 4, "Disabled option changed reset routing");
                auto changed = route(reset, mode);
                Require(changed.size() == 8, "Reset must reapply the map exactly once per destination");
                for (unsigned i = 0; i < 4; ++i)
                {
                    Require(unchanged[i].Data == reset, "Original reset modified");
                    Require(changed[2*i].Data == reset && changed[2*i].Port == i &&
                        changed[2*i+1].Data == expected && changed[2*i+1].Port == i, "Reset/map ordering or destination incorrect");
                }
            }
            auto explicitMap = route(mode == Mode::Basic ? native : basic, mode);
            Require(explicitMap.size() == 4 && explicitMap[0].Data != expected,
                "Explicit map in MIDI must not be replaced by the preference");
        }
        const std::vector<uint8_t> localReset{0xF0,0x43,0x10,0x4C,0,0,0x7E,0,0xF7};
        auto local = route(localReset, Mode::Native, 2);
        Require(local.size() == 2 && local[0].Port == 2 && local[1].Port == 2, "Local reset must not change other ports");
        for (const auto & data : std::vector<std::vector<uint8_t>>{
            {}, {0xF0}, {0xF0,0x43,0x10,0x4C,0,0,0x7E,0},
            {0xF0,0x7E,0x7F,9,2,0xF7},
            {0xF0,0x43,0x10,0x4C,8,0,7,3,0xF7},
            {0xF0,0x41,0x10,0x42,0x12,0x40,0,0x7F,0,0x40,0xF7}})
        {
            Require(!MUVoiceMap::IsSystemReset(data), "Malformed/unknown/local SysEx treated as reset");
            auto baseline = route(data, Mode::None);
            auto candidate = route(data, Mode::Basic);
            Require(baseline.size() == candidate.size(), "Non-reset got an injected message");
            for (size_t i = 0; i < baseline.size(); ++i)
                Require(baseline[i].Port == candidate[i].Port && baseline[i].Data == candidate[i].Data,
                    "Non-reset message changed");
        }
        Require(route(basic, Mode::Basic, 4).empty(), "Unsupported source must not alias an active port");
        std::cout << "MU voice-map regression tests passed\n";
        return 0;
    }
    catch (const std::exception & e) { std::cerr << e.what() << '\n'; return 1; }
}
