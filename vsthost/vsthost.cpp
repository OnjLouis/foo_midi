#include "stdafx.h"
#include "MIDIPorts.h"

#include <pluginterfaces/vst2.x/aeffect.h>
#include <pluginterfaces/vst2.x/aeffectx.h>

typedef AEffect * (VSTCALLBACK * main_func)(audioMasterCallback audioMaster);

// #define LOG_EXCHANGE

#ifdef LOG_EXCHANGE
unsigned exchange_count = 0;
#endif

enum
{
    BUFFER_SIZE = 4096
};
using VSTHost::MaxMIDIPorts;

bool need_idle = false;
bool idle_started = false;

static char * _DirectoryPath = nullptr;

static HANDLE _hFileNUL = nullptr;
static HANDLE _hPipeIn = nullptr;
static HANDLE _hPipeOut = nullptr;

#pragma pack(push, 8)
#pragma warning(disable: 4820) // x bytes padding added after data member

struct vst_event_t
{
    struct vst_event_t * next;

    unsigned port;

    union
    {
        VstMidiEvent midiEvent;
        VstMidiSysexEvent sysexEvent;
    } ev;
}* _EventsHead = nullptr, * _EventsTail = nullptr;

#pragma warning(default: 4820) // x bytes padding added after data member
#pragma pack(pop)

void FreeEvents()
{
    vst_event_t * Event = _EventsHead;

    while (Event)
    {
        vst_event_t * Next = Event->next;

        if (Event->ev.sysexEvent.type == kVstSysExType)
            ::free(Event->ev.sysexEvent.sysexDump);

        ::free(Event);

        Event = Next;
    }

    _EventsHead = nullptr;
    _EventsTail = nullptr;
}

void WriteBytes(const void * data, uint32_t size)
{
    DWORD BytesWritten;

    ::WriteFile(_hPipeOut, data, size, &BytesWritten, NULL);

#ifdef LOG_EXCHANGE
    TCHAR logfile[MAX_PATH];
    _stprintf_s(logfile, _T("C:\\temp\\log\\bytes_%08u.out"), exchange_count++);
    FILE * f = _tfopen(logfile, _T("wb"));
    fwrite(out, 1, size, f);
    fclose(f);
#endif
}

void WriteCode(uint32_t code)
{
    WriteBytes(&code, sizeof(code));
}

void ReadBytes(void * in, uint32_t size)
{
    DWORD BytesRead;

    if (!::ReadFile(_hPipeIn, in, size, &BytesRead, NULL) || (BytesRead < size))
    {
        memset(in, 0, size);

    #ifdef LOG_EXCHANGE
        TCHAR logfile[MAX_PATH];
        _stprintf_s(logfile, _T("C:\\temp\\log\\bytes_%08u.err"), exchange_count++);
        FILE * f = _tfopen(logfile, _T("wb"));
        _ftprintf(f, _T("Wanted %u bytes, got %u"), size, BytesRead);
        fclose(f);
    #endif
    }
    else
    {
    #ifdef LOG_EXCHANGE
        TCHAR logfile[MAX_PATH];
        _stprintf_s(logfile, _T("C:\\temp\\log\\bytes_%08u.in"), exchange_count++);
        FILE * f = _tfopen(logfile, _T("wb"));
        fwrite(in, 1, size, f);
        fclose(f);
    #endif
    }
}

uint32_t ReadCode()
{
    uint32_t code;

    ReadBytes(&code, sizeof(code));

    return code;
}

void ReadChunk(AEffect * effect, std::vector<uint8_t> & out)
{
    out.resize(0);

    uint32_t unique_id = (uint32_t) effect->uniqueID;

    append_be(out, unique_id);

    bool type_chunked = !!(effect->flags & effFlagsProgramChunks);

    append_be(out, type_chunked);

    if (!type_chunked)
    {
        uint32_t num_params = (uint32_t) effect->numParams;

        append_be(out, num_params);

        for (uint32_t i = 0; i < num_params; ++i)
        {
            float parameter = effect->getParameter(effect, (VstInt32) i);

            append_be(out, parameter);
        }
    }
    else
    {
        void * chunk;

        uint32_t size = (uint32_t) effect->dispatcher(effect, effGetChunk, 0, 0, &chunk, 0);

        append_be(out, size);

        size_t chunk_size = out.size();

        out.resize(chunk_size + size);

        memcpy(&out[chunk_size], chunk, size);
    }
}

