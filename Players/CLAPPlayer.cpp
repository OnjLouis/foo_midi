
/** $VER: CLAPPlayer.cpp (2025.08.02) P. Stuer - Wrapper for CLAP plugins **/

#include "pch.h"

#include "CLAPPlayer.h"
#include "CLAPPlugIn.h"

#include "Resource.h"
#include "Encoding.h"
#include "Exception.h"
#include "Log.h"

#pragma region player_t

CLAPPlayer::CLAPPlayer() noexcept : player_t(), _Samples32 {}, _Samples64 {},
    _Outputs32 { _Samples32[0], _Samples32[1] }, _Outputs64 { _Samples64[0], _Samples64[1] }, _AudioOut {}
{
}

CLAPPlayer::~CLAPPlayer()
{
    Shutdown();
}

bool CLAPPlayer::Startup()
{
    if (_IsStarted)
        return true;

    fb2k::inMainThreadSynchronous2([this]
    {
        auto PlugIn = _CLAPHost.CreatePlugIn();
        if (!PlugIn)
            return;

        if (!PlugIn->Initialize() || !PlugIn->Activate(_SampleRate))
        {
            PlugIn->Terminate();
            return;
        }
        _PlugIn = std::move(PlugIn);
    });

    if (!_PlugIn)
    {
        _ErrorMessage = "Failed to initialize or activate the CLAP plug-in";
        return false;
    }

    _AudioOut = {};
    _AudioOut.channel_count = _countof(_Outputs32);
    if (_PlugIn->Prefers64bitAudio())
        _AudioOut.data64 = _Outputs64;
    else
        _AudioOut.data32 = _Outputs32;

    if (!_PlugIn->StartProcessing())
    {
        _ErrorMessage = "The CLAP plug-in could not start processing";
        Shutdown();
        return false;
    }

    _IsStarted = true;

    Configure(_MIDIFlavor, _FilterEffects);
    Reset();

    return true;
}

void CLAPPlayer::Shutdown()
{
    _IsStarted = false;
    _InEvents.Clear();
    auto PlugIn = std::move(_PlugIn);
    if (!PlugIn)
        return;

    PlugIn->StopProcessing();
    auto Cleanup = [PlugIn]
    {
        PlugIn->Deactivate();

        PlugIn->Terminate();
    };

    fb2k::inMainThread(Cleanup);
}

void CLAPPlayer::Render(audio_sample * dstFrames, uint32_t dstCount)
{
    ::memset(dstFrames, 0, ((size_t) dstCount * _countof(_Outputs32)) * sizeof(audio_sample));

    try
    {
        const int64_t SteadyTimeNotAvailable = -1;

        clap_process_t Processor =
        {
            .steady_time         = SteadyTimeNotAvailable,
            .frames_count        = dstCount,
            .transport           = nullptr,
            .audio_inputs        = nullptr,
            .audio_outputs       = &_AudioOut,
            .audio_inputs_count  = 0,
            .audio_outputs_count = 1,
            .in_events           = &_InEvents,
            .out_events          = &_OutEvents,
        };

        _AudioOut.constant_mask = 0;
        const auto Processed = _PlugIn->Process(Processor);
        _InEvents.Clear();
        if (!Processed)
            return;

        if (_AudioOut.data32 != nullptr)
        {
            const float * _LChannel = _Outputs32[0];
            const float * _RChannel = _Outputs32[1];

            for (size_t j = 0; j < dstCount; ++j)
            {
                dstFrames[j * 2 + 0] = (audio_sample) _LChannel[j];
                dstFrames[j * 2 + 1] = (audio_sample) _RChannel[j];
            }
        }
        else
        {
            const double * _LChannel = _Outputs64[0];
            const double * _RChannel = _Outputs64[1];

            for (size_t j = 0; j < dstCount; ++j)
            {
                dstFrames[j * 2 + 0] = (audio_sample) _LChannel[j];
                dstFrames[j * 2 + 1] = (audio_sample) _RChannel[j];
            }
        }
    }
    catch (...) { }

    _InEvents.Clear();
}

/// <summary>
/// Resets the player.
/// </summary>
bool CLAPPlayer::Reset()
{
    _InEvents.Clear();
    const auto Count = GetPortCount();
    for (size_t Port = 0; Port < _PortNumbers.size(); ++Port)
        if (CLAP::MidiInputIndex(static_cast<uint32_t>(Port), _PortNumbers, Count))
            ResetPort(static_cast<uint8_t>(Port), 0);

    return true;
}

void CLAPPlayer::SendEvent(uint32_t data, uint32_t time)
{
    auto Status     = (uint8_t) (data);
    auto Data1      = (uint8_t) (data >>  8);
    auto Data2      = (uint8_t) (data >> 16);
    auto PortNumber = (uint8_t) (data >> 24);
    const auto Input = CLAP::MidiInputIndex(PortNumber, _PortNumbers, GetPortCount());
    if (!Input)
        return;

    Log.AtTrace().Write(STR_COMPONENT_BASENAME " is sending MIDI message %8u: %02X %02X %02X.", time, Status, Data1, Data2);

    _InEvents.Add(Status, Data1, Data2, *Input, time);
}

void CLAPPlayer::SendSysEx(const uint8_t * data, size_t size, uint32_t portNumber, uint32_t time)
{
    const auto Input = CLAP::MidiInputIndex(portNumber, _PortNumbers, GetPortCount());
    if (!Input || !data || size == 0 || size > UINT32_MAX)
        return;
#ifdef _DEBUG
    std::string Buffer; Buffer.resize((size * 3) + 1); // 3 characters + terminator

    char * p = Buffer.data();

    for (size_t i = 0; i < size; ++i)
    {
        ::sprintf_s(p, 4, "%02X ", data[i]);
        p += 3;
    }

    Log.AtTrace().Write(STR_COMPONENT_BASENAME " is sending MIDI SysEx message %8u: %s.", time, Buffer.c_str());
#endif

    _InEvents.Add(data, static_cast<uint32_t>(size), *Input, time);
};
