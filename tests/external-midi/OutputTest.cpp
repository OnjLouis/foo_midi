#include "../../Players/ExternalMIDI/Output.h"
#include <iostream>

using namespace ExternalMIDI;

static std::vector<std::wstring> Names { L"Other port", L"Test output" };
static std::vector<DWORD> Messages;
static UINT Opened = UINT(-1);
static unsigned Opens = 0, Closes = 0, Prepared = 0, Released = 0, Resets = 0;
static bool ChangeName = false, Complete = true;
static MIDIHDR * Pending = nullptr;
static std::vector<uint8_t> Expected;

static void Check(bool Value, const char * Name)
{
    if (!Value) throw std::runtime_error(Name);
}

extern "C"
{
    UINT WINAPI midiOutGetNumDevs() { return static_cast<UINT>(Names.size()); }
    MMRESULT WINAPI midiOutGetDevCapsW(UINT_PTR Id, LPMIDIOUTCAPSW Caps, UINT)
    {
        const bool Handle = Id == 123;
        const auto Index = Handle ? Opened : static_cast<UINT>(Id);
        if (Index >= Names.size()) return MMSYSERR_BADDEVICEID;
        const auto & Name = Handle && ChangeName ? Names[0] : Names[Index];
        wcsncpy_s(Caps->szPname, Name.c_str(), _TRUNCATE);
        return MMSYSERR_NOERROR;
    }
    MMRESULT WINAPI midiOutOpen(LPHMIDIOUT Handle, UINT Id, DWORD_PTR, DWORD_PTR, DWORD)
    {
        Opened = Id;
        ++Opens;
        *Handle = reinterpret_cast<HMIDIOUT>(123);
        return MMSYSERR_NOERROR;
    }
    MMRESULT WINAPI midiOutClose(HMIDIOUT)
    {
        Check(Pending == nullptr, "close must not abandon live header");
        ++Closes;
        return MMSYSERR_NOERROR;
    }
    MMRESULT WINAPI midiOutShortMsg(HMIDIOUT, DWORD Message)
    {
        Messages.push_back(Message);
        return MMSYSERR_NOERROR;
    }
    MMRESULT WINAPI midiOutGetErrorTextA(MMRESULT, LPSTR Text, UINT Size)
    {
        strncpy_s(Text, Size, "Test error", _TRUNCATE);
        return MMSYSERR_NOERROR;
    }
    MMRESULT WINAPI midiOutPrepareHeader(HMIDIOUT, LPMIDIHDR Header, UINT Size)
    {
        Check(Size == sizeof(MIDIHDR), "header size");
        Check(Header->dwBufferLength == Expected.size(), "SysEx byte length, not event count");
        Check(std::vector<uint8_t>(Header->lpData, Header->lpData + Header->dwBufferLength) == Expected, "SysEx payload before prepare");
        Pending = Header;
        ++Prepared;
        return MMSYSERR_NOERROR;
    }
    MMRESULT WINAPI midiOutLongMsg(HMIDIOUT, LPMIDIHDR Header, UINT)
    {
        Check(Header == Pending, "prepared header remains stable");
        Header->dwFlags = Complete ? MHDR_DONE : MHDR_INQUEUE;
        return MMSYSERR_NOERROR;
    }
    MMRESULT WINAPI midiOutUnprepareHeader(HMIDIOUT, LPMIDIHDR Header, UINT)
    {
        Check(Header == Pending, "release original header");
        Check((Header->dwFlags & MHDR_DONE) != 0, "never unprepare a running SysEx");
        Check(std::vector<uint8_t>(Header->lpData, Header->lpData + Header->dwBufferLength) == Expected, "payload survives until return");
        Pending = nullptr;
        ++Released;
        return MMSYSERR_NOERROR;
    }
    MMRESULT WINAPI midiOutReset(HMIDIOUT)
    {
        if (Pending) Pending->dwFlags = MHDR_DONE;
        ++Resets;
        return MMSYSERR_NOERROR;
    }
}

int main()
{
    try
    {
        {
            Output Port;
            Port.Open(L"Test output");
            Check(Opened == 1 && Messages.empty(), "open only explicitly selected device");
            Check(Port.Send({0, 0x01643C90, {}}, [] { return false; }), "short send");
            Check(Messages.size() == 1 && Messages[0] == 0x00643C90, "strip internal port byte");
            Expected = {0xF0, 0x43, 0x10, 0x4C, 0, 0, 0x7E, 0, 0xF7};
            Check(Port.Send({0, 0, Expected}, [] { return false; }), "completed SysEx");
            Check(Prepared == 1 && Released == 1 && !Pending, "completed buffer released exactly once");
            Complete = false;
            unsigned Checks = 0;
            Check(!Port.Send({0, 0, Expected}, [&] { return ++Checks > 2; }), "cancel incomplete SysEx");
            Check(Prepared == 2 && Released == 2 && !Pending && Resets > 0, "cancel resets and safely releases");
            Port.Silence();
            Check(Messages.size() == 49, "panic sends sustain off and all sound/note off on 16 channels");
        }
        Check(Opens == 1 && Closes == 1, "normal handle released");
        Names.erase(Names.begin());
        Messages.clear();
        const auto ResetBeforeUnusedClose = Resets;
        {
            Output Port;
            Port.Open(L"Test output");
            Check(Opened == 0, "device renumbering resolved by name");
        }
        Check(Messages.empty() && Resets == ResetBeforeUnusedClose, "unused verified output closes without sending MIDI");
        Names = { L"Other port" };
        bool Rejected = false;
        const auto Before = Opens;
        try { Output Port; Port.Open(L"Test output"); } catch (...) { Rejected = true; }
        Check(Rejected && Opens == Before, "missing device does not open default");
        Names = {L"Test output", L"Test output"};
        Rejected = false;
        try { Output Port; Port.Open(L"Test output"); } catch (...) { Rejected = true; }
        Check(Rejected && Opens == Before, "ambiguous names fail closed");
        Names = {L"Other port", L"Test output"};
        ChangeName = true;
        Messages.clear();
        const auto ResetBefore = Resets;
        Rejected = false;
        try { Output Port; Port.Open(L"Test output"); } catch (...) { Rejected = true; }
        Check(Rejected && Messages.empty() && Resets == ResetBefore, "hotplug during open sends no cleanup to wrong device");
        std::cout << "External MIDI output ownership regressions passed (mock driver, no MIDI sent)\n";
        return 0;
    }
    catch (const std::exception & Error)
    {
        std::cerr << Error.what() << '\n';
        return 1;
    }
}
