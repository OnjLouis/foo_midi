#include "CLAPMidiInputs.h"
#include "MIDIChannelMute.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
void Require(bool condition, const char * message)
{
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

struct FakePorts
{
    uint32_t Count = 4;
    uint32_t Missing = UINT32_MAX;
    uint32_t NotMIDI = UINT32_MAX;
};

const clap_plugin_note_ports_t Ports {
    [](const clap_plugin_t * plugIn, bool input) -> uint32_t
    {
        return input ? static_cast<const FakePorts *>(plugIn->plugin_data)->Count : 0;
    },
    [](const clap_plugin_t * plugIn, uint32_t index, bool input, clap_note_port_info_t * info) -> bool
    {
        const auto & fixture = *static_cast<const FakePorts *>(plugIn->plugin_data);
        if (!input || index == fixture.Missing || index >= fixture.Count) return false;
        info->id = 100 + index * 10; // Events must use indices, never these IDs.
        info->supported_dialects = index == fixture.NotMIDI ? CLAP_NOTE_DIALECT_CLAP : CLAP_NOTE_DIALECT_MIDI;
        return true;
    }
};

clap_plugin_t MakePlugin(FakePorts & fixture)
{
    clap_plugin_t plugIn {};
    plugIn.plugin_data = &fixture;
    plugIn.get_extension = [](const clap_plugin_t *, const char * id) -> const void *
    {
        return std::string_view(id) == CLAP_EXT_NOTE_PORTS ? &Ports : nullptr;
    };
    return plugIn;
}
}

int main()
{
    FakePorts fixture;
    auto plugIn = MakePlugin(fixture);
    Require(CLAP::MidiInputPortCount(&plugIn) == 4, "Four MIDI ports must be accepted");
    fixture.Count = 5;
    Require(CLAP::MidiInputPortCount(&plugIn) == 5, "Four parts plus a board input must be accepted");
    Require(CLAP::MidiInputIndex(1, {0, 4}, 5) == 4, "Sparse board input must retain its port number");
    fixture.Count = 1;
    Require(CLAP::MidiInputPortCount(&plugIn) == 1, "One MIDI port must still work");
    fixture.Count = 0;
    Require(CLAP::MidiInputPortCount(&plugIn) == 0, "Zero-port plug-in was accepted");
    fixture.Count = 4; fixture.Missing = 2;
    Require(CLAP::MidiInputPortCount(&plugIn) == 0, "Failed port query was ignored");
    fixture.Missing = UINT32_MAX; fixture.NotMIDI = 3;
    Require(CLAP::MidiInputPortCount(&plugIn) == 0, "Unsupported MIDI dialect was accepted");
    fixture.NotMIDI = UINT32_MAX; fixture.Count = CLAP::MaxMidiInputs + 1;
    Require(CLAP::MidiInputPortCount(&plugIn) == 0, "Unbounded port count was accepted");
    plugIn.get_extension = nullptr;
    Require(CLAP::MidiInputPortCount(&plugIn) == 0, "Missing extension was accepted");

    const std::vector<uint8_t> layout{0, 3, 2, 1};
    CLAP::InputEvents events;
    for (uint32_t i = 0; i < 4; ++i)
    {
        const auto dest = CLAP::MidiInputIndex(i, layout, 4);
        Require(dest && *dest == layout[i], "File port did not reach the corresponding input");
        events.Add(0x90, static_cast<uint8_t>(60 + i), 100, *dest, i * 2);
        std::array<uint8_t, 4> packet{0xF0, 0x7D, static_cast<uint8_t>(i), 0xF7};
        events.Add(packet.data(), static_cast<uint32_t>(packet.size()), *dest, i * 2 + 1);
        packet.fill(0);
    }
    Require(CLAP::MidiInputIndex(0, {0}, 1) == 0, "Conventional single-port mapping changed");
    Require(CLAP::MidiInputIndex(0, {3}, 1) == 0, "First logical port should play through one-input synths");
    Require(!CLAP::MidiInputIndex(1, {0, 3}, 3), "Absent input must not be folded into another port");
    Require(!CLAP::MidiInputIndex(3, {0, 3}, 4), "Undeclared port was routed");

    const std::vector<uint8_t> sparsePorts{0, 3};
    uint16_t enabled[128];
    for (auto & mask : enabled) mask = UINT16_MAX;
    enabled[1] &= ~(1u << 2);
    std::vector<uint16_t> muted;
    MIDIChannelMute::SendAllNotesOff(sparsePorts.size(), enabled, [&](uint32_t event)
    {
        const auto input = CLAP::MidiInputIndex(event >> 24, sparsePorts, 4);
        Require(input && (event & 0xFFFFu) == 0x7BB2u, "Unexpected channel mode message");
        muted.push_back(*input);
    });
    Require(muted == std::vector<uint16_t>{3}, "Muting the second logical stream must silence physical input D");

    Require(events.size(&events) == 8, "Events dropped or duplicated");
    for (uint32_t i = 0; i < 4; ++i)
    {
        const auto * midi = reinterpret_cast<const clap_event_midi_t *>(events.get(&events, i * 2));
        const auto * sysex = reinterpret_cast<const clap_event_midi_sysex_t *>(events.get(&events, i * 2 + 1));
        Require(midi->header.type == CLAP_EVENT_MIDI && midi->header.size == sizeof(*midi), "Invalid MIDI event header");
        Require(midi->port_index == layout[i] && midi->header.time == i * 2 && midi->data[1] == 60 + i, "MIDI event changed");
        Require(sysex->header.type == CLAP_EVENT_MIDI_SYSEX && sysex->header.size == sizeof(*sysex), "Invalid SysEx event header");
        Require(sysex->port_index == layout[i] && sysex->header.time == i * 2 + 1, "SysEx route or time changed");
        Require(sysex->size == 4 && sysex->buffer[0] == 0xF0 && sysex->buffer[2] == i && sysex->buffer[3] == 0xF7,
            "SysEx data must survive the caller's buffer and event queue growth");
    }
    events.Clear();
    Require(events.size(&events) == 0 && events.get(&events, 0) == nullptr, "Stale events survived a clear");
}
