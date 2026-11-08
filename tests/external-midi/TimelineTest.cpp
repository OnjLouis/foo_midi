#include "../../Players/ExternalMIDI/Timeline.h"
#include <iostream>
#include <limits>

using namespace ExternalMIDI;

static void Check(bool Value, const char * Name)
{
    if (!Value) throw std::runtime_error(Name);
}

int main()
{
    try
    {
        Sequence Song { "test.mid", 0, L"Test output", 3, {
            {0, 0x000041B0, {}},
            {0, 0x000001B0, {}},
            {0, 0x000005C0, {}},
            {0, 0x00643C90, {}},
            {1, 0, {0xF0, 0x43, 0x10, 0x4C, 0x08, 0, 1, 65, 0xF7}},
            {2, 0x00003C80, {}}
        }};
        Song.Validate();
        Timeline Player(Song);
        std::vector<Event> Sent;
        auto Send = [&](const Event & Item) { Sent.push_back(Item); return true; };
        Check(Sent.empty(), "construction sends nothing");
        Player.DispatchThrough(0, Send);
        Check(Sent.size() == 4, "events at zero retain order");
        Player.DispatchThrough(0.5, Send);
        Check(Sent.size() == 4, "future events not emitted");
        Player.DispatchThrough(1, Send);
        Check(Sent.size() == 5 && !Sent.back().SysEx.empty(), "mid-song SysEx");
        auto State = Player.Seek(1.5);
        Check(State.size() == 5 && State.back().Message == 0x00643C90, "seek restores held note after ordered state");
        Check(State[3].SysEx == Song.Events[4].SysEx, "seek retains custom dump");
        Sent.clear();
        Player.DispatchThrough(1.99, Send);
        Check(Sent.empty(), "seek does not repeat prior events");
        Player.DispatchThrough(2, Send);
        Check(Sent.size() == 1 && Sent[0].Message == 0x00003C80, "seek retains future note-off");
        State = Player.Seek(2.5);
        Check(State.size() == 4, "finished notes not resurrected");
        State = Player.Seek(0);
        Check(State.empty(), "backward seek clears cursor");
        Sent.clear();
        Player.DispatchThrough(0, [](const Event &) { return false; });
        Player.DispatchThrough(0, Send);
        Check(Sent.size() == 4, "cancel leaves cursor at unsent event");

        Sequence SustainSong { "pedal.mid", 0, L"Test output", 3, {
            {0, 0x007F40B0, {}}, {0, 0x00643C90, {}}, {1, 0x00003C80, {}}, {2, 0x000040B0, {}}
        }};
        Timeline Pedal(SustainSong);
        State = Pedal.Seek(1.5);
        Check(State.size() == 3 && (State[1].Message & 0xF0) == 0x90 && (State[2].Message & 0xF0) == 0x80, "sustained note restored without permanent hold");
        Check(Pedal.Seek(2.5).size() == 2, "released pedal drops sustained notes");
        SustainSong.Events.insert(SustainSong.Events.begin() + 3, {1.2, 0, {0xF0, 0x7E, 0x7F, 9, 1, 0xF7}});
        Timeline Reset(SustainSong);
        Check(Reset.Seek(1.5).size() == 2, "reset clears chased active notes");

        Sequence Dual { "dual.mid", 0, L"Primary", 3, {
            {0, 0x000005C0, {}, 8},
            {0, 0x00643C90, {}, 8},
            {0, 0x00000AC0, {}, 3},
            {0, 0x007F40B0, {}, 3},
            {0, 0x00503C90, {}, 3},
            {1, 0x00003C80, {}, 3},
            {1, 0, {0xF0, 0x7E, 0x7F, 9, 1, 0xF7}, 8}
        }, L"Secondary" };
        Dual.RoutePorts();
        Dual.Validate();
        Check(Dual.Events[0].Port == 1 && Dual.Events[2].Port == 0, "ports map by logical number, not event order");
        Timeline DualPlayer(Dual);
        State = DualPlayer.Seek(2);
        Check(State.size() == 6 && State[3].Port == 1 && State[3].SysEx == Dual.Events.back().SysEx,
            "seek preserves port and bytes for a reset");
        Check(State[4].Port == 0 && State[4].Message == 0x00503C90 &&
            State[5].Port == 0 && State[5].Message == 0x00003C80,
            "reset on one output does not clear sustained note on the other");
        auto Rejects = [](const Sequence & Candidate)
        {
            try { Candidate.Validate(); } catch (const std::runtime_error &) { return true; }
            return false;
        };
        Dual.SecondaryDevice.clear();
        Check(Rejects(Dual), "dual-port song requires a secondary device");
        Dual.SecondaryDevice = Dual.Device;
        Check(Rejects(Dual), "dual-port song must not merge channels into one device");
        Dual.Events.push_back({2, 0x00643C90, {}, 2});
        bool TooMany = false;
        try { Dual.RoutePorts(); } catch (const std::runtime_error &) { TooMany = true; }
        Check(TooMany, "a third used port is not folded into another device");
        auto Single = Song;
        for (auto & Item : Single.Events) Item.Port = 9;
        Single.RoutePorts();
        Single.Validate();
        Check(!Single.UsesSecondary() && Single.Events[0].Port == 0,
            "a single nonzero logical port keeps using the primary device");

        const std::vector<uint8_t> Mode {0xF0,0x41,0x10,0x42,0x12,0,0,0x7F,0,1,0xF7};
        const std::vector<uint8_t> ResetGS {0xF0,0x41,0x10,0x42,0x12,0x40,0,0x7F,0,0x41,0xF7};
        const std::vector<uint8_t> Chorus {0xF0,0x41,0x10,0x42,0x12,0x40,1,0x38,3,4,0xF7};
        const std::vector<uint8_t> Alternate {0xF0,0x41,0x10,0x42,0x12,0x50,0x1A,0x15,2,0x7F,0xF7};
        const std::vector<uint8_t> Local {0xF0,0x41,0x10,0x42,0x12,0x40,0x1A,0x15,2,0x0F,0xF7};
        auto Bad = Alternate;
        Bad[9] = 0;
        const std::vector<uint8_t> Unknown {0xF0,0x43,0x10,0x4C,0,0,0x7E,0,0xF7};
        Sequence Roland { "roland.mid", 0, L"Primary", 4, {
            {0,0,Mode,0}, {0.1,0,Chorus,0}, {0.2,0,Alternate,0},
            {0.3,0,Local,0}, {0.4,0,Bad,0}, {0.5,0,Unknown,0},
            {1,0x00643C90,{},0}, {1,0x00643C90,{},2}
        }, L"Secondary" };
        auto Raw = Roland;
        Raw.RoutePorts();
        Check(Raw.Events.size() == 8 && Raw.Events[0].SysEx == Mode &&
            Raw.Events[2].SysEx == Alternate && Raw.Events.back().Port == 1, "raw preserves hardware bytes");
        Roland.RoutePorts({0,7,1}, true);
        Roland.Validate();
        Check(Roland.Events.size() == 10, "only shared reset and chorus duplicate");
        Check(Roland.Events[0].SysEx == ResetGS && Roland.Events[0].Port == 0 &&
            Roland.Events[1].SysEx == ResetGS && Roland.Events[1].Port == 1,
            "SC-88 mode set becomes a valid GS reset for both independent outputs");
        Check(Roland.Events[2].SysEx == Chorus && Roland.Events[3].SysEx == Chorus &&
            Roland.Events[3].Seconds == 0.1 && Roland.Events[3].Port == 1, "shared GS effect keeps its timestamp");
        Check(Roland.Events[4].Port == 1 && Roland.Events[4].SysEx == Local,
            "alternate group uses declared raw port and recalculated checksum");
        Check(Roland.Events[5].Port == 0 && Roland.Events[5].SysEx == Local &&
            Roland.Events[6].Port == 0 && Roland.Events[6].SysEx == Bad &&
            Roland.Events[7].Port == 0 && Roland.Events[7].SysEx == Unknown,
            "local writes, invalid checksums and non-GS messages do not leak across ports");
        Sequence SoloGS { "solo.mid", 0, L"Primary", 1, {{0,0,Mode,1}}, L"Secondary" };
        SoloGS.RoutePorts({0,1}, true);
        Check(SoloGS.Events.size() == 1 && SoloGS.Events[0].Port == 0 && SoloGS.Events[0].SysEx == Mode,
            "split mode does not rewrite single-port files");

        Clock Time;
        Check(!Time.IsFresh(0), "clock starts inactive");
        Time.Update(5, 10);
        Check(Time.IsFresh(10.05) && std::abs(Time.Position(10.05) - 5.05) < 0.000001, "clock follows transport anchor");
        Check(!Time.IsFresh(10.101), "stale transport cannot run indefinitely");
        Check(!Time.IsFresh(9), "backward host clock rejected");
        Time.Update(std::numeric_limits<double>::quiet_NaN(), 10);
        Check(!Time.IsFresh(10), "invalid position rejected");
        bool Rejected = false;
        Song.Device.clear();
        try { Song.Validate(); } catch (...) { Rejected = true; }
        Check(Rejected, "no default MIDI mapper fallback");
        std::cout << "External MIDI timeline regressions passed\n";
        return 0;
    }
    catch (const std::exception & Error)
    {
        std::cerr << Error.what() << '\n';
        return 1;
    }
}
