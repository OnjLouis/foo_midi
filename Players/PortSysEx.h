#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace PortSysEx
{
constexpr uint8_t XGModel = 0x4C;
constexpr uint8_t GSModel = 0x42;
constexpr uint8_t GSDataSet = 0x12;
constexpr uint8_t MultiPartAddress = 0x08;
constexpr uint8_t ChannelsPerPort = 16;
constexpr uint8_t PartOff = 0x7F;
struct Packet
{
    uint32_t Port;
    std::vector<uint8_t> Data;
};

// Adapt shared-device addresses to independent 16-channel plugin contexts.
// The original SMF and SysEx table remain untouched, including their timestamps.
inline std::vector<Packet> Route(const uint8_t * data, size_t size, uint32_t source,
    const std::vector<uint8_t> & ports, uint32_t capacity)
{
    std::vector<Packet> result;
    if (source >= capacity || size == 0) return result;
    std::vector<uint8_t> bytes(data, data + size);
    auto emit = [&](uint32_t port, const std::vector<uint8_t> & value)
    {
        if (port < capacity) result.push_back({port, value});
    };
    auto target = [&](uint8_t raw) -> uint32_t
    {
        const auto it = std::find(ports.begin(), ports.end(), raw);
        return it == ports.end() ? capacity : static_cast<uint32_t>(it - ports.begin());
    };
    auto broadcast = [&]
    {
        const auto count = std::min(capacity, static_cast<uint32_t>(std::max(size_t(1), ports.size())));
        for (uint32_t port = 0; port < count; ++port) emit(port, bytes);
    };
    auto assignPart = [&](size_t offset)
    {
        const auto part = bytes[offset];
        const auto destination = part == PartOff ? capacity : (part < ChannelsPerPort ? source : target(part / ChannelsPerPort));
        const auto count = std::min(capacity, static_cast<uint32_t>(std::max(size_t(1), ports.size())));
        for (uint32_t port = 0; port < count; ++port)
        {
            auto local = bytes;
            local[offset] = port == destination ? part % ChannelsPerPort : PartOff;
            emit(port, local);
        }
    };
    if (size < 6 || bytes.front() != 0xF0 || bytes.back() != 0xF7)
    {
        emit(source, bytes);
        return result;
    }
    if (std::any_of(bytes.begin() + 1, bytes.end() - 1, [](uint8_t value) { return value >= 0x80; }))
    {
        emit(source, bytes);
        return result;
    }
    // Universal GM/GM2 resets and master volume apply to the logical device.
    if ((bytes[1] == 0x7E && bytes[3] == 9 && size == 6) ||
        (bytes[1] == 0x7F && bytes[3] == 4 && bytes[4] == 1 && size == 8))
    {
        broadcast();
        return result;
    }
    if (size >= 9 && bytes[1] == 0x43 && (bytes[2] & 0xF0) == 0x10 && bytes[3] == XGModel)
    {
        const uint8_t high = bytes[4], middle = bytes[5], low = bytes[6];
        if (high == MultiPartAddress && middle < 64)
        {
            const auto destination = middle < ChannelsPerPort ? source : target(middle / ChannelsPerPort);
            bytes[5] %= ChannelsPerPort;
            // Receive-channel numbers may also use the MU's absolute 0..63 range.
            if (low <= 4 && size - 8 > static_cast<size_t>(4 - low))
            {
                auto & receive = bytes[7 + 4 - low];
                if (receive < 64 && receive / ChannelsPerPort == middle / ChannelsPerPort) receive %= ChannelsPerPort;
            }
            emit(destination, bytes);
        }
        else if (high == 2 && middle == 1 && low <= 0x5B && size - 8 > static_cast<size_t>(0x5B - low))
            assignPart(7 + 0x5B - low);
        else if (high == 3 && low <= 0x0C && size - 8 > static_cast<size_t>(0x0C - low))
            assignPart(7 + 0x0C - low);
        else if (source == 0 && (high == 0 || high == 2 || high == 3 || (high >= 0x30 && high <= 0x33)))
            broadcast();
        else
            emit(source, bytes);
        return result;
    }
    if (size >= 11 && bytes[1] == 0x41 && bytes[3] == GSModel && bytes[4] == GSDataSet)
    {
        unsigned checksum = 0;
        for (size_t i = 5; i + 1 < size; ++i) checksum += bytes[i];
        if ((checksum & 0x7F) == 0)
        {
            if (source == 0 && (bytes[5] == 0x50 || bytes[5] == 0x51))
            {
                bytes[5] -= 0x10;
                bytes[size - 2] = (bytes[size - 2] + 0x10) & 0x7F;
                emit(target(1), bytes);
            }
            else if (source == 0 && ((bytes[5] == 0 && bytes[6] == 0 && bytes[7] == 0x7F) ||
                (bytes[5] == 0x40 && bytes[6] <= 3)))
                broadcast();
            else
                emit(source, bytes);
            return result;
        }
    }
    emit(source, bytes);
    return result;
}
}
