/* aeffectx.h — clean-room VST 2.4 plugin interface header.
 *
 * Reverse-engineered from public Steinberg VST 2.4 API documentation and
 * the host/plugin contract that thousands of Linux/macOS/Windows audio
 * plugins compile against. Distributed with Zeus under GPL v2-or-later;
 * no Steinberg code is reproduced here. The struct layout, opcode values,
 * and function signatures are matters of binary compatibility, not
 * copyrightable expression.
 *
 * Use: include this file from a host or plugin C/C++ source. The host
 * provides an audioMasterCallback, calls VSTPluginMain to obtain the
 * AEffect*, then drives the plugin via dispatcher() / processReplacing()
 * / setParameter() / getParameter().
 */

#ifndef ZEUS_VESTIGE_AEFFECTX_H
#define ZEUS_VESTIGE_AEFFECTX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AEffect.magic — plugins set this to 'VstP' (0x56737450) when valid. */
#define kEffectMagic ((int32_t)('V' << 24 | 's' << 16 | 't' << 8 | 'P'))

struct AEffect;

typedef intptr_t (*audioMasterCallback)(struct AEffect* effect,
                                        int32_t opcode,
                                        int32_t index,
                                        intptr_t value,
                                        void* ptr,
                                        float opt);

typedef intptr_t (*AEffectDispatcherProc)(struct AEffect* effect,
                                          int32_t opcode,
                                          int32_t index,
                                          intptr_t value,
                                          void* ptr,
                                          float opt);

typedef void (*AEffectProcessProc)(struct AEffect* effect,
                                   float** inputs,
                                   float** outputs,
                                   int32_t sampleFrames);

typedef void (*AEffectProcessDoubleProc)(struct AEffect* effect,
                                         double** inputs,
                                         double** outputs,
                                         int32_t sampleFrames);

typedef void (*AEffectSetParameterProc)(struct AEffect* effect,
                                        int32_t index,
                                        float parameter);

typedef float (*AEffectGetParameterProc)(struct AEffect* effect,
                                         int32_t index);

/* AEffect.flags bitmask. */
enum {
    effFlagsHasEditor          = 1 << 0,
    effFlagsCanReplacing       = 1 << 4,
    effFlagsProgramChunks      = 1 << 5,
    effFlagsIsSynth            = 1 << 8,
    effFlagsNoSoundInStop      = 1 << 9,
    effFlagsCanDoubleReplacing = 1 << 12
};

struct AEffect {
    int32_t                    magic;        /* kEffectMagic */
    AEffectDispatcherProc      dispatcher;
    AEffectProcessProc         process;      /* legacy / deprecated */
    AEffectSetParameterProc    setParameter;
    AEffectGetParameterProc    getParameter;

    int32_t                    numPrograms;
    int32_t                    numParams;
    int32_t                    numInputs;
    int32_t                    numOutputs;

    int32_t                    flags;

    intptr_t                   resvd1;       /* reserved for host */
    intptr_t                   resvd2;       /* reserved for host */

    int32_t                    initialDelay; /* samples */

    int32_t                    realQualities;     /* deprecated */
    int32_t                    offQualities;      /* deprecated */
    float                      ioRatio;           /* deprecated */

    void*                      object;       /* plugin's user data */
    void*                      user;         /* host's user data */

    int32_t                    uniqueID;
    int32_t                    version;

    AEffectProcessProc         processReplacing;
    AEffectProcessDoubleProc   processDoubleReplacing;

    char                       future[56];
};

/* dispatcher() opcodes — host -> plugin. */
enum AEffectOpcodes {
    effOpen                    = 0,
    effClose                   = 1,
    effSetProgram              = 2,
    effGetProgram              = 3,
    effSetProgramName          = 4,
    effGetProgramName          = 5,
    effGetParamLabel           = 6,
    effGetParamDisplay         = 7,
    effGetParamName            = 8,
    effSetSampleRate           = 10,
    effSetBlockSize            = 11,
    effMainsChanged            = 12,   /* value: 0=suspend, 1=resume */
    effEditGetRect             = 13,   /* ptr: ERect** */
    effEditOpen                = 14,   /* ptr: native window handle */
    effEditClose               = 15,
    effEditIdle                = 19,
    effGetChunk                = 23,
    effSetChunk                = 24,
    effProcessEvents           = 25,
    effCanBeAutomated          = 26,
    effString2Parameter        = 27,
    effGetProgramNameIndexed   = 29,
    effGetInputProperties      = 33,
    effGetOutputProperties     = 34,
    effGetPlugCategory         = 35,
    effOfflineNotify           = 38,
    effOfflinePrepare          = 39,
    effOfflineRun              = 40,
    effSetSpeakerArrangement   = 42,
    effSetBypass               = 44,
    effGetEffectName           = 45,
    effGetVendorString         = 47,
    effGetProductString        = 48,
    effGetVendorVersion        = 49,
    effVendorSpecific          = 50,
    effCanDo                   = 51,
    effGetTailSize             = 52,
    effGetParameterProperties  = 56,
    effGetVstVersion           = 58,
    effEditKeyDown             = 59,
    effEditKeyUp               = 60,
    effSetEditKnobMode         = 61,
    effGetMidiProgramName      = 62,
    effGetCurrentMidiProgram   = 63,
    effGetMidiProgramCategory  = 64,
    effHasMidiProgramsChanged  = 65,
    effGetMidiKeyName          = 66,
    effBeginSetProgram         = 67,
    effEndSetProgram           = 68,
    effGetSpeakerArrangement   = 69,
    effShellGetNextPlugin      = 70,
    effStartProcess            = 71,
    effStopProcess             = 72,
    effSetTotalSampleToProcess = 73,
    effSetPanLaw               = 74,
    effBeginLoadBank           = 75,
    effBeginLoadProgram        = 76,
    effSetProcessPrecision     = 77,
    effGetNumMidiInputChannels = 78,
    effGetNumMidiOutputChannels = 79
};

