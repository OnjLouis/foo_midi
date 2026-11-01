#pragma once

#include "PortSysEx.h"
#include <array>

namespace MUVoiceMap
{
enum class Mode { None = 0, Basic = 1, Native = 2 };

inline Mode FromConfig(int64_t value) noexcept
{
    return value == 1 ? Mode::Basic : value == 2 ? Mode::Native : Mode::None;
}

inline constexpr std::array<uint8_t, 9> BasicMessage{0xF0,0x43,0x10,0x49,0,0,0x12,0,0xF7};
inline constexpr std::array<uint8_t, 9> NativeMessage{0xF0,0x43,0x10,0x49,0,0,0x12,1,0xF7};

inline const std::array<uint8_t, 9> * Message(Mode mode) noexcept
{
    switch (mode)
    {
        case Mode::Basic: return &BasicMessage;
        case Mode::Native: return &NativeMessage;
        default: return nullptr;
    }
}

inline bool IsSystemReset(const std::vector<uint8_t> & data) noexcept
{
    if (data.size() < 6 || data.front() != 0xF0 || data.back() != 0xF7)
        return false;

    if (data.size() == 6)
        return data[1] == 0x7E && data[2] < 0x80 && data[3] == 9 && (data[4] == 1 || data[4] == 3);

    if (data.size() == 9)
        return data[1] == 0x43 && (data[2] & 0xF0) == 0x10 && data[3] == 0x4C &&
            data[4] == 0 && data[5] == 0 && data[6] == 0x7E && data[7] == 0;

    if (data.size() == 11)
        return data[1] == 0x41 && data[2] < 0x80 && data[3] == 0x42 && data[4] == 0x12 &&
            data[5] == 0x40 && data[6] == 0 && data[7] == 0x7F && data[8] == 0 && data[9] == 0x41;

    return false;
}

// A reset may restore the instrument's default map. Reapply only on each
// actual reset destination, at the same timestamp, without another mode reset.
inline std::vector<PortSysEx::Packet> Route(const uint8_t * data, size_t size, uint32_t source,
    const std::vector<uint8_t> & ports, uint32_t capacity, Mode mode)
{
    auto packets = PortSysEx::Route(data, size, source, ports, capacity);
    const auto * message = Message(mode);
    if (message == nullptr)
        return packets;

    std::vector<PortSysEx::Packet> result;
    result.reserve(packets.size() * 2);
    for (const auto & packet : packets)
    {
        result.push_back(packet);
        if (IsSystemReset(packet.Data))
            result.push_back({packet.Port, {message->begin(), message->end()}});
    }
    return result;
}
}
