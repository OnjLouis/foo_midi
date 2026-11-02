#include "../../Players/ExternalMIDI/Worker.h"
#include <functional>
#include <iostream>

using namespace ExternalMIDI;

static std::mutex Mutex;
static std::condition_variable Changed;
static std::vector<Event> Messages;
static unsigned Opens = 0, Closes = 0, Silences = 0;
static bool FailOpen = false, BlockDump = false, InDump = false;

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
    void Open(const std::wstring &)
    {
        std::lock_guard<std::mutex> Lock(Mutex);
        if (FailOpen) throw std::runtime_error("Mock port unavailable");
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
        Changed.notify_all();
        return true;
    }

private:
    bool _Open = false;
};

int main()
{
    try
    {
        const auto Song = std::make_shared<Sequence>(Sequence{"test.mid", 0, L"Test port", 10, {
            {0, 0x000005C0, {}}, {0, 0x00643C90, {}}, {5, 0x00003C80, {}}
        }});
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
            Check(Messages.size() == Count && Opens == 1, "paused timer updates do not reopen or send");
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
        Check(Error.first == 42 && Error.second == "Mock port unavailable", "error belongs to exact transport generation");
        ErrorPlayer.Stop();
        std::cout << "External MIDI worker lifecycle regressions passed (recording sink only)\n";
        return 0;
    }
    catch (const std::exception & Error)
    {
        std::cerr << Error.what() << '\n';
        return 1;
    }
}
