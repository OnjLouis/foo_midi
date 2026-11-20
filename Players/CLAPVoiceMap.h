#pragma once

#include "MUVoiceMap.h"
#include "../Infrastructure/CLAPMidiInputs.h"

namespace CLAP
{
inline void QueueMUVoiceMap(InputEvents & events, MUVoiceMap::Mode mode, uint8_t inputCount, uint32_t time)
{
    const auto * message = MUVoiceMap::Message(mode);
    // The CLAP inputs belong to one synth, so send the device-wide map on input A only.
    if (inputCount && message)
        events.Add(message->data(), static_cast<uint32_t>(message->size()), 0, time);
}

inline bool QueueSysEx(InputEvents & events, const uint8_t * data, size_t size, uint32_t port,
    const std::vector<uint8_t> & filePorts, uint8_t inputCount, uint32_t time, MUVoiceMap::Mode mode)
{
    const auto input = MidiInputIndex(port, filePorts, inputCount);
    if (!input || !data || size == 0 || size > UINT32_MAX)
        return false;
    events.Add(data, static_cast<uint32_t>(size), *input, time);
    if (mode != MUVoiceMap::Mode::None && MUVoiceMap::IsSystemReset(data, size))
        QueueMUVoiceMap(events, mode, inputCount, time);
    return true;
}
}
