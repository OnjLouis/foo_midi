#include "../../Players/ExternalMIDI/Worker.h"
#include <functional>
#include <iostream>

using namespace ExternalMIDI;

static std::mutex Mutex;
static std::condition_variable Changed;
static std::vector<Event> Messages;
static std::vector<std::wstring> Destinations;
static std::vector<std::wstring> Closed;
static unsigned Opens = 0, Closes = 0, Silences = 0;
static bool FailOpen = false, BlockDump = false, InDump = false;
static bool FailSecondary = false;

static void Check(bool Value, const char * Name)
{
    if (!Value) throw std::runtime_error(Name);
}

static void Wait(const std::function<bool()> & Predicate)
{
    std::unique_lock<std::mutex> Lock(Mutex);
    Check(Changed.wait_for(Lock, std::chrono::seconds(3), Predicate), "worker timed out");
}

class RecordingPort
{
public:
    ~RecordingPort() { Close(); }
    void Open(const std::wstring & Name)
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        if (FailOpen || (FailSecondary && Name == L"Secondary"))
            throw std::runtime_error("Mock port unavailable");
        _Name = Name;
        _Open = true;
        ++Opens;
        Changed.notify_all();
    }
    void Close()
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        if (!_Open) return;
        _Open = false;
        ++Closes;
        Closed.push_back(_Name);
        Changed.notify_all();
    }
    void Silence()
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        ++Silences;
        Changed.notify_all();
    }
    bool Send(const Event & Item, const std::function<bool()> & Cancelled)
    {
        if (Cancelled()) return false;
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            if (!Item.SysEx.empty() && BlockDump)
            {
                InDump = true;
                Changed.notify_all();
            }
        }
        if (!Item.SysEx.empty() && BlockDump)
        {
            const auto Deadline = Now() + 2;
            while (!Cancelled() && Now() < Deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return false;
        }
        if (Cancelled()) return false;
        std::lock_guard<std::mutex> Lock(Mutex);
        Messages.push_back(Item);
        Destinations.push_back(_Name);
        Changed.notify_all();
        return true;
    }

private:
    bool _Open = false;
    std::wstring _Name;
};

