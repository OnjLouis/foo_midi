#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace ExternalMIDI
{
    constexpr uint8_t OutputCount = 2;

    struct Event
    {
        double Seconds = 0;
        uint32_t Message = 0;
        std::vector<uint8_t> SysEx;
        uint8_t Port = 0;

        bool IsNote() const noexcept
        {
            const auto Type = Message & 0xF0;
            return SysEx.empty() && (Type == 0x80 || Type == 0x90);
        }
    };

    struct Sequence
    {
        std::string Path;
        unsigned Subsong = 0;
        std::wstring Device;
        double Duration = 0;
        std::vector<Event> Events;
        std::wstring SecondaryDevice;

        // The parser supplies logical port indices. Compress only the used
        // indices, so a single-port subsong always uses the primary device.
        void RoutePorts()
        {
            constexpr int Unused = -1;
            int First = Unused, Second = Unused;
            for (const auto & Item : Events)
            {
                if (Item.Port == First || Item.Port == Second) continue;
                if (First == Unused) First = Item.Port;
                else if (Second == Unused) Second = Item.Port;
                else throw std::runtime_error("This file uses more than two MIDI ports. No messages were sent.");
            }
            if (Second != Unused && Second < First) std::swap(First, Second);
            for (auto & Item : Events) Item.Port = Item.Port == Second ? 1 : 0;
        }

        bool UsesSecondary() const noexcept
        {
            return std::any_of(Events.begin(), Events.end(), [](const Event & Item) { return Item.Port == 1; });
        }

        void Validate() const
        {
            if (Device.empty())
                throw std::runtime_error("Select a primary MIDI output with Configure first.");
            if (UsesSecondary() && SecondaryDevice.empty())
                throw std::runtime_error("This file uses two MIDI ports. Select a secondary MIDI output with Configure. No messages were sent.");
            if (UsesSecondary() && SecondaryDevice == Device)
                throw std::runtime_error("Primary and secondary MIDI outputs must be different.");
            if (!std::isfinite(Duration) || Duration < 0)
                throw std::runtime_error("Invalid external MIDI duration.");
            double Previous = 0;
            for (const auto & Item : Events)
            {
                if (Item.Port >= OutputCount)
                    throw std::runtime_error("Invalid external MIDI output index.");
                if (!std::isfinite(Item.Seconds) || Item.Seconds < Previous || Item.Seconds > Duration)
                    throw std::runtime_error("Invalid external MIDI event time.");
                Previous = Item.Seconds;
                if (!Item.SysEx.empty() && (Item.SysEx.front() != 0xF0 || Item.SysEx.back() != 0xF7))
                    throw std::runtime_error("Incomplete MIDI SysEx message.");
            }
        }
    };

    // This class has no device access. Decoding, prefetch and conversion cannot
    // send MIDI; only the transport worker consumes these explicitly timed events.
    class Timeline
    {
    public:
        explicit Timeline(const Sequence & Song) : _Song(Song) { }

        std::vector<Event> Seek(double Seconds)
        {
            if (!std::isfinite(Seconds) || Seconds < 0)
                throw std::runtime_error("Invalid MIDI seek position.");

            _Next = 0;
            std::vector<Event> State;
            struct PortState
            {
                std::array<std::array<uint8_t, 128>, 16> Held{};
                std::array<std::array<uint8_t, 128>, 16> Sustained{};
                std::array<bool, 16> Sustain{};
            };
            std::array<PortState, OutputCount> Ports{};
            while (_Next < _Song.Events.size() && _Song.Events[_Next].Seconds < Seconds)
            {
                const auto & Item = _Song.Events[_Next++];
                auto & Port = Ports[Item.Port];
                auto & Held = Port.Held;
                auto & Sustained = Port.Sustained;
                auto & Sustain = Port.Sustain;
                const auto Type = Item.Message & 0xF0;
                const auto Channel = Item.Message & 0x0F;
                const auto Key = (Item.Message >> 8) & 0x7F;
                const auto Value = (Item.Message >> 16) & 0x7F;

                // State messages remain in file order, including bank/program,
                // RPN/NRPN and mid-song custom-voice dumps. Never replay old attacks.
                if (!Item.IsNote())
                {
                    State.push_back(Item);
                    if (!Item.SysEx.empty())
                    {
                        if (IsReset(Item.SysEx))
                        {
                            Port = {};
                        }
                    }
                    else if (Type == 0xB0)
                    {
                        if (Key == 64)
                        {
                            Sustain[Channel] = Value >= 64;
                            if (!Sustain[Channel]) Sustained[Channel] = {};
                        }
                        if (Key == 120 || Key == 123)
                        {
                            Held[Channel] = {};
                            Sustained[Channel] = {};
                        }
                        if (Key == 121)
                        {
                            Sustain[Channel] = false;
                            Sustained[Channel] = {};
                        }
                    }
                    continue;
                }
                if (Type == 0x90 && Value != 0)
                {
                    Held[Channel][Key] = static_cast<uint8_t>(Value);
                    Sustained[Channel][Key] = 0;
                }
                else
                {
                    if (Sustain[Channel]) Sustained[Channel][Key] = Held[Channel][Key];
                    Held[Channel][Key] = 0;
                }
            }

            for (uint8_t Port = 0; Port < OutputCount; ++Port)
                for (unsigned Channel = 0; Channel < 16; ++Channel)
                    for (unsigned Key = 0; Key < 128; ++Key)
                    {
                        if (const auto Velocity = Ports[Port].Held[Channel][Key])
                            State.push_back({ Seconds, 0x90u | Channel | (Key << 8) | (Velocity << 16), {}, Port });
                        else if (const auto Velocity = Ports[Port].Sustained[Channel][Key])
                        {
                            State.push_back({ Seconds, 0x90u | Channel | (Key << 8) | (Velocity << 16), {}, Port });
                            State.push_back({ Seconds, 0x80u | Channel | (Key << 8), {}, Port });
                        }
                    }
            return State;
        }

        template<typename Send>
        void DispatchThrough(double Seconds, Send && Output)
        {
            while (_Next < _Song.Events.size() && _Song.Events[_Next].Seconds <= Seconds)
            {
                if (!Output(_Song.Events[_Next])) return;
                ++_Next;
            }
        }

    private:
        static bool IsReset(const std::vector<uint8_t> & Data) noexcept
        {
            if (Data.size() == 6 && Data[1] == 0x7E && Data[3] == 9 && (Data[4] == 1 || Data[4] == 3)) return true;
            if (Data.size() == 9 && Data[1] == 0x43 && Data[3] == 0x4C &&
                Data[4] == 0 && Data[5] == 0 && Data[6] == 0x7E && Data[7] == 0) return true;
            return Data.size() == 11 && Data[1] == 0x41 && Data[3] == 0x42 &&
                Data[4] == 0x12 && Data[5] == 0x40 && Data[6] == 0 && Data[7] == 0x7F && Data[8] == 0 && Data[9] == 0x41;
        }

        const Sequence & _Song;
        size_t _Next = 0;
    };

    class Clock
    {
    public:
        static constexpr double LeaseSeconds = 0.100;

        void Update(double Position, double Now) noexcept
        {
            _Position = Position;
            _Updated = Now;
        }

        bool IsFresh(double Now) const noexcept
        {
            return std::isfinite(_Position) && _Position >= 0 &&
                Now >= _Updated && Now - _Updated <= LeaseSeconds;
        }

        double Position(double Now) const noexcept { return _Position + std::max(0.0, Now - _Updated); }

    private:
        double _Position = -1;
        double _Updated = 0;
    };
}
