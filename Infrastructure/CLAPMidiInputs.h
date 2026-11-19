#pragma once

#include <clap/clap.h>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

namespace CLAP
{
// The shared MIDI player tracks up to eight logical ports at loop boundaries.
inline constexpr uint8_t MaxMidiInputs = 8;

inline uint8_t MidiInputPortCount(const clap_plugin_t * plugIn) noexcept
{
    if (!plugIn || !plugIn->get_extension) return 0;
    const auto * ports = static_cast<const clap_plugin_note_ports_t *>(plugIn->get_extension(plugIn, CLAP_EXT_NOTE_PORTS));
    if (!ports || !ports->count || !ports->get) return 0;

    const auto count = ports->count(plugIn, true);
    if (count == 0 || count > MaxMidiInputs) return 0;

    for (uint32_t index = 0; index < count; ++index)
    {
        clap_note_port_info_t info {};
        if (!ports->get(plugIn, index, true, &info) || !(info.supported_dialects & CLAP_NOTE_DIALECT_MIDI))
            return 0;
    }
    return static_cast<uint8_t>(count);
}

inline std::optional<uint16_t> MidiInputIndex(uint32_t logical, const std::vector<uint8_t> & filePorts, uint8_t count) noexcept
{
    // Preserve the original first logical input for one-input instruments.
    if (count == 1) return logical == 0 ? std::optional<uint16_t>{0} : std::nullopt;
    if (logical >= filePorts.size()) return std::nullopt;

    // The parser compacts file port numbers; CLAP bus A/B/C/D is not compacted.
    const auto physical = filePorts[logical];
    if (physical >= count) return std::nullopt;
    return physical;
}

class InputEvents : public clap_input_events_t
{
public:
    InputEvents() noexcept
    {
        ctx = this;
        size = [](const clap_input_events_t * self) -> uint32_t
        {
            return static_cast<uint32_t>(static_cast<const InputEvents *>(self->ctx)->_Events.size());
        };
        get = [](const clap_input_events_t * self, uint32_t index) -> const clap_event_header_t *
        {
            const auto & events = static_cast<const InputEvents *>(self->ctx)->_Events;
            if (index >= events.size()) return nullptr;
            return std::visit([](const auto & event) -> const clap_event_header_t *
            {
                if constexpr (std::is_same_v<std::decay_t<decltype(event)>, SysEx>)
                    return &event.Event.header;
                else
                    return &event.header;
            }, events[index]);
        };
    }

    InputEvents(const InputEvents &) = delete;
    InputEvents & operator=(const InputEvents &) = delete;

    void Add(uint8_t status, uint8_t data1, uint8_t data2, uint16_t port, uint32_t time)
    {
        _Events.emplace_back(clap_event_midi_t{
            {sizeof(clap_event_midi_t), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0},
            port, {status, data1, data2}
        });
    }

    void Add(const uint8_t * data, uint32_t length, uint16_t port, uint32_t time)
    {
        _Events.emplace_back(std::in_place_type<SysEx>, data, length, port, time);
    }

    void Clear() noexcept { _Events.clear(); }

private:
    struct SysEx
    {
        std::vector<uint8_t> Buffer;
        clap_event_midi_sysex_t Event;

        SysEx(const uint8_t * data, uint32_t length, uint16_t port, uint32_t time)
            : Buffer(data, data + length), Event{
                {sizeof(clap_event_midi_sysex_t), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0},
                port, Buffer.data(), length}
        { }

        SysEx(const SysEx &) = delete;
        SysEx & operator=(const SysEx &) = delete;
        SysEx(SysEx &&) noexcept = default;
        SysEx & operator=(SysEx &&) noexcept = default;
    };

    std::vector<std::variant<clap_event_midi_t, SysEx>> _Events;
};
}
