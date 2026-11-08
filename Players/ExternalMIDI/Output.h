#pragma once

#include "Timeline.h"
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <functional>
#include <memory>

namespace ExternalMIDI
{
    struct Device
    {
        UINT Id;
        std::wstring Name;
    };

    std::vector<Device> EnumerateDevices();

    class Output
    {
    public:
        Output() = default;
        Output(const Output &) = delete;
        Output & operator=(const Output &) = delete;
        ~Output();

        void Open(const std::wstring & Name);
        bool Send(const Event & Item, const std::function<bool()> & Cancelled);
        void Silence();
        void Close();

    private:
        struct LongMessage
        {
            MIDIHDR Header{};
            std::vector<uint8_t> Data;
        };
        void ReleaseMessage();
        void Quarantine() noexcept;

        HMIDIOUT _Handle = nullptr;
        HANDLE _Completed = nullptr;
        bool _Verified = false;
        bool _NeedsSilence = false;
        std::unique_ptr<LongMessage> _Pending;
        static std::atomic<bool> _DriverFailed;
    };
}