/* audioMasterCallback opcodes — plugin -> host. */
enum AudioMasterOpcodes {
    audioMasterAutomate                = 0,
    audioMasterVersion                 = 1,    /* expects 2400 for VST 2.4 */
    audioMasterCurrentId               = 2,
    audioMasterIdle                    = 3,
    audioMasterPinConnected            = 4,
    audioMasterWantMidi                = 6,
    audioMasterGetTime                 = 7,
    audioMasterProcessEvents           = 8,
    audioMasterIOChanged               = 13,
    audioMasterSizeWindow              = 15,
    audioMasterGetSampleRate           = 16,
    audioMasterGetBlockSize            = 17,
    audioMasterGetInputLatency         = 18,
    audioMasterGetOutputLatency        = 19,
    audioMasterGetCurrentProcessLevel  = 23,
    audioMasterGetAutomationState      = 24,
    audioMasterOfflineStart            = 25,
    audioMasterOfflineRead             = 26,
    audioMasterOfflineWrite            = 27,
    audioMasterOfflineGetCurrentPass   = 28,
    audioMasterOfflineGetCurrentMetaPass = 29,
    audioMasterGetVendorString         = 32,
    audioMasterGetProductString        = 33,
    audioMasterGetVendorVersion        = 34,
    audioMasterVendorSpecific          = 35,
    audioMasterCanDo                   = 37,
    audioMasterGetLanguage             = 38,
    audioMasterGetDirectory            = 41,
    audioMasterUpdateDisplay           = 42,
    audioMasterBeginEdit               = 43,
    audioMasterEndEdit                 = 44,
    audioMasterOpenFileSelector        = 45,
    audioMasterCloseFileSelector       = 46
};

/* effEditGetRect returns one of these via the ptr argument. */
typedef struct ERect {
    int16_t top;
    int16_t left;
    int16_t bottom;
    int16_t right;
} ERect;

/* String length conventions (bytes, not chars). */
enum {
    kVstMaxParamStrLen     = 8,
    kVstMaxLabelLen        = 64,
    kVstMaxShortLabelLen   = 8,
    kVstMaxCategLabelLen   = 24,
    kVstMaxFileNameLen     = 100,
    kVstMaxNameLen         = 64,
    kVstMaxVendorStrLen    = 64,
    kVstMaxProductStrLen   = 64,
    kVstMaxEffectNameLen   = 32
};

/* VstTimeInfo — supplied by host on audioMasterGetTime. */
typedef struct VstTimeInfo {
    double  samplePos;
    double  sampleRate;
    double  nanoSeconds;
    double  ppqPos;
    double  tempo;
    double  barStartPos;
    double  cycleStartPos;
    double  cycleEndPos;
    int32_t timeSigNumerator;
    int32_t timeSigDenominator;
    int32_t smpteOffset;
    int32_t smpteFrameRate;
    int32_t samplesToNextClock;
    int32_t flags;
} VstTimeInfo;

enum VstTimeInfoFlags {
    kVstTransportChanged       = 1 << 0,
    kVstTransportPlaying       = 1 << 1,
    kVstTransportCycleActive   = 1 << 2,
    kVstTransportRecording     = 1 << 3,
    kVstAutomationWriting      = 1 << 6,
    kVstAutomationReading      = 1 << 7,
    kVstNanosValid             = 1 << 8,
    kVstPpqPosValid            = 1 << 9,
    kVstTempoValid             = 1 << 10,
    kVstBarsValid              = 1 << 11,
    kVstCyclePosValid          = 1 << 12,
    kVstTimeSigValid           = 1 << 13,
    kVstSmpteValid             = 1 << 14,
    kVstClockValid             = 1 << 15
};

/* Plugin entry-point name. The host dlsyms one of these to obtain
 * the VSTPluginMain function. "main" is the legacy alias. */
typedef struct AEffect* (*VSTPluginMainFn)(audioMasterCallback audioMaster);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZEUS_VESTIGE_AEFFECTX_H */
