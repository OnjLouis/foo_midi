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
        explicit DeviceDialog(std::wstring Name) : Selected(std::move(Name)) { }
        std::wstring Selected;
        BEGIN_MSG_MAP(DeviceDialog)
            MESSAGE_HANDLER(WM_INITDIALOG, Init)
            COMMAND_ID_HANDLER(IDOK, Accept)
            COMMAND_ID_HANDLER(IDCANCEL, Cancel)
        END_MSG_MAP()

    private:
        LRESULT Init(UINT, WPARAM, LPARAM, BOOL &)
        {
            _Devices = EnumerateDevices();
            auto Combo = GetDlgItem(IDC_EXTERNAL_MIDI_DEVICE);
            int Selection = -1;
            for (const auto & Item : _Devices)
            {
                const auto Index = Combo.SendMessage(CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Item.Name.c_str()));
                if (Item.Name == Selected) Selection = static_cast<int>(Index);
            }
            if (Selection >= 0) Combo.SendMessage(CB_SETCURSEL, Selection);
            ::SetDlgItemTextW(m_hWnd, IDC_EXTERNAL_MIDI_CURRENT, Selected.empty() ? L"No output selected." : Selected.c_str());
            CenterWindow(GetParent());
            return TRUE;
        }
        LRESULT Accept(WORD, WORD, HWND, BOOL &)
        {
            const auto Index = static_cast<size_t>(GetDlgItem(IDC_EXTERNAL_MIDI_DEVICE).SendMessage(CB_GETCURSEL));
            if (Index >= _Devices.size())
            {
                ::MessageBoxW(m_hWnd, L"Select an available MIDI output. If your device is missing, reconnect it and reopen this dialog.", L"MIDI output", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            Selected = _Devices[Index].Name;
            EndDialog(IDOK);
            return 0;
        }
        LRESULT Cancel(WORD, WORD, HWND, BOOL &) { EndDialog(IDCANCEL); return 0; }
        std::vector<Device> _Devices;
    };

    bool Configure(HWND Parent, std::wstring & Device)
    {
        DeviceDialog Dialog(Device);
        if (Dialog.DoModal(Parent) != IDOK) return false;
        Device = std::move(Dialog.Selected);
        return true;
    }
}
