#pragma once

#include "Timeline.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace ExternalMIDI
{
    inline double Now() noexcept
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Port is a WinMM output in the component and a recording sink in tests.
    // Only this worker sends MIDI; it owns every output for the transport epoch.
    template<typename Port>
    class Worker
    {
    public:
        struct Request
        {
            std::shared_ptr<const Sequence> Song;
            double Position = 0;
            double Updated = 0;
            bool Paused = true;
            uint64_t Epoch = 0;
        };

        ~Worker() { Stop(); }

        void Update(Request State)
        {
            std::lock_guard<std::mutex> Lock(_Mutex);
            if (_Quit) return;
            _Request = std::move(State);
            _Epoch = _Request.Epoch;
            if (!_Thread.joinable() && _Request.Song)
                _Thread = std::thread([this] { Run(); });
            _Changed.notify_one();
        }

        void Stop()
        {
            {
                std::lock_guard<std::mutex> Lock(_Mutex);
                _Quit = true;
                ++_Epoch;
                _Changed.notify_one();
            }
            if (_Thread.joinable()) _Thread.join();
        }

        std::pair<uint64_t, std::string> TakeError()
        {
            std::lock_guard<std::mutex> Lock(_Mutex);
            std::pair<uint64_t, std::string> Result{_ErrorEpoch, {}};
            Result.second.swap(_Error);
            return Result;
        }

    private:
        Request Snapshot()
        {
            std::lock_guard<std::mutex> Lock(_Mutex);
            return _Request;
        }

        void Run() noexcept
        {
            uint64_t ActiveEpoch = ~uint64_t(0);
            std::array<std::unique_ptr<Port>, OutputCount> Outputs;
            std::unique_ptr<Timeline> Events;
            std::shared_ptr<const Sequence> Song;
            bool Stale = false;
            while (!_Quit)
            {
                auto State = Snapshot();
                const auto Cancelled = [this, Epoch = State.Epoch] { return _Quit || _Epoch != Epoch; };
                const auto Send = [&](const Event & Item) { return Outputs[Item.Port]->Send(Item, Cancelled); };
                const auto Silence = [&]
                {
                    for (const auto & Output : Outputs)
                        if (Output) Output->Silence();
                };
                try
                {
                    if (State.Epoch != ActiveEpoch)
                    {
                        for (auto & Output : Outputs)
                            if (Output) Output->Close();
                        Outputs = {};
                        Events.reset();
                        Song = State.Song;
                        ActiveEpoch = State.Epoch;
                        Stale = false;
                        if (Song && !State.Paused)
                        {
                            const auto Open = [&](uint8_t Index, const std::wstring & Name)
                            {
                                Outputs[Index] = std::make_unique<Port>();
                                try { Outputs[Index]->Open(Name); }
                                catch (const std::exception & Error)
                                {
                                    throw std::runtime_error(std::string(Index == 0 ? "Primary output: " : "Secondary output: ") + Error.what());
                                }
                            };
                            if (!Cancelled()) Open(0, Song->Device);
                            if (!Cancelled() && Song->UsesSecondary()) Open(1, Song->SecondaryDevice);
                            if (!Cancelled())
                            {
                                // Do not send even startup cleanup until every required device is open.
                                Silence();
                                Events = std::make_unique<Timeline>(*Song);
                                Clock Initial;
                                Initial.Update(State.Position, State.Updated);
                                if (Initial.IsFresh(Now()))
                                    for (const auto & Item : Events->Seek(State.Position))
                                        if (!Send(Item)) break;
                            }
                        }
                    }
                    if (Events && !Cancelled())
                    {
                        State = Snapshot();
                        Clock Time;
                        Time.Update(State.Position, State.Updated);
                        if (!Time.IsFresh(Now()))
                        {
                            if (!Stale) Silence();
                            Stale = true;
                        }
                        else
                        {
                            if (Stale)
                            {
                                for (const auto & Item : Events->Seek(State.Position))
                                    if (!Send(Item)) break;
                                Stale = false;
                            }
                            Events->DispatchThrough(Time.Position(Now()), Send);
                        }
                    }
                }
                catch (const std::exception & Error)
                {
                    Events.reset();
                    Outputs = {};
                    std::lock_guard<std::mutex> Lock(_Mutex);
                    _Error = Error.what();
                    _ErrorEpoch = ActiveEpoch;
                }
                catch (...)
                {
                    Events.reset();
                    Outputs = {};
                    std::lock_guard<std::mutex> Lock(_Mutex);
                    _Error = "Unexpected failure in external MIDI output.";
                    _ErrorEpoch = ActiveEpoch;
                }
                std::unique_lock<std::mutex> Lock(_Mutex);
                if (!Events || State.Paused)
                    _Changed.wait(Lock, [&] { return _Quit || _Epoch != ActiveEpoch; });
                else
                    _Changed.wait_for(Lock, std::chrono::milliseconds(1));
            }
        }

        std::mutex _Mutex;
        std::condition_variable _Changed;
        Request _Request;
        std::atomic<uint64_t> _Epoch{0};
        std::atomic<bool> _Quit{false};
        std::thread _Thread;
        std::string _Error;
        uint64_t _ErrorEpoch = 0;
    };
}