void setChunk(AEffect * pEffect, std::vector<uint8_t> const & in)
{
    uint32_t size = (uint32_t) in.size();

    if (pEffect == nullptr || size == 0)
        return;

    const uint8_t * inc = in.data();

    uint32_t effect_id;

    retrieve_be(effect_id, inc, size);

    if (effect_id != (uint32_t) pEffect->uniqueID)
        return;

    bool type_chunked;

    retrieve_be(type_chunked, inc, size);

    if (type_chunked != !!(pEffect->flags & effFlagsProgramChunks))
        return;

    if (!type_chunked)
    {
        uint32_t num_params;

        retrieve_be(num_params, inc, size);

        if (num_params != (uint32_t) pEffect->numParams)
            return;

        for (uint32_t i = 0; i < num_params; ++i)
        {
            float parameter;

            retrieve_be(parameter, inc, size);

            pEffect->setParameter(pEffect, (VstInt32) i, parameter);
        }
    }
    else
    {
        uint32_t chunk_size;

        retrieve_be(chunk_size, inc, size);

        if (chunk_size > size)
            return;

        pEffect->dispatcher(pEffect, effSetChunk, 0, (VstIntPtr) chunk_size, (void *) inc, 0);
    }
}

struct MyDLGTEMPLATE : DLGTEMPLATE
{
    WORD ext[3];

    MyDLGTEMPLATE()
    {
        memset(this, 0, sizeof(*this));
    };
};

INT_PTR CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM, LPARAM lParam) noexcept
{
    AEffect * effect;

    switch (msg)
    {
        case WM_INITDIALOG:
        {
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, lParam);

            effect = (AEffect *) lParam;

            ::SetWindowTextW(hwnd, L"VST Editor");
            ::SetTimer(hwnd, 1, 20, 0);

            if (effect)
            {
                effect->dispatcher(effect, effEditOpen, 0, 0, hwnd, 0);

                ERect * eRect = 0;

                effect->dispatcher(effect, effEditGetRect, 0, 0, &eRect, 0);

                if (eRect)
                {
                    int width = eRect->right - eRect->left;
                    int height = eRect->bottom - eRect->top;

                    if (width < 50)
                        width = 50;
                    if (height < 50)
                        height = 50;

                    RECT wRect;

                    ::SetRect(&wRect, 0, 0, width, height);
                    ::AdjustWindowRectEx(&wRect, (DWORD) ::GetWindowLongW(hwnd, GWL_STYLE), FALSE, (DWORD) ::GetWindowLongW(hwnd, GWL_EXSTYLE));

                    width = wRect.right - wRect.left;
                    height = wRect.bottom - wRect.top;

                    ::SetWindowPos(hwnd, HWND_TOP, 0, 0, width, height, SWP_NOMOVE);
                }
            }
        } break;

        case WM_TIMER:
            effect = (AEffect *) ::GetWindowLongPtrW(hwnd, GWLP_USERDATA);

            if (effect)
                effect->dispatcher(effect, effEditIdle, 0, 0, 0, 0);
            break;

        case WM_CLOSE:
        {
            effect = (AEffect *) ::GetWindowLongPtrW(hwnd, GWLP_USERDATA);

            ::KillTimer(hwnd, 1);

            if (effect)
                effect->dispatcher(effect, effEditClose, 0, 0, 0, 0);

            ::EndDialog(hwnd, IDOK);
            break;
        }
    }

    return 0;
}

struct audioMasterData
{
    VstIntPtr effect_number;
};

