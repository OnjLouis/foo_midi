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
    struct Event
    {
        double Seconds = 0;
        uint32_t Message = 0;
        std::vector<uint8_t> SysEx;

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

        void Validate() const
        {
            if (Device.empty())
                throw std::runtime_error("Select a MIDI output with Configure first.");
            if (!std::isfinite(Duration) || Duration < 0)
                throw std::runtime_error("Invalid external MIDI duration.");
            double Previous = 0;
            for (const auto & Item : Events)
            {
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
            std::array<std::array<uint8_t, 128>, 16> Held{};
            std::array<std::array<uint8_t, 128>, 16> Sustained{};
            std::array<bool, 16> Sustain{};
            while (_Next < _Song.Events.size() && _Song.Events[_Next].Seconds < Seconds)
            {
                const auto & Item = _Song.Events[_Next++];
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
                            Held = {};
                            Sustained = {};
                            Sustain = {};
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

            for (unsigned Channel = 0; Channel < 16; ++Channel)
                for (unsigned Key = 0; Key < 128; ++Key)
                {
                    if (const auto Velocity = Held[Channel][Key])
                        State.push_back({ Seconds, 0x90u | Channel | (Key << 8) | (Velocity << 16), {} });
                    else if (const auto Velocity = Sustained[Channel][Key])
                    {
                        State.push_back({ Seconds, 0x90u | Channel | (Key << 8) | (Velocity << 16), {} });
                        State.push_back({ Seconds, 0x80u | Channel | (Key << 8), {} });
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
