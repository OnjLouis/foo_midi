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

    // Port is a real WinMM output in the component and a recording sink in tests.
    // Only this worker owns it; the UI and decoder never call into a MIDI driver.
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
            std::unique_ptr<Port> Output;
            std::unique_ptr<Timeline> Events;
            std::shared_ptr<const Sequence> Song;
            bool Stale = false;
            while (!_Quit)
            {
                auto State = Snapshot();
                const auto Cancelled = [this, Epoch = State.Epoch] { return _Quit || _Epoch != Epoch; };
                try
                {
                    if (State.Epoch != ActiveEpoch)
                    {
                        if (Output) Output->Close();
                        Output.reset();
                        Events.reset();
                        Song = State.Song;
                        ActiveEpoch = State.Epoch;
                        Stale = false;
                        if (Song && !State.Paused)
                        {
                            Output = std::make_unique<Port>();
                            Output->Open(Song->Device);
                            Output->Silence();
                            Events = std::make_unique<Timeline>(*Song);
                            Clock Initial;
                            Initial.Update(State.Position, State.Updated);
                            if (Initial.IsFresh(Now()))
                                for (const auto & Item : Events->Seek(State.Position))
                                    if (!Output->Send(Item, Cancelled)) break;
                        }
                    }
                    if (Output && Events && !Cancelled())
                    {
                        State = Snapshot();
                        Clock Time;
                        Time.Update(State.Position, State.Updated);
                        if (!Time.IsFresh(Now()))
                        {
                            if (!Stale) Output->Silence();
                            Stale = true;
                        }
                        else
                        {
                            if (Stale)
                            {
                                for (const auto & Item : Events->Seek(State.Position))
                                    if (!Output->Send(Item, Cancelled)) break;
                                Stale = false;
                            }
                            Events->DispatchThrough(Time.Position(Now()), [&](const Event & Item) { return Output->Send(Item, Cancelled); });
                        }
                    }
                }
                catch (const std::exception & Error)
                {
                    Events.reset();
                    Output.reset();
                    std::lock_guard<std::mutex> Lock(_Mutex);
                    _Error = Error.what();
                    _ErrorEpoch = State.Epoch;
                }
                catch (...)
                {
                    Events.reset();
                    Output.reset();
                    std::lock_guard<std::mutex> Lock(_Mutex);
                    _Error = "Unexpected failure in external MIDI output.";
                    _ErrorEpoch = State.Epoch;
                }
                std::unique_lock<std::mutex> Lock(_Mutex);
                if (!Output || State.Paused)
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