static VstIntPtr VSTCALLBACK audioMaster(AEffect * effect, VstInt32 opcode, VstInt32, VstIntPtr, void * ptr, float)
{
    audioMasterData * data = nullptr;

    if (effect)
        data = (audioMasterData *) effect->user;

    switch (opcode)
    {
        case audioMasterVersion:
            return kVstVersion;

        case audioMasterCurrentId:
            if (data)
                return data->effect_number;
            break;

        case audioMasterGetVendorString:
            strncpy((char *) ptr, "NoWork, Inc.", 64);
            break;

        case audioMasterGetProductString:
            strncpy((char *) ptr, "VSTi Host Bridge", 64);
            break;

        case audioMasterGetVendorVersion:
            return 1000;

        case audioMasterGetLanguage:
            return kVstLangEnglish;

        case audioMasterVendorSpecific: // Steinberg HACK
            if (ptr)
            {
                uint32_t * blah = (uint32_t *) (((char *) ptr) - 4);
                if (*blah == 0x0737bb68)
                {
                    *blah ^= 0x5CC8F349;
                    blah[2] = 0x19E;
                    return 0x1E7;
                }
            }
            break;

        case audioMasterGetDirectory:
            return (VstIntPtr) _DirectoryPath;

            /* More crap */
        case DECLARE_VST_DEPRECATED(audioMasterNeedIdle):
            need_idle = true;
            return 0;
    }

    return 0;
}

LONG __stdcall FilterUnhandledException(LPEXCEPTION_POINTERS param)
{
    if (::IsDebuggerPresent())
        return ::UnhandledExceptionFilter(param);

    // DumpCrashInfo( param );
    ::TerminateProcess(::GetCurrentProcess(), 0);

    return 0; // never reached
}

