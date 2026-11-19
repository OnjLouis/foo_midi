#pragma once

#include <cstddef>
#include <cstdint>
#include <MIDI.h>

namespace MIDIChannelMute
{
template <std::size_t N, typename Send>
void SendAllNotesOff(std::size_t logicalPortCount, const uint16_t (&enabled)[N], Send send)
{
    for (std::size_t port = 0; port < logicalPortCount && port < N; ++port)
    {
        for (uint32_t channel = 0; channel < 16; ++channel)
        {
            if (enabled[port] & (1u << channel)) continue;
            send((uint32_t(port) << 24) | (midi::Controller::AllNotesOff << 8) | midi::ControlChange | channel);
        }
    }
}
}