int main()
{
    try
    {
        const auto Song = std::make_shared<Sequence>(Sequence{"test.mid", 0, L"Test port", 10, {
            {0, 0x000005C0, {}}, {0, 0x00643C90, {}}, {5, 0x00003C80, {}}
        }, L"Secondary"});
        Worker<RecordingPort> Player;
        Worker<RecordingPort>::Request State{Song, 1, Now() - 1, false, 1};
        Player.Update(State);
        Wait([] { return Silences >= 2; });
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Check(Messages.empty(), "stale initial clock must not chase or sound notes");
        }
        State.Updated = Now();
        Player.Update(State);
        Wait([] { return Messages.size() >= 2; });
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Check(Messages[0].Message == 0x000005C0 && Messages[1].Message == 0x00643C90, "fresh clock restores program before note");
        }
        State.Paused = true;
        ++State.Epoch;
        Player.Update(State);
        Wait([] { return Closes == 1; });
        size_t Count;
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Count = Messages.size();
        }
        State.Position = 6;
        State.Updated = Now();
        Player.Update(State);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Check(Messages.size() == Count && Opens == 1, "single-port playback ignores configured secondary and does not reopen while paused");
        }
        State.Paused = false;
        ++State.Epoch;
        State.Updated = Now();
        Player.Update(State);
        Wait([&] { return Messages.size() > Count; });
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Check(Messages.back().Message == 0x000005C0, "seek beyond note-off does not resurrect note");
            Count = Messages.size();
        }
        State.Song = std::make_shared<Sequence>(Sequence{"next.mid", 0, L"Other port", 2, {
            {0, 0x00000CC0, {}}, {0, 0x00644090, {}}
        }});
        State.Position = 0;
        State.Updated = Now();
        ++State.Epoch;
        Player.Update(State);
        Wait([&] { return Messages.size() >= Count + 2; });
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Check(Closes == 2 && Messages[Count].Message == 0x00000CC0 && Messages[Count + 1].Message == 0x00644090, "track change closes old output before new events");
        }
        Player.Stop();
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Check(Opens == Closes, "shutdown releases all output handles");
            BlockDump = true;
        }
        Worker<RecordingPort> DumpPlayer;
        Worker<RecordingPort>::Request Dump{std::make_shared<Sequence>(Sequence{"dump.mid", 0, L"Test port", 2, {
            {0, 0, {0xF0, 0x7E, 0x7F, 9, 1, 0xF7}}, {0, 0x00644390, {}}
        }}), 0, Now(), false, 1};
        DumpPlayer.Update(Dump);
        Wait([] { return InDump; });
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Count = Messages.size();
        }
        Dump.Paused = true;
        ++Dump.Epoch;
        DumpPlayer.Update(Dump);
        DumpPlayer.Stop();
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Check(Messages.size() == Count && Opens == Closes, "cancelled dump cannot emit following note");
            BlockDump = false;
            FailOpen = true;
        }
        Worker<RecordingPort> ErrorPlayer;
        Dump.Paused = false;
        Dump.Epoch = 42;
        Dump.Updated = Now();
        ErrorPlayer.Update(Dump);
        std::pair<uint64_t, std::string> Error;
        const auto Deadline = Now() + 3;
        do
        {
            Error = ErrorPlayer.TakeError();
            if (!Error.second.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (Now() < Deadline);
        Check(Error.first == 42 && Error.second == "Primary output: Mock port unavailable", "error belongs to exact transport generation");
        ErrorPlayer.Stop();

        auto DualSong = std::make_shared<Sequence>(Sequence{"dual.mid", 0, L"Primary", 8, {
            {0, 0x000005C0, {}, 0},
            {0, 0, {0xF0, 0x7E, 0x7F, 9, 1, 0xF7}, 1},
            {0, 0x00000CC0, {}, 1},
            {0, 0x00643C90, {}, 0},
            {0, 0x00443C90, {}, 1},
            {4, 0x00003C80, {}, 0},
            {5, 0x00003C80, {}, 1}
        }, L"Secondary"});
        DualSong->Validate();
        Worker<RecordingPort> DualPlayer;
        unsigned Before;
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            FailOpen = false;
            Before = Closes;
            Count = Messages.size();
        }
        Worker<RecordingPort>::Request Dual{DualSong, 0, Now(), false, 1};
        DualPlayer.Update(Dual);
        Wait([&] { return Messages.size() >= Count + 5; });
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Check(Destinations[Count] == L"Primary" && Destinations[Count + 1] == L"Secondary" &&
                Messages[Count + 1].SysEx == DualSong->Events[1].SysEx &&
                Destinations[Count + 3] == L"Primary" && Destinations[Count + 4] == L"Secondary",
                "both outputs receive their own note and unmodified SysEx events in order");
        }
        Dual.Paused = true;
        ++Dual.Epoch;
        DualPlayer.Update(Dual);
        Wait([&] { return Closes >= Before + 2; });
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Check(Closed[Before] == L"Primary" && Closed[Before + 1] == L"Secondary", "pause closes both outputs");
            Count = Messages.size();
        }
        Dual.Paused = false;
        Dual.Position = 4.5;
        Dual.Updated = Now();
        ++Dual.Epoch;
        DualPlayer.Update(Dual);
        Wait([&] { return Messages.size() >= Count + 4; });
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Check(Messages.size() == Count + 4 && Messages.back().Message == 0x00443C90 &&
                Destinations.back() == L"Secondary", "seek restores only the output whose note is still held");
        }
        DualPlayer.Stop();

        Worker<RecordingPort> MissingSecondary;
        unsigned SilenceBefore;
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            FailSecondary = true;
            Count = Messages.size();
            Before = Closes;
            SilenceBefore = Silences;
        }
        Dual.Position = 0;
        Dual.Updated = Now();
        ++Dual.Epoch;
        MissingSecondary.Update(Dual);
        Wait([&] { return Closes > Before; });
        MissingSecondary.Stop();
        Error = MissingSecondary.TakeError();
        Check(Error.first == Dual.Epoch && Error.second == "Secondary output: Mock port unavailable",
            "second-device failure identifies which output failed");
        Check(Opens == Closes && Messages.size() == Count && Silences == SilenceBefore,
            "missing secondary aborts without sending on primary or leaking a handle");
        std::cout << "External MIDI worker lifecycle regressions passed (recording sink only)\n";
        return 0;
    }
    catch (const std::exception & Error)
    {
        std::cerr << Error.what() << '\n';
        return 1;
    }
}