int WINAPI WinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int)
{
    int argc = 0;

    LPWSTR * argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

    if (argv == nullptr || argc != 3)
        return 1;

    wchar_t * end_char = nullptr;

#ifdef NDEBUG
    unsigned Cookie = ::wcstoul(argv[2], &end_char, 16);

    if (end_char == argv[2] || *end_char)
        return 2;
#endif

    uint32_t Sum = 0;

    end_char = argv[1];

    while (*end_char)
        Sum += (WCHAR) (*end_char++ * 820109);

#ifdef NDEBUG
    if (Sum != Cookie)
        return 3;
#endif

    uint32_t Code = 0;

    AEffect * Effect[MaxMIDIPorts] = {};
    bool EffectStarted[MaxMIDIPorts] = {};

    audioMasterData effectData[MaxMIDIPorts] = { { 0 }, { 1 }, { 2 }, { 3 } };

    std::vector<uint8_t> State;

    uint32_t SampleRate = 44100;

    std::vector<uint8_t> chunk;
    std::vector<float> sample_buffer;

    _hFileNUL = ::CreateFileW(_T("NUL"), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

    _hPipeIn  = ::GetStdHandle(STD_INPUT_HANDLE);
    _hPipeOut = ::GetStdHandle(STD_OUTPUT_HANDLE);

    ::SetStdHandle(STD_INPUT_HANDLE, _hFileNUL);
    ::SetStdHandle(STD_OUTPUT_HANDLE, _hFileNUL);

    {
        INITCOMMONCONTROLSEX icc =
        {
            sizeof(icc),
            ICC_WIN95_CLASSES | ICC_COOL_CLASSES | ICC_STANDARD_CLASSES
        };

        if (!::InitCommonControlsEx(&icc))
            return 4;
    }

    if (FAILED(::CoInitialize(NULL)))
        return 5;

#ifndef _DEBUG
    ::SetUnhandledExceptionFilter(FilterUnhandledException);
#endif

    {
        size_t l = ::wcslen(argv[1]);

        _DirectoryPath = (char *) ::malloc(l + 1);

        ::wcstombs(_DirectoryPath, argv[1], l);

        _DirectoryPath[l] = '\0';

        char * Slash = ::strrchr(_DirectoryPath, '\\');

        *Slash = '\0';
    }

    float ** float_list_in = nullptr;
    float ** float_list_out = nullptr;
    float * float_null = nullptr;
    float * float_out = nullptr;
    uint32_t MaxOutputs = 0;
    main_func Main = nullptr;

    HMODULE hModule = ::LoadLibraryW(argv[1]);

    if (hModule == 0)
    {
        Code = 6;
        goto exit;
    }

    #pragma warning(disable: 4191) //unsafe conversion from 'FARPROC' to 'main_func'

    // Find the DLL entry point.
    Main = (main_func) ::GetProcAddress(hModule, "VSTPluginMain");

    if (Main == nullptr)
    {
        Main = (main_func) ::GetProcAddress(hModule, "main");

        if (Main == nullptr)
        {
            Main = (main_func) ::GetProcAddress(hModule, "MAIN");

            if (Main == nullptr)
            {
                Code = 7;
                goto exit;
            }
        }
    }

    {
        Effect[0] = Main(&audioMaster);

        if ((Effect[0] == nullptr) || (Effect[0]->magic != kEffectMagic))
        {
            Code = 8;
            goto exit;
        }

        Effect[0]->user = &effectData[0];
        Effect[0]->dispatcher(Effect[0], effOpen, 0, 0, 0, 0);

        if ((Effect[0]->dispatcher(Effect[0], effGetPlugCategory, 0, 0, 0, 0) != kPlugCategSynth) || (Effect[0]->dispatcher(Effect[0], effCanDo, 0, 0, (void *) "receiveVstMidiEvent", 0) < 1))
        {
            Code = 9;
            goto exit;
        }
    }

    MaxOutputs = (uint32_t) min(Effect[0]->numOutputs, 2);

    {
        char name_string[256] = { 0 };
        char vendor_string[256] = { 0 };
        char product_string[256] = { 0 };

        uint32_t name_string_length;
        uint32_t vendor_string_length;
        uint32_t product_string_length;
        uint32_t vendor_version;
        uint32_t unique_id;

        Effect[0]->dispatcher(Effect[0], effGetEffectName, 0, 0, &name_string, 0);
        Effect[0]->dispatcher(Effect[0], effGetVendorString, 0, 0, &vendor_string, 0);
        Effect[0]->dispatcher(Effect[0], effGetProductString, 0, 0, &product_string, 0);

        name_string_length = (uint32_t) ::strlen(name_string);
        vendor_string_length = (uint32_t) ::strlen(vendor_string);
        product_string_length = (uint32_t) ::strlen(product_string);
        vendor_version = (uint32_t) Effect[0]->dispatcher(Effect[0], effGetVendorVersion, 0, 0, 0, 0);
        unique_id = (uint32_t) Effect[0]->uniqueID;

        WriteCode(0);
        WriteCode(name_string_length);
        WriteCode(vendor_string_length);
        WriteCode(product_string_length);
        WriteCode(vendor_version);
        WriteCode(unique_id);
        WriteCode(MaxOutputs);

        if (name_string_length)
            WriteBytes(name_string, name_string_length);

        if (vendor_string_length)
            WriteBytes(vendor_string, vendor_string_length);

        if (product_string_length)
            WriteBytes(product_string, product_string_length);
    }

    for (;;)
    {
        Code = ReadCode();

        if (Code == 0)
            break;

        switch (Code)
        {
            case 1: // Get Chunk
            {
                ReadChunk(Effect[0], chunk);

                WriteCode(0);
                WriteCode((uint32_t) chunk.size());
                WriteBytes(chunk.data(), (uint32_t) chunk.size());
                break;
            }

            case 2: // Set Chunk
            {
                uint32_t size = ReadCode();
                chunk.resize(size);
                if (size) ReadBytes(chunk.data(), size);

                for (auto effect : Effect)
                    setChunk(effect, chunk);

                WriteCode(0);
                break;
            }

            case 3: // Has Editor
            {
                uint32_t has_editor = (Effect[0]->flags & effFlagsHasEditor) ? 1u : 0u;

                WriteCode(0);
                WriteCode(has_editor);
                break;
            }

            case 4: // Display Editor Modal
            {
                if (Effect[0]->flags & effFlagsHasEditor)
                {
                    MyDLGTEMPLATE t;

                    t.style = WS_POPUPWINDOW | WS_DLGFRAME | DS_MODALFRAME | DS_CENTER;

                    DialogBoxIndirectParam(0, &t, ::GetDesktopWindow(), (DLGPROC) EditorProc, (LPARAM) (Effect[0]));

                    ReadChunk(Effect[0], chunk);
                    for (unsigned port = 1; port < MaxMIDIPorts; ++port)
                        setChunk(Effect[port], chunk);
                }

                WriteCode(0);
                break;
            }

            case 5: // Set Sample Rate
            {
                uint32_t size = ReadCode();

                if (size != sizeof(SampleRate))
                {
                    Code = 10;
                    goto exit;
                }

                SampleRate = ReadCode();

                WriteCode(0);
                break;
            }

            case 6: // Reset
            {
                for (unsigned port = 0; port < MaxMIDIPorts; ++port)
                {
                    if (!Effect[port]) continue;
                    if (EffectStarted[port])
                        Effect[port]->dispatcher(Effect[port], effStopProcess, 0, 0, 0, 0);
                    Effect[port]->dispatcher(Effect[port], effClose, 0, 0, 0, 0);
                    Effect[port] = nullptr;
                    EffectStarted[port] = false;
                }

                State.resize(0);
                idle_started = false;

                FreeEvents();

                Effect[0] = Main(&audioMaster);

                if (!Effect[0])
                {
                    Code = 8;
                    goto exit;
                }

                Effect[0]->user = &effectData[0];
                Effect[0]->dispatcher(Effect[0], effOpen, 0, 0, 0, 0);
                setChunk(Effect[0], chunk);

                WriteCode(0);
                break;
            }

            case 7: // Send MIDI Event
            {
                vst_event_t * ev = (vst_event_t *) calloc(sizeof(vst_event_t), 1);

                if (ev != nullptr)
                {
                    if (_EventsTail)
                        _EventsTail->next = ev;

                    _EventsTail = ev;

                    if (!_EventsHead)
                        _EventsHead = ev;

                    uint32_t b = ReadCode();

                    ev->port = (b & 0x7F000000) >> 24;

                    ev->ev.midiEvent.type = kVstMidiType;
                    ev->ev.midiEvent.byteSize = sizeof(ev->ev.midiEvent);

                    memcpy(&ev->ev.midiEvent.midiData, &b, 3);

                    WriteCode(0);
                }
                break;
            }

            case 8: // Send System Exclusive Event
            {
                vst_event_t * ev = (vst_event_t *) calloc(sizeof(vst_event_t), 1);

                if (ev != nullptr)
                {
                    if (_EventsTail)
                        _EventsTail->next = ev;

                    _EventsTail = ev;

                    if (!_EventsHead)
                        _EventsHead = ev;

                    uint32_t size = ReadCode();
                    uint32_t port = size >> 24; size &= 0xFFFFFF;

                    ev->port = port;

                    ev->ev.sysexEvent.type = kVstSysExType;
                    ev->ev.sysexEvent.byteSize = sizeof(ev->ev.sysexEvent);
                    ev->ev.sysexEvent.dumpBytes = (VstInt32) size;
                    ev->ev.sysexEvent.sysexDump = (char *) ::malloc(size);

                    ReadBytes(ev->ev.sysexEvent.sysexDump, size);

                    WriteCode(0);
                }
                break;
            }

            case 9: // Render Samples
            {
                unsigned event_count[MaxMIDIPorts] = {};
                for (auto ev = _EventsHead; ev; ev = ev->next)
                    if (ev->port < MaxMIDIPorts)
                        ++event_count[ev->port];

                // Instantiate only ports receiving data, not idle copies for
                // every single-port song. Unsupported ports never alias port D.
                for (unsigned port = 0; port < MaxMIDIPorts; ++port)
                {
                    if (!Effect[port] && event_count[port])
                    {
                        Effect[port] = Main(&audioMaster);
                        if (!Effect[port] || Effect[port]->magic != kEffectMagic)
                        {
                            Code = 11;
                            goto exit;
                        }
                        Effect[port]->user = &effectData[port];
                        Effect[port]->dispatcher(Effect[port], effOpen, 0, 0, 0, 0);
                        setChunk(Effect[port], chunk);
                        if (Effect[port]->numInputs != Effect[0]->numInputs ||
                            Effect[port]->numOutputs != Effect[0]->numOutputs)
                        {
                            Code = 11;
                            goto exit;
                        }
                    }
                    if (!Effect[port] || EffectStarted[port]) continue;
                    Effect[port]->dispatcher(Effect[port], effSetSampleRate, 0, 0, 0, float(SampleRate));
                    Effect[port]->dispatcher(Effect[port], effSetBlockSize, 0, BUFFER_SIZE, 0, 0);
                    Effect[port]->dispatcher(Effect[port], effMainsChanged, 0, 1, 0, 0);
                    Effect[port]->dispatcher(Effect[port], effStartProcess, 0, 0, 0, 0);
                    EffectStarted[port] = true;
                }

                const uint32_t num_outputs = (uint32_t) Effect[0]->numOutputs;
                if (State.empty())
                {
                    size_t buffer_size = sizeof(float *) * (Effect[0]->numInputs + num_outputs * MaxMIDIPorts);
                    buffer_size += sizeof(float) * BUFFER_SIZE * (1 + num_outputs * MaxMIDIPorts);
                    State.resize(buffer_size);
                    float_list_in = (float **) State.data();
                    float_list_out = float_list_in + Effect[0]->numInputs;
                    float_null = (float *) (float_list_out + num_outputs * MaxMIDIPorts);
                    float_out = float_null + BUFFER_SIZE;
                    for (uint32_t i = 0; i < (uint32_t) Effect[0]->numInputs; ++i)
                        float_list_in[i] = float_null;
                    for (uint32_t i = 0; i < num_outputs * MaxMIDIPorts; ++i)
                        float_list_out[i] = float_out + BUFFER_SIZE * i;
                    ::memset(float_null, 0, BUFFER_SIZE * sizeof(float));
                    sample_buffer.resize(BUFFER_SIZE * MaxOutputs);
                }

                if (need_idle)
                {
                    for (auto effect : Effect)
                        if (effect) effect->dispatcher(effect, DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
                    if (!idle_started)
                    {
                        unsigned idle_run = BUFFER_SIZE * 200;
                        while (idle_run)
                        {
                            uint32_t count_to_do = min(idle_run, BUFFER_SIZE);
                            for (unsigned port = 0; port < MaxMIDIPorts; ++port)
                            {
                                auto effect = Effect[port];
                                if (!effect) continue;
                                effect->processReplacing(effect, float_list_in, float_list_out + num_outputs * port, (VstInt32) count_to_do);
                                effect->dispatcher(effect, DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
                            }
                            idle_run -= count_to_do;
                        }
                    }
                }

                VstEvents * events[MaxMIDIPorts] = {};
                std::vector<VstIntPtr> event_storage[MaxMIDIPorts];
                for (unsigned port = 0; port < MaxMIDIPorts; ++port)
                {
                    if (!event_count[port]) continue;
                    const size_t bytes = offsetof(VstEvents, events) + sizeof(VstEvent *) * event_count[port];
                    event_storage[port].resize((bytes + sizeof(VstIntPtr) - 1) / sizeof(VstIntPtr));
                    events[port] = (VstEvents *) event_storage[port].data();
                    events[port]->numEvents = (VstInt32) event_count[port];
                    events[port]->reserved = 0;
                    unsigned index = 0;
                    for (auto ev = _EventsHead; ev; ev = ev->next)
                        if (ev->port == port)
                            events[port]->events[index++] = (VstEvent *) &ev->ev;
                    Effect[port]->dispatcher(Effect[port], effProcessEvents, 0, 0, events[port], 0);
                }

                if (need_idle)
                {
                    for (auto effect : Effect)
                        if (effect) effect->dispatcher(effect, DECLARE_VST_DEPRECATED(effIdle), 0, 0, 0, 0);
                    if (!idle_started)
                    {
                        for (unsigned port = 0; port < MaxMIDIPorts; ++port)
                            if (events[port])
                                Effect[port]->dispatcher(Effect[port], effProcessEvents, 0, 0, events[port], 0);
                        idle_started = true;
                    }
                }

                uint32_t SampleCount = ReadCode();
                WriteCode(0);
                while (SampleCount)
                {
                    unsigned SamplesToDo = min(SampleCount, BUFFER_SIZE);
                    for (unsigned port = 0; port < MaxMIDIPorts; ++port)
                        if (Effect[port])
                            Effect[port]->processReplacing(Effect[port], float_list_in, float_list_out + num_outputs * port, (VstInt32) SamplesToDo);
                    float * out = sample_buffer.data();
                    for (unsigned i = 0; i < SamplesToDo; ++i)
                    {
                        for (unsigned channel = 0; channel < MaxOutputs; ++channel)
                        {
                            float sample = 0;
                            for (unsigned port = 0; port < MaxMIDIPorts; ++port)
                                if (Effect[port])
                                    sample += float_out[i + BUFFER_SIZE * (channel + num_outputs * port)];
                            *out++ = sample;
                        }
                    }
                    WriteBytes(sample_buffer.data(), SamplesToDo * MaxOutputs * sizeof(float));
                    SampleCount -= SamplesToDo;
                }
                FreeEvents();
                break;
            }

            case 10: // Send MIDI Event, with timestamp
            {
                vst_event_t * ev = (vst_event_t *) calloc(sizeof(vst_event_t), 1);

                if (_EventsTail) _EventsTail->next = ev;

                _EventsTail = ev;

                if (!_EventsHead) _EventsHead = ev;

                uint32_t b = ReadCode();
                uint32_t timestamp = ReadCode();

                ev->port = (b & 0x7F000000) >> 24;

                ev->ev.midiEvent.type = kVstMidiType;
                ev->ev.midiEvent.byteSize = sizeof(ev->ev.midiEvent);
                memcpy(&ev->ev.midiEvent.midiData, &b, 3);
                ev->ev.midiEvent.deltaFrames = (VstInt32) timestamp;

                WriteCode(0);
                break;
            }

            case 11: // Send System Exclusive Event, with timestamp
            {
                vst_event_t * ev = (vst_event_t *) calloc(sizeof(vst_event_t), 1);

                if (_EventsTail) _EventsTail->next = ev;

                _EventsTail = ev;

                if (!_EventsHead) _EventsHead = ev;

                uint32_t size = ReadCode();
                uint32_t port = size >> 24;
                size &= 0xFFFFFF;

                uint32_t timestamp = ReadCode();

                ev->port = port;
                ev->ev.sysexEvent.type = kVstSysExType;
                ev->ev.sysexEvent.byteSize = sizeof(ev->ev.sysexEvent);
                ev->ev.sysexEvent.dumpBytes = (VstInt32) size;
                ev->ev.sysexEvent.sysexDump = (char *) malloc(size);
                ev->ev.sysexEvent.deltaFrames = (VstInt32) timestamp;

                ReadBytes(ev->ev.sysexEvent.sysexDump, size);

                WriteCode(0);
                break;
            }

            default:
            {
                Code = 12;
                goto exit;
            }
        }
    }

exit:
    for (unsigned port = 0; port < MaxMIDIPorts; ++port)
    {
        if (!Effect[port]) continue;
        if (EffectStarted[port])
            Effect[port]->dispatcher(Effect[port], effStopProcess, 0, 0, 0, 0);
        Effect[port]->dispatcher(Effect[port], effClose, 0, 0, 0, 0);
    }

    FreeEvents();

    if (hModule)
        ::FreeLibrary(hModule);

    ::CoUninitialize();

    if (_DirectoryPath)
        free(_DirectoryPath);

    if (argv)
        ::LocalFree(argv);

    WriteCode(Code);

    if (_hFileNUL)
    {
        ::CloseHandle(_hFileNUL);

        ::SetStdHandle(STD_INPUT_HANDLE, _hPipeIn);
        ::SetStdHandle(STD_OUTPUT_HANDLE, _hPipeOut);
    }

    return (int) Code;
}
