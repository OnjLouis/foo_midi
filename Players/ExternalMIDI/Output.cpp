#include "Output.h"
#include <chrono>
#include <thread>

namespace ExternalMIDI
{
    std::atomic<bool> Output::_DriverFailed{false};

    static void Check(MMRESULT Result)
    {
        if (Result == MMSYSERR_NOERROR) return;
        char Text[MAXERRORLENGTH]{};
        midiOutGetErrorTextA(Result, Text, MAXERRORLENGTH);
        throw std::runtime_error(std::string("MIDI output: ") + Text);
    }

    std::vector<Device> EnumerateDevices()
    {
        std::vector<Device> Devices;
        const UINT Count = midiOutGetNumDevs();
        for (UINT Index = 0; Index < Count; ++Index)
        {
            MIDIOUTCAPSW Caps{};
            if (midiOutGetDevCapsW(Index, &Caps, sizeof(Caps)) == MMSYSERR_NOERROR)
                Devices.push_back({ Index, Caps.szPname });
        }
        return Devices;
    }

    Output::~Output()
    {
        try { Close(); }
        catch (...) { Quarantine(); }
    }

    void Output::Open(const std::wstring & Name)
    {
        if (_DriverFailed) throw std::runtime_error("MIDI driver did not release a buffer. Restart foobar2000 before retrying hardware output.");
        if (Name.empty()) throw std::runtime_error("No MIDI output selected.");
        const auto Devices = EnumerateDevices();
        const Device * Found = nullptr;
        for (const auto & Item : Devices)
        {
            if (Item.Name != Name) continue;
            if (Found) throw std::runtime_error("Multiple MIDI outputs have the selected name. Give them unique names before using external MIDI output.");
            Found = &Item;
        }
        if (!Found) throw std::runtime_error("The selected MIDI output is disconnected or unavailable. No other port was used.");
        _Completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!_Completed) throw std::runtime_error("Cannot create MIDI completion event.");
        Check(midiOutOpen(&_Handle, Found->Id, reinterpret_cast<DWORD_PTR>(_Completed), 0, CALLBACK_EVENT));
        MIDIOUTCAPSW Caps{};
        Check(midiOutGetDevCapsW(reinterpret_cast<UINT_PTR>(_Handle), &Caps, sizeof(Caps)));
        if (Name != Caps.szPname) throw std::runtime_error("MIDI output changed during connection. No messages were sent.");
        _Verified = true;
    }

    bool Output::Send(const Event & Item, const std::function<bool()> & Cancelled)
    {
        if (Cancelled()) return false;
        if (Item.SysEx.empty())
        {
            Check(midiOutShortMsg(_Handle, Item.Message & 0x00FFFFFFu));
            return true;
        }
        if (Item.SysEx.size() > 16 * 1024 * 1024)
            throw std::runtime_error("External MIDI SysEx exceeds the 16 MiB safety limit.");

        _Pending = std::make_unique<LongMessage>();
        _Pending->Data = Item.SysEx;
        auto & Header = _Pending->Header;
        Header.lpData = reinterpret_cast<LPSTR>(_Pending->Data.data());
        Header.dwBufferLength = static_cast<DWORD>(_Pending->Data.size());
        const auto Prepared = midiOutPrepareHeader(_Handle, &Header, sizeof(Header));
        if (Prepared != MMSYSERR_NOERROR)
        {
            _Pending.reset();
            Check(Prepared);
        }
        try
        {
            Check(midiOutLongMsg(_Handle, &Header, sizeof(Header)));
            // WinMM owns both the header and payload until completion. It may
            // also block inside LongMsg; never call this from Foobar's main thread.
            const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while ((Header.dwFlags & MHDR_DONE) == 0)
            {
                if (Cancelled() || std::chrono::steady_clock::now() >= Deadline)
                {
                    Check(midiOutReset(_Handle));
                    ReleaseMessage();
                    if (Cancelled()) return false;
                    throw std::runtime_error("MIDI output timed out sending SysEx.");
                }
                WaitForSingleObject(_Completed, 5);
            }
            ReleaseMessage();
        }
        catch (...)
        {
            if (_Pending)
            {
                midiOutReset(_Handle);
                try { ReleaseMessage(); } catch (...) { Quarantine(); }
            }
            throw;
        }
        return !Cancelled();
    }

    void Output::ReleaseMessage()
    {
        if (!_Pending) return;
        for (unsigned Attempt = 0; Attempt < 100; ++Attempt)
        {
            const auto Result = midiOutUnprepareHeader(_Handle, &_Pending->Header, sizeof(MIDIHDR));
            if (Result == MMSYSERR_NOERROR)
            {
                _Pending.reset();
                return;
            }
            if (Result != MIDIERR_STILLPLAYING) Check(Result);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error("MIDI driver did not release its SysEx buffer.");
    }

    void Output::Silence()
    {
        if (!_Handle) return;
        Check(midiOutReset(_Handle));
        ReleaseMessage();
        for (unsigned Channel = 0; Channel < 16; ++Channel)
        {
            Check(midiOutShortMsg(_Handle, 0xB0u | Channel | (64u << 8)));
            Check(midiOutShortMsg(_Handle, 0xB0u | Channel | (120u << 8)));
            Check(midiOutShortMsg(_Handle, 0xB0u | Channel | (123u << 8)));
        }
    }

    void Output::Close()
    {
        if (_Handle)
        {
            if (_Verified) Silence();
            Check(midiOutClose(_Handle));
            _Handle = nullptr;
            _Verified = false;
        }
        if (_Completed)
        {
            CloseHandle(_Completed);
            _Completed = nullptr;
        }
    }

    void Output::Quarantine() noexcept
    {
        // A broken driver may still own this memory and event. Preserve them
        // until process exit rather than freeing live driver pointers. Disable
        // further opens so a driver fault cannot accumulate unbounded buffers.
        _DriverFailed = true;
        (void)_Pending.release();
        _Handle = nullptr;
        _Completed = nullptr;
    }
}
