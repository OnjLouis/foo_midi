#include "pch.h"
#include "ExternalMIDI.h"
#include "Configuration.h"
#include "Resource.h"
#include "../Players/ExternalMIDI/Output.h"
#include "../Players/ExternalMIDI/Worker.h"
#include <sdk/initquit.h>
#include <sdk/play_callback.h>
#include <sdk/playback_control.h>
#include <sdk/popup_message.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace ExternalMIDI
{
    static std::mutex RegistryMutex;
    static std::deque<std::shared_ptr<const Sequence>> Registry;

    void Register(std::shared_ptr<const Sequence> Song)
    {
        Song->Validate();
        std::lock_guard<std::mutex> Lock(RegistryMutex);
        for (auto It = Registry.begin(); It != Registry.end();)
            if ((*It)->Path == Song->Path && (*It)->Subsong == Song->Subsong) It = Registry.erase(It);
            else ++It;
        Registry.push_back(std::move(Song));
        // Retain prefetched short files after their decoder has been destroyed,
        // without retaining the user's entire playlist for the application lifetime.
        size_t Bytes = 0;
        for (const auto & Entry : Registry)
            for (const auto & Item : Entry->Events) Bytes += sizeof(Item) + Item.SysEx.size();
        while (Registry.size() > 8 || (Registry.size() > 1 && Bytes > 64 * 1024 * 1024))
        {
            for (const auto & Item : Registry.front()->Events) Bytes -= sizeof(Item) + Item.SysEx.size();
            Registry.pop_front();
        }
    }

    static std::shared_ptr<const Sequence> Find(const char * Path, unsigned Subsong)
    {
        std::lock_guard<std::mutex> Lock(RegistryMutex);
        for (auto It = Registry.rbegin(); It != Registry.rend(); ++It)
            if ((*It)->Path == Path && (*It)->Subsong == Subsong) return *It;
        return {};
    }

    class Transport : public play_callback_static
    {
    public:
        unsigned get_flags() override
        {
            return flag_on_playback_starting | flag_on_playback_new_track | flag_on_playback_stop |
                flag_on_playback_pause | flag_on_playback_seek;
        }

        void Initialize() { _Instance = this; }
        void Shutdown()
        {
            Stop();
            _Worker.Stop();
            _Instance = nullptr;
            std::lock_guard<std::mutex> Lock(RegistryMutex);
            Registry.clear();
        }

        void on_playback_starting(play_control::t_track_command, bool Paused) override
        {
            Stop();
            _State.Paused = Paused;
        }

        void on_playback_new_track(metadb_handle_ptr Track) override
        {
            Stop();
            if (static_cast<PlayerType>(CfgPlayerType.get()) != PlayerType::MCI) return;
            _Track = Track;
            _Timer = SetTimer(nullptr, 0, 20, Timer);
            if (!_Timer) console::print("External MIDI: could not start playback clock.");
        }

        void on_playback_stop(play_control::t_stop_reason) override { Stop(); }
        void on_playback_pause(bool Paused) override
        {
            _State.Paused = Paused;
            ++_State.Epoch;
            _State.Updated = Now();
            _Worker.Update(_State);
        }
        void on_playback_seek(double Position) override
        {
            _State.Position = Position;
            _State.Updated = Now();
            ++_State.Epoch;
            _Worker.Update(_State);
        }
        void on_playback_edited(metadb_handle_ptr) override { }
        void on_playback_dynamic_info(const file_info &) override { }
        void on_playback_dynamic_info_track(const file_info &) override { }
        void on_playback_time(double) override { }
        void on_volume_change(float) override { }

    private:
        void Stop()
        {
            if (_Timer) KillTimer(nullptr, _Timer);
            _Timer = 0;
            _Track.release();
            _State.Song.reset();
            ++_State.Epoch;
            _Worker.Update(_State);
        }

        void Tick()
        {
            const auto Error = _Worker.TakeError();
            if (!Error.second.empty())
            {
                console::formatter() << "External MIDI: " << Error.second.c_str();
                if (Error.first == _State.Epoch)
                {
                    playback_control::get()->stop();
                    popup_message::g_show(Error.second.c_str(), "MIDI output");
                    return;
                }
            }
            if (_Track.is_empty()) return;
            // playback_control is main-thread-only and must not be called from
            // playback callbacks. Sample it here; the worker only sees plain data.
            const auto Playback = playback_control::get();
            metadb_handle_ptr Current;
            if (static_cast<PlayerType>(CfgPlayerType.get()) != PlayerType::MCI ||
                !Playback->get_now_playing(Current) || Current != _Track)
            {
                Stop();
                return;
            }
            if (!_State.Song)
            {
                auto Song = Find(_Track->get_path(), _Track->get_subsong_index());
                if (!Song) return;
                _State.Song = std::move(Song);
                ++_State.Epoch;
            }
            const bool Paused = Playback->is_paused();
            if (_State.Paused != Paused) ++_State.Epoch;
            _State.Paused = Paused;
            _State.Position = Playback->playback_get_position();
            _State.Updated = Now();
            _Worker.Update(_State);
        }

        static void CALLBACK Timer(HWND, UINT, UINT_PTR, DWORD) noexcept
        {
            if (!_Instance) return;
            try { _Instance->Tick(); }
            catch (const std::exception & Error)
            {
                console::formatter() << "External MIDI: " << Error.what();
                _Instance->Stop();
            }
            catch (...) { _Instance->Stop(); }
        }

        static Transport * _Instance;
        UINT_PTR _Timer = 0;
        metadb_handle_ptr _Track;
        Worker<Output> _Worker;
        Worker<Output>::Request _State;
    };

    Transport * Transport::_Instance = nullptr;
    static service_factory_single_t<Transport> TransportFactory;

    class Lifetime : public initquit
    {
    public:
        void on_init() override { TransportFactory.get_static_instance().Initialize(); }
        void on_quit() override { TransportFactory.get_static_instance().Shutdown(); }
    };
    static initquit_factory_t<Lifetime> LifetimeFactory;

    class DeviceDialog : public CDialogImpl<DeviceDialog>
    {
    public:
        enum { IDD = IDD_EXTERNAL_MIDI };
        DeviceDialog(const std::wstring & Primary, const std::wstring & Secondary) : Selected{Primary, Secondary} { }
        std::array<std::wstring, OutputCount> Selected;
        BEGIN_MSG_MAP(DeviceDialog)
            MESSAGE_HANDLER(WM_INITDIALOG, Init)
            COMMAND_ID_HANDLER(IDOK, Accept)
            COMMAND_ID_HANDLER(IDCANCEL, Cancel)
        END_MSG_MAP()

    private:
        LRESULT Init(UINT, WPARAM, LPARAM, BOOL &)
        {
            const auto Devices = EnumerateDevices();
            Populate(IDC_EXTERNAL_MIDI_DEVICE, 0, Devices);
            Populate(IDC_EXTERNAL_MIDI_SECONDARY, 1, Devices);
            CenterWindow(GetParent());
            return TRUE;
        }

        void Populate(int Control, size_t Port, const std::vector<Device> & Devices)
        {
            auto Combo = GetDlgItem(Control);
            auto & Choices = _Choices[Port];
            LRESULT Selection = CB_ERR;
            const auto Add = [&](const std::wstring & Name, const std::wstring & Label)
            {
                const auto Index = Combo.SendMessage(CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Label.c_str()));
                if (Index < 0) return;
                Choices.push_back(Name);
                if (Name == Selected[Port]) Selection = Index;
            };
            if (Port == 1) Add(L"", L"None");
            for (const auto & Item : Devices) Add(Item.Name, Item.Name);
            if (Selection == CB_ERR && !Selected[Port].empty())
                Add(Selected[Port], Selected[Port] + L" (unavailable)");
            if (Selection != CB_ERR) Combo.SendMessage(CB_SETCURSEL, Selection);
        }

        LRESULT Accept(WORD, WORD, HWND, BOOL &)
        {
            const auto Primary = static_cast<size_t>(GetDlgItem(IDC_EXTERNAL_MIDI_DEVICE).SendMessage(CB_GETCURSEL));
            const auto Secondary = static_cast<size_t>(GetDlgItem(IDC_EXTERNAL_MIDI_SECONDARY).SendMessage(CB_GETCURSEL));
            if (Primary >= _Choices[0].size() || Secondary >= _Choices[1].size())
            {
                ::MessageBoxW(m_hWnd, L"Select a primary MIDI output and a secondary output or None.", L"MIDI output", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (_Choices[0][Primary] == _Choices[1][Secondary])
            {
                ::MessageBoxW(m_hWnd, L"Primary and secondary MIDI outputs must be different.", L"MIDI output", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            Selected = {_Choices[0][Primary], _Choices[1][Secondary]};
            EndDialog(IDOK);
            return 0;
        }
        LRESULT Cancel(WORD, WORD, HWND, BOOL &) { EndDialog(IDCANCEL); return 0; }
        std::array<std::vector<std::wstring>, OutputCount> _Choices;
    };

    bool Configure(HWND Parent, std::wstring & Primary, std::wstring & Secondary)
    {
        DeviceDialog Dialog(Primary, Secondary);
        if (Dialog.DoModal(Parent) != IDOK) return false;
        Primary = std::move(Dialog.Selected[0]);
        Secondary = std::move(Dialog.Selected[1]);
        return true;
    }
}
