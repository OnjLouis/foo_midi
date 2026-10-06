#include <pluginterfaces/vst2.x/aeffectx.h>
#include <cstring>
#include <vector>

namespace
{
int Instances = 0;
struct Change { int frame; int program; bool note; };
struct Probe
{
    AEffect effect{};
    int program = 0;
    bool note = false;
    std::vector<Change> changes;
};
VstIntPtr VSTCALLBACK Dispatch(AEffect * effect, VstInt32 op, VstInt32, VstIntPtr, void * ptr, float)
{
    auto & probe = *static_cast<Probe *>(effect->object);
    switch (op)
    {
        case effOpen: return 1;
        case effClose: --Instances; delete &probe; return 1;
        case effGetPlugCategory: return kPlugCategSynth;
        case effCanDo: return std::strcmp(static_cast<char *>(ptr), "receiveVstMidiEvent") == 0;
        case effGetEffectName: std::strcpy(static_cast<char *>(ptr), "Port regression probe"); return 1;
        case effGetVendorString: case effGetProductString: std::strcpy(static_cast<char *>(ptr), "Test fixture"); return 1;
        case effProcessEvents:
        {
            auto events = static_cast<VstEvents *>(ptr);
            for (int i = 0; i < events->numEvents; ++i)
            {
                auto event = events->events[i];
                if (event->type == kVstMidiType)
                {
                    auto midi = reinterpret_cast<VstMidiEvent *>(event);
                    const auto status = static_cast<unsigned char>(midi->midiData[0]) & 0xF0;
                    if (status == 0xC0) probe.program = static_cast<unsigned char>(midi->midiData[1]);
                    if (status == 0x90 || status == 0x80)
                        probe.changes.push_back({event->deltaFrames, probe.program,
                            status == 0x90 && midi->midiData[2] != 0});
                }
                else if (event->type == kVstSysExType)
                {
                    auto sysex = reinterpret_cast<VstMidiSysexEvent *>(event);
                    if (sysex->dumpBytes == 4 && static_cast<unsigned char>(sysex->sysexDump[1]) == 0x7D)
                        probe.changes.push_back({event->deltaFrames,
                            static_cast<unsigned char>(sysex->sysexDump[2]), true});
                }
            }
            return 1;
        }
    }
    return 0;
}
void VSTCALLBACK Process(AEffect * effect, float **, float ** out, VstInt32 frames)
{
    auto & probe = *static_cast<Probe *>(effect->object);
    size_t next = 0;
    for (int frame = 0; frame < frames; ++frame)
    {
        while (next < probe.changes.size() && probe.changes[next].frame <= frame)
        {
            probe.program = probe.changes[next].program;
            probe.note = probe.changes[next].note;
            ++next;
        }
        out[0][frame] = probe.note ? (probe.program + 1) * 0.01f : 0;
        out[1][frame] = probe.note ? Instances * 0.01f : 0;
    }
    probe.changes.clear();
}
}
extern "C" __declspec(dllexport) AEffect * VSTCALLBACK VSTPluginMain(audioMasterCallback)
{
    auto probe = new Probe;
    ++Instances;
    probe->effect.magic = kEffectMagic;
    probe->effect.dispatcher = Dispatch;
    probe->effect.numPrograms = 128;
    probe->effect.numOutputs = 2;
    probe->effect.flags = effFlagsCanReplacing | effFlagsIsSynth;
    probe->effect.object = probe;
    probe->effect.uniqueID = 0x506F7274;
    probe->effect.processReplacing = Process;
    return &probe->effect;
}
