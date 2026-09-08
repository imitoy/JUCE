/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

#include <dlfcn.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/utils/result.h>

namespace juce
{

//==============================================================================
// A native PipeWire audio backend, modelled on the JACK backend but talking to
// libpipewire directly. Like the JACK code, the library is dlopen'd at runtime
// so that applications only need the PipeWire development headers at build
// time, and don't have to link against the library explicitly.
//
// PipeWire is a graph-based audio server: a JUCE device opens a "playback"
// stream (an output node which connects to a sink such as a sound card) and/or
// a "capture" stream (an input node which connects to a source). Both streams
// are driven by the graph's clock, so - as with the JACK backend - the sample
// rate and buffer size are the ones currently in use by the server, not ones
// which can be chosen by the application.

static void* juce_libpipewireHandle = nullptr;

static void* juce_loadPipeWireFunction (const char* const name)
{
    if (juce_libpipewireHandle == nullptr)
        return nullptr;

    return dlsym (juce_libpipewireHandle, name);
}

#define JUCE_DECL_PIPEWIRE_FUNCTION(return_type, fn_name, argument_types, arguments)  \
  return_type fn_name argument_types                                                  \
  {                                                                                   \
      using ReturnType = return_type;                                                 \
      typedef return_type (*fn_type) argument_types;                                  \
      static fn_type fn = (fn_type) juce_loadPipeWireFunction (#fn_name);             \
      jassert (fn != nullptr);                                                        \
      return (fn != nullptr) ? ((*fn) arguments) : ReturnType();                      \
  }

#define JUCE_DECL_VOID_PIPEWIRE_FUNCTION(fn_name, argument_types, arguments)          \
  void fn_name argument_types                                                         \
  {                                                                                   \
      typedef void (*fn_type) argument_types;                                         \
      static fn_type fn = (fn_type) juce_loadPipeWireFunction (#fn_name);             \
      jassert (fn != nullptr);                                                        \
      if (fn != nullptr) (*fn) arguments;                                             \
  }

//==============================================================================
// Loading & general setup
JUCE_DECL_VOID_PIPEWIRE_FUNCTION (pw_init, (int* argc, char** argv[]), (argc, argv))

// Context & main loop
JUCE_DECL_PIPEWIRE_FUNCTION (struct pw_context*,   pw_context_new,        (struct pw_loop* loop, struct pw_properties* props, size_t userDataSize),              (loop, props, userDataSize))
JUCE_DECL_PIPEWIRE_FUNCTION (struct pw_properties*, pw_properties_new_dict, (const struct spa_dict* dict), (dict))
JUCE_DECL_PIPEWIRE_FUNCTION (struct pw_core*,      pw_context_connect,    (struct pw_context* ctx, struct pw_properties* props, size_t userDataSize),             (ctx, props, userDataSize))
JUCE_DECL_VOID_PIPEWIRE_FUNCTION (pw_context_destroy, (struct pw_context* ctx), (ctx))
JUCE_DECL_PIPEWIRE_FUNCTION (struct pw_main_loop*, pw_main_loop_new,      (const struct spa_dict* props),                                                         (props))
JUCE_DECL_PIPEWIRE_FUNCTION (struct pw_loop*,      pw_main_loop_get_loop, (struct pw_main_loop* loop),                                                           (loop))
JUCE_DECL_VOID_PIPEWIRE_FUNCTION (pw_main_loop_destroy, (struct pw_main_loop* loop), (loop))
JUCE_DECL_PIPEWIRE_FUNCTION (int, pw_main_loop_run,  (struct pw_main_loop* loop), (loop))
JUCE_DECL_PIPEWIRE_FUNCTION (int, pw_main_loop_quit, (struct pw_main_loop* loop), (loop))
JUCE_DECL_PIPEWIRE_FUNCTION (int, pw_core_disconnect, (struct pw_core* core), (core))

// Properties

// Streams
JUCE_DECL_PIPEWIRE_FUNCTION (struct pw_stream*, pw_stream_new,           (struct pw_core* core, const char* name, struct pw_properties* props),                   (core, name, props))
JUCE_DECL_VOID_PIPEWIRE_FUNCTION (pw_stream_destroy, (struct pw_stream* stream), (stream))
JUCE_DECL_VOID_PIPEWIRE_FUNCTION (pw_stream_add_listener, (struct pw_stream* stream, struct spa_hook* hook, const struct pw_stream_events* events, void* data), (stream, hook, events, data))
JUCE_DECL_PIPEWIRE_FUNCTION (enum pw_stream_state, pw_stream_get_state,  (struct pw_stream* stream, const char** error),                                          (stream, error))
JUCE_DECL_PIPEWIRE_FUNCTION (int, pw_stream_connect, (struct pw_stream* stream, enum pw_direction direction, uint32_t targetId, enum pw_stream_flags flags, const struct spa_pod** params, uint32_t nParams), (stream, direction, targetId, flags, params, nParams))
JUCE_DECL_PIPEWIRE_FUNCTION (int, pw_stream_disconnect, (struct pw_stream* stream), (stream))
JUCE_DECL_PIPEWIRE_FUNCTION (struct pw_buffer*, pw_stream_dequeue_buffer, (struct pw_stream* stream), (stream))
JUCE_DECL_PIPEWIRE_FUNCTION (int, pw_stream_queue_buffer, (struct pw_stream* stream, struct pw_buffer* buffer), (stream, buffer))
JUCE_DECL_PIPEWIRE_FUNCTION (int, pw_stream_get_time_n, (struct pw_stream* stream, struct pw_time* time, size_t size), (stream, time, size))

#if JUCE_DEBUG
 #define PIPEWIRE_LOGGING_ENABLED 1
#endif

#if PIPEWIRE_LOGGING_ENABLED
namespace
{
    void pipewire_Log (const String& s)
    {
        std::cerr << s << std::endl;
    }
}
 #define JUCE_PIPEWIRE_LOG(x)   pipewire_Log (x)
#else
 #define JUCE_PIPEWIRE_LOG(x)   {}
#endif

//==============================================================================
// Loads libpipewire (if it isn't already loaded) and initialises it. This must
// happen before any other PipeWire function is used, but is only required if a
// PipeWire device type is actually used, so it's done lazily.
static bool juce_initialisePipeWire()
{
    static const bool succeeded = []()
    {
        if (juce_libpipewireHandle == nullptr)
            juce_libpipewireHandle = dlopen ("libpipewire-0.3.so.0", RTLD_LAZY);

        if (juce_libpipewireHandle != nullptr)
            pw_init (nullptr, nullptr);

        return juce_libpipewireHandle != nullptr;
    }();

    return succeeded;
}

//==============================================================================
#ifndef JUCE_PIPEWIRE_CLIENT_NAME
 #ifdef JucePlugin_Name
  #define JUCE_PIPEWIRE_CLIENT_NAME JucePlugin_Name
 #else
  #define JUCE_PIPEWIRE_CLIENT_NAME "JUCEPipeWire"
 #endif
#endif

//==============================================================================
// Data collected while scanning the PipeWire registry. JUCE can't address
// nodes by their (unstable) registry ids, so each device is identified by its
// stable node.name, with a human-readable node.description used for display.
struct PipeWirePortDescription
{
    uint32_t portId = 0;
    String channelName;
};

struct PipeWireDeviceDescription
{
    String nodeName;                        // stable PipeWire node name, e.g. "alsa_output.pci-...analog-stereo"
    String displayName;                     // node.description (falling back to nodeName) for the UI
    Array<PipeWirePortDescription> inputPorts;   // ports with direction "in"  (the channels a sink accepts)
    Array<PipeWirePortDescription> outputPorts;  // ports with direction "out" (the channels a source offers)
};

struct PipeWireScanResults
{
    Array<PipeWireDeviceDescription> sinks, sources;
};

namespace
{
    const char* getPipeWireProp (const struct spa_dict* props, const char* key)
    {
        return props != nullptr ? spa_dict_lookup (props, key) : nullptr;
    }

    String getPipeWirePropString (const struct spa_dict* props, const char* key)
    {
        if (const char* v = getPipeWireProp (props, key))
            return String (CharPointer_UTF8 (v));

        return {};
    }

    uint32_t getPipeWirePropUInt (const struct spa_dict* props, const char* key, uint32_t fallback = 0)
    {
        const String v (getPipeWirePropString (props, key));

        if (v.isNotEmpty())
            return (uint32_t) v.getLargeIntValue();

        return fallback;
    }

    bool isPipeWireAudioChannelName (const String& name) noexcept { return name.isNotEmpty() && name != "UNK"; }

    // Converts a channel name as reported by a port's "audio.channel" property
    // (e.g. "FL") into the corresponding SPA audio channel enum value.
    uint32_t getPipeWireAudioChannelPosition (const String& channelName)
    {
        if (channelName == "MONO") return SPA_AUDIO_CHANNEL_MONO;
        if (channelName == "FL")   return SPA_AUDIO_CHANNEL_FL;
        if (channelName == "FR")   return SPA_AUDIO_CHANNEL_FR;
        if (channelName == "FC")   return SPA_AUDIO_CHANNEL_FC;
        if (channelName == "LFE")  return SPA_AUDIO_CHANNEL_LFE;
        if (channelName == "SL")   return SPA_AUDIO_CHANNEL_SL;
        if (channelName == "SR")   return SPA_AUDIO_CHANNEL_SR;
        if (channelName == "FLC")  return SPA_AUDIO_CHANNEL_FLC;
        if (channelName == "FRC")  return SPA_AUDIO_CHANNEL_FRC;
        if (channelName == "RC")   return SPA_AUDIO_CHANNEL_RC;
        if (channelName == "RL")   return SPA_AUDIO_CHANNEL_RL;
        if (channelName == "RR")   return SPA_AUDIO_CHANNEL_RR;
        if (channelName == "TC")   return SPA_AUDIO_CHANNEL_TC;
        if (channelName == "TFL")  return SPA_AUDIO_CHANNEL_TFL;
        if (channelName == "TFC")  return SPA_AUDIO_CHANNEL_TFC;
        if (channelName == "TFR")  return SPA_AUDIO_CHANNEL_TFR;
        if (channelName == "TRL")  return SPA_AUDIO_CHANNEL_TRL;
        if (channelName == "TRC")  return SPA_AUDIO_CHANNEL_TRC;
        if (channelName == "TRR")  return SPA_AUDIO_CHANNEL_TRR;
        if (channelName == "RLC")  return SPA_AUDIO_CHANNEL_RLC;
        if (channelName == "RRC")  return SPA_AUDIO_CHANNEL_RRC;
        if (channelName == "FLW")  return SPA_AUDIO_CHANNEL_FLW;
        if (channelName == "FRW")  return SPA_AUDIO_CHANNEL_FRW;
        if (channelName == "LFE2") return SPA_AUDIO_CHANNEL_LFE2;

        return SPA_AUDIO_CHANNEL_UNKNOWN;
    }

    String getFallbackPipeWireChannelName (int index, bool isInput)
    {
        return (isInput ? "in_" : "out_") + String (index + 1);
    }
}

//==============================================================================
// Scans the PipeWire registry for the sinks/sources which JUCE devices can be
// created from, along with the channels (ports) which each one exposes.
//
// The scan runs its own short-lived PipeWire session on a stack object, in the
// same way that the JACK backend opens a temporary client to scan for ports.
class PipeWireRegistryScanner
{
public:
    PipeWireRegistryScanner()
    {
        if (! juce_initialisePipeWire())
            return;

        mainLoop = juce::pw_main_loop_new (nullptr);
        context  = juce::pw_context_new (juce::pw_main_loop_get_loop (mainLoop), nullptr, 0);
        core     = juce::pw_context_connect (context, nullptr, 0);
    }

    ~PipeWireRegistryScanner()
    {
        stop();

        if (registry != nullptr)
        {
            pw_registry_destroy (registry, 0);
            registry = nullptr;
        }

        if (core != nullptr)
        {
            juce::pw_core_disconnect (core);
            core = nullptr;
        }

        if (context != nullptr)
        {
            juce::pw_context_destroy (context);
            context = nullptr;
        }

        if (mainLoop != nullptr)
        {
            juce::pw_main_loop_destroy (mainLoop);
            mainLoop = nullptr;
        }
    }

    // Runs the registry scan and fills in the result object. Returns false if
    // the PipeWire server couldn't be reached.
    bool scan (PipeWireScanResults& results)
    {
        if (core == nullptr)
            return false;

        coreEvents = {};
        coreEvents.version = PW_VERSION_CORE_EVENTS;
        coreEvents.done    = coreEventDone;
        pw_core_add_listener (core, &coreListener, &coreEvents, this);

        registryEvents = {};
        registryEvents.version       = PW_VERSION_REGISTRY_EVENTS;
        registryEvents.global        = registryEventGlobal;
        registryEvents.global_remove = registryEventGlobalRemove;
        registry = pw_core_get_registry (core, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener (registry, &registryListener, &registryEvents, this);

        // Wait until all existing globals have been reported and the sync
        // request has been answered.
        pw_core_sync (core, 0, ++syncSequence);
        juce::pw_main_loop_run (mainLoop);

        assembleResults (results);
        return true;
    }

private:
    //==============================================================================
    struct NodeRecord
    {
        uint32_t nodeId = 0;
        bool isSink = false;
        PipeWireDeviceDescription device;
    };

    void stop()
    {
        if (mainLoop != nullptr)
            juce::pw_main_loop_quit (mainLoop);
    }

    static void coreEventDone (void* data, uint32_t, int)
    {
        // All the globals have been reported now, so we can stop the loop.
        static_cast<PipeWireRegistryScanner*> (data)->stop();
    }

    static void registryEventGlobal (void* data, uint32_t id, uint32_t, const char* type,
                                     uint32_t, const struct spa_dict* props)
    {
        static_cast<PipeWireRegistryScanner*> (data)->handleGlobal (id, type, props);
    }

    static void registryEventGlobalRemove (void*, uint32_t)
    {
        // This one-shot scan is finished before any objects get removed.
    }

    void handleGlobal (uint32_t id, const char* type, const struct spa_dict* props)
    {
        if (type == nullptr)
            return;

        const String mediaClass (getPipeWirePropString (props, PW_KEY_MEDIA_CLASS));

        if (String (type) == PW_TYPE_INTERFACE_Node && (mediaClass == "Audio/Sink" || mediaClass.startsWith ("Audio/Sink/")
                                                        || mediaClass == "Audio/Source" || mediaClass.startsWith ("Audio/Source/")))
        {
            NodeRecord node;
            node.nodeId = id;
            node.isSink = mediaClass.startsWith ("Audio/Sink");
            node.device.nodeName = getPipeWirePropString (props, PW_KEY_NODE_NAME);
            node.device.displayName = getPipeWirePropString (props, PW_KEY_NODE_DESCRIPTION);

            if (node.device.displayName.isEmpty())
                node.device.displayName = node.device.nodeName;

            if (node.device.nodeName.isNotEmpty())
                nodes.add (node);
        }
        else if (String (type) == PW_TYPE_INTERFACE_Port)
        {
            // Associate the port with its node so the channels of a sink or
            // source can be discovered. Note that a sink's input ports are the
            // channels it accepts, and a source's output ports are the channels
            // it provides.
            const uint32_t nodeId = getPipeWirePropUInt (props, PW_KEY_NODE_ID);
            int index = -1;

            for (int i = 0; i < nodes.size(); ++i)
                if (nodes.getReference (i).nodeId == nodeId)
                {
                    index = i;
                    break;
                }

            if (index < 0)
                return;

            const bool isInput = getPipeWirePropString (props, PW_KEY_PORT_DIRECTION) == "in";

            PipeWirePortDescription port;
            port.portId = getPipeWirePropUInt (props, PW_KEY_PORT_ID);
            port.channelName = getPipeWirePropString (props, PW_KEY_AUDIO_CHANNEL);

            if (! isPipeWireAudioChannelName (port.channelName))
                port.channelName = getFallbackPipeWireChannelName ((int) port.portId, isInput);

            if (isInput)  nodes.getReference (index).device.inputPorts.add (port);
            else          nodes.getReference (index).device.outputPorts.add (port);
        }
    }

    void assembleResults (PipeWireScanResults& results)
    {
        for (auto& node : nodes)
        {
            if (node.isSink)
            {
                if (node.device.inputPorts.isEmpty())
                    continue;

                sortPorts (node.device.inputPorts);
                results.sinks.add (node.device);
            }
            else
            {
                if (node.device.outputPorts.isEmpty())
                    continue;

                sortPorts (node.device.outputPorts);
                results.sources.add (node.device);
            }
        }
    }

    static void sortPorts (Array<PipeWirePortDescription>& ports)
    {
        // Ports arrive in registry order, which isn't guaranteed to be sorted.
        struct Comparator
        {
            static int compareElements (const PipeWirePortDescription& a, const PipeWirePortDescription& b)
            {
                return (int) a.portId - (int) b.portId;
            }
        };

        Comparator c;
        ports.sort (c, false);
    }

    //==============================================================================
    struct pw_main_loop* mainLoop = nullptr;
    struct pw_context* context = nullptr;
    struct pw_core* core = nullptr;
    struct pw_registry* registry = nullptr;
    struct spa_hook coreListener;
    struct spa_hook registryListener;
    pw_core_events coreEvents;
    pw_registry_events registryEvents;

    Array<NodeRecord> nodes;
    int syncSequence = 0;

    JUCE_DECLARE_NON_COPYABLE (PipeWireRegistryScanner)
};

} // namespace juce

//==============================================================================
namespace juce
{

// A ring buffer which carries captured audio from the capture stream's
// process events to the playback stream's process events when a device is
// running duplex. Both streams are dispatched on the same main loop thread,
// but their order within a graph cycle isn't guaranteed, so the captured
// audio is buffered until the playback side is ready for it. The data is
// stored interleaved, and a whole block is committed to the ring with a single
// index update so a partially-written block can never be observed.
class PipeWireCaptureBuffer
{
public:
    PipeWireCaptureBuffer() = default;

    void resize (int numChannels, int capacityFrames)
    {
        channels = jmax (1, numChannels);
        capacity = jmax (1024, nextPowerOfTwo (capacityFrames));
        data.calloc (capacity * channels);
    }

    void reset()
    {
        readPos = 0;
        writePos = 0;
    }

    // Writes one complete block. Returns false (and counts an overrun) if
    // there isn't enough space, rather than overwriting unread data.
    bool writeBlock (float* const* channelData, int frames)
    {
        jassert (channelData != nullptr);

        if (frames <= 0)
            return true;

        const int space = capacity - 1 - getNumFramesAvailable();

        if (frames > space)
        {
            ++numOverruns;
            return false;
        }

        int writeIndex = writePos;

        for (int f = 0; f < frames; ++f)
        {
            for (int c = 0; c < channels; ++c)
                data[((size_t) writeIndex * (size_t) channels) + (size_t) c] = channelData[c][f];

            writeIndex = (writeIndex + 1) & (capacity - 1);
        }

        writePos = writeIndex;
        return true;
    }

    // Reads up to numFrames frames. Any missing frames (the capture side hasn't
    // produced enough data yet, e.g. when the graph was just started or the
    // two streams are briefly out of step) are filled with silence.
    void readBlock (float* const* channelData, int numFrames)
    {
        const int framesToRead = jmin (numFrames, getNumFramesAvailable());

        if (framesToRead > 0)
        {
            int readIndex = readPos;

            for (int f = 0; f < framesToRead; ++f)
            {
                for (int c = 0; c < channels; ++c)
                    channelData[c][f] = data[((size_t) readIndex * (size_t) channels) + (size_t) c];

                readIndex = (readIndex + 1) & (capacity - 1);
            }

            readPos = readIndex;
        }

        for (int c = 0; c < channels; ++c)
            if (numFrames > framesToRead)
                zeromem (channelData[c] + framesToRead, (size_t) (numFrames - framesToRead) * sizeof (float));

        if (numFrames > framesToRead)
            ++numUnderruns;
    }

    int getNumUnderruns() const noexcept  { return numUnderruns; }
    int getNumOverruns()  const noexcept  { return numOverruns; }

private:
    int getNumFramesAvailable() const
    {
        return (writePos - readPos) & (capacity - 1);
    }

    HeapBlock<float> data;
    int channels = 0;
    int capacity = 0;
    int readPos = 0;
    int writePos = 0;
    std::atomic<int> numUnderruns { 0 };
    std::atomic<int> numOverruns { 0 };

    JUCE_DECLARE_NON_COPYABLE (PipeWireCaptureBuffer)
};

//==============================================================================
// The thread which runs the PipeWire main loop. All the stream events - state
// changes, param changes and the process callbacks - are dispatched from
// here. This means the audio callback always runs on this single thread, and
// the device can be safely torn down from another thread once the loop has
// been stopped and this thread has exited.
class PipeWireMainLoopThread final : public Thread
{
public:
    explicit PipeWireMainLoopThread (struct pw_main_loop* loop)
        : Thread ("PipeWireMainLoop"), mainLoop (loop)
    {
    }

    void run() override
    {
        if (mainLoop != nullptr)
            juce::pw_main_loop_run (mainLoop);
    }

    // Called from another thread to stop the loop.
    void requestStop()
    {
        if (mainLoop != nullptr)
            juce::pw_main_loop_quit (mainLoop);
    }

private:
    struct pw_main_loop* mainLoop = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PipeWireMainLoopThread)
};

//==============================================================================
class PipeWireAudioIODevice final : public AudioIODevice
{
public:
    PipeWireAudioIODevice (const String& inName,
                           const String& outName,
                           const String& inNodeName,
                           const String& outNodeName,
                           const StringArray& inChannelNames,
                           const StringArray& outChannelNames)
        : AudioIODevice (outName.isEmpty() ? inName : outName, "PipeWire"),
          inputName (inName),
          outputName (outName),
          inputNodeName (inNodeName),
          outputNodeName (outNodeName),
          inputChannelNames (inChannelNames),
          outputChannelNames (outChannelNames)
    {
        jassert (outNodeName.isNotEmpty() || inNodeName.isNotEmpty());
    }

    ~PipeWireAudioIODevice() override
    {
        close();
    }

    //==============================================================================
    StringArray getOutputChannelNames() override   { return outputChannelNames; }
    StringArray getInputChannelNames() override    { return inputChannelNames; }

    Array<double> getAvailableSampleRates() override
    {
        Array<double> rates;

        if (currentSampleRate > 0)
            rates.add (currentSampleRate);

        return rates;
    }

    Array<int> getAvailableBufferSizes() override
    {
        Array<int> sizes;

        if (currentBufferSize > 0)
            sizes.add (currentBufferSize);

        return sizes;
    }

    int getDefaultBufferSize() override             { return currentBufferSize > 0 ? (int) currentBufferSize : fallbackBufferSize; }
    int getCurrentBufferSizeSamples() override      { return currentBufferSize; }
    double getCurrentSampleRate() override          { return currentSampleRate; }

    //==============================================================================
    String open (const BigInteger& inputChannels, const BigInteger& outputChannels,
                 double, int) override
    {
        close();

        if (! juce_initialisePipeWire())
        {
            lastError = "Couldn't load libpipewire";
            return lastError;
        }

        lastError.clear();
        captureBuffer.reset();

        const int numEnabledInputChannels  = inputChannels.countNumberOfSetBits();
        const int numEnabledOutputChannels = outputChannels.countNumberOfSetBits();

        const bool needInput  = numEnabledInputChannels > 0 && inputNodeName.isNotEmpty();
        const bool needOutput = numEnabledOutputChannels > 0 && outputNodeName.isNotEmpty();

        if (! needInput && ! needOutput)
        {
            lastError = "No PipeWire input or output selected";
            return lastError;
        }

        // Work out how many channels to ask PipeWire for. If the node's ports
        // were discovered during the scan, connect all of them (like the JACK
        // backend registers every port of the target client); otherwise fall
        // back to the number of channels the caller asked for.
        numInputChannels  = needInput  ? jmax (1, inputChannelNames.size()  > 0 ? inputChannelNames.size()  : numEnabledInputChannels)  : 0;
        numOutputChannels = needOutput ? jmax (1, outputChannelNames.size() > 0 ? outputChannelNames.size() : numEnabledOutputChannels) : 0;

        mainLoop = juce::pw_main_loop_new (nullptr);
        context  = juce::pw_context_new (juce::pw_main_loop_get_loop (mainLoop), nullptr, 0);
        core     = juce::pw_context_connect (context, nullptr, 0);

        if (core == nullptr)
        {
            lastError = "Unable to connect to the PipeWire server";
            close();
            return lastError;
        }

        // Translate the JUCE-enabled channels into the channel indices which
        // will be passed to the callback. Channels which exist on the device
        // but weren't enabled by the user are still connected, but are filled
        // with silence and not passed to the callback, mirroring the JACK
        // backend's behaviour with unconnected ports.
        activeInputChannels = inputChannels;
        activeOutputChannels = outputChannels;

        for (int i = numInputChannels; i <= activeInputChannels.getHighestBit(); ++i)
            activeInputChannels.clearBit (i);

        for (int i = numOutputChannels; i <= activeOutputChannels.getHighestBit(); ++i)
            activeOutputChannels.clearBit (i);

        allocateChannelScratchSpace();

        captureBuffer.resize (numInputChannels, ringBufferCapacity);

        streamEvents = {};
        streamEvents.version = PW_VERSION_STREAM_EVENTS;
        streamEvents.state_changed = streamStateChanged;
        streamEvents.param_changed = streamParamChanged;
        streamEvents.process = streamProcess;

        streamEventsCapture = {};
        streamEventsCapture.version = PW_VERSION_STREAM_EVENTS;
        streamEventsCapture.state_changed = streamStateChanged;
        streamEventsCapture.param_changed = streamParamChanged;
        streamEventsCapture.process = streamProcessCapture;

        if (needOutput)
        {
            playbackStream = createStream (outputNodeName, outputName, "Playback",
                                           PW_DIRECTION_OUTPUT, streamEvents, playbackListener);

            if (playbackStream == nullptr)
            {
                lastError = "Failed to create a PipeWire playback stream";
                close();
                return lastError;
            }
        }

        if (needInput)
        {
            captureStream = createStream (inputNodeName, inputName, "Capture",
                                          PW_DIRECTION_INPUT, streamEventsCapture, captureListener);

            if (captureStream == nullptr)
            {
                lastError = "Failed to create a PipeWire capture stream";
                close();
                return lastError;
            }
        }

        // Start the loop and wait until the streams have finished negotiating.
        loopThread.reset (new PipeWireMainLoopThread (mainLoop));
        loopThread->startThread();

        const auto timeout = Time::currentTimeMillis() + 10000;

        while (! streamsAreReady() && ! streamsHaveFailed() && Time::currentTimeMillis() < timeout)
            Thread::sleep (10);

        if (streamsHaveFailed())
        {
            lastError = "Couldn't negotiate a PipeWire audio format with the selected device";
            close();
            return lastError;
        }

        if (! streamsAreReady())
        {
            lastError = "Timed out waiting for the PipeWire streams to start";
            close();
            return lastError;
        }

        if (currentSampleRate <= 0)
            currentSampleRate = 48000.0;

        if (currentBufferSize <= 0)
            currentBufferSize = 1024;

        deviceIsOpen = true;
        return lastError;
    }

    void close() override
    {
        stop();

        if (loopThread != nullptr)
        {
            loopThread->requestStop();
            loopThread->stopThread (2000);
            loopThread.reset();
        }

        if (captureStream != nullptr)
        {
            juce::pw_stream_disconnect (captureStream);
            juce::pw_stream_destroy (captureStream);
            captureStream = nullptr;
        }

        if (playbackStream != nullptr)
        {
            juce::pw_stream_disconnect (playbackStream);
            juce::pw_stream_destroy (playbackStream);
            playbackStream = nullptr;
        }

        if (core != nullptr)
        {
            juce::pw_core_disconnect (core);
            core = nullptr;
        }

        if (context != nullptr)
        {
            juce::pw_context_destroy (context);
            context = nullptr;
        }

        if (mainLoop != nullptr)
        {
            juce::pw_main_loop_destroy (mainLoop);
            mainLoop = nullptr;
        }

        numInputChannels = 0;
        numOutputChannels = 0;
        currentSampleRate = 0;
        currentBufferSize = 0;
        deviceIsOpen = false;
        streamsReady = false;
        streamsFailed = false;
    }

    void start (AudioIODeviceCallback* newCallback) override
    {
        if (deviceIsOpen && newCallback != callback)
        {
            if (newCallback != nullptr)
                newCallback->audioDeviceAboutToStart (this);

            AudioIODeviceCallback* const oldCallback = callback;

            {
                const ScopedLock sl (callbackLock);
                callback = newCallback;
            }

            if (oldCallback != nullptr)
                oldCallback->audioDeviceStopped();
        }
    }

    void stop() override
    {
        start (nullptr);
    }

    bool isOpen() override    { return deviceIsOpen; }
    bool isPlaying() override { return callback != nullptr; }
    int getCurrentBitDepth() override { return 32; }
    String getLastError() override    { return lastError; }

    int getXRunCount() const noexcept override
    {
        return xruns.load (std::memory_order_relaxed) + captureBuffer.getNumUnderruns() + captureBuffer.getNumOverruns();
    }

    BigInteger getActiveOutputChannels() const override  { return activeOutputChannels; }
    BigInteger getActiveInputChannels()  const override  { return activeInputChannels; }

    int getOutputLatencyInSamples() override
    {
        if (playbackStream != nullptr)
            if (int latency = getStreamLatencyInSamples (playbackStream))
                return latency;

        return currentBufferSize;
    }

    int getInputLatencyInSamples() override
    {
        if (captureStream != nullptr)
            if (int latency = getStreamLatencyInSamples (captureStream))
                return latency;

        return currentBufferSize;
    }

    String inputName, outputName;
    String inputNodeName, outputNodeName;

private:
    //==============================================================================
    StringArray inputChannelNames, outputChannelNames;

    static constexpr int maxFramesPerBlock = 16384;
    static constexpr int ringBufferCapacity = 16384;
    static constexpr int fallbackBufferSize = 1024;

    //==============================================================================
    struct spa_hook playbackListener;
    struct spa_hook captureListener;

    struct pw_stream* playbackStream = nullptr;
    struct pw_stream* captureStream = nullptr;
    pw_stream_events streamEvents;
    pw_stream_events streamEventsCapture;

    std::unique_ptr<PipeWireMainLoopThread> loopThread;
    struct pw_main_loop* mainLoop = nullptr;
    struct pw_context* context = nullptr;
    struct pw_core* core = nullptr;

    PipeWireCaptureBuffer captureBuffer;

    int numInputChannels = 0;
    int numOutputChannels = 0;

    std::atomic<double> currentSampleRate { 0 };
    std::atomic<int> currentBufferSize { 0 };

    std::atomic<bool> deviceIsOpen { false };
    std::atomic<bool> streamsReady { false };
    std::atomic<bool> streamsFailed { false };

    std::atomic<int> xruns { 0 };

    String lastError;
    AudioIODeviceCallback* callback = nullptr;
    CriticalSection callbackLock;
    BigInteger activeInputChannels, activeOutputChannels;

    // Scratch space for deinterleaving the single interleaved data plane that
    // PipeWire hands to a raw-format stream into per-channel buffers. The
    // capture side gets its own buffers so it never shares storage with the
    // playback side when the device is running duplex.
    HeapBlock<float> inputScratchStorage, outputScratchStorage, captureScratchStorage;
    HeapBlock<float*> inputScratchChannels, outputScratchChannels, captureScratchChannels;
    Array<int> activeInputChannelIndices, activeOutputChannelIndices;

    //==============================================================================
    class ServerFailureDispatcher final : private AsyncUpdater
    {
    public:
        explicit ServerFailureDispatcher (PipeWireAudioIODevice& device) : ref (device) {}
        ~ServerFailureDispatcher() override { cancelPendingUpdate(); }

        void trigger()
        {
            if (MessageManager::getInstance() != nullptr && MessageManager::getInstance()->isThisTheMessageThread())
                handleAsyncUpdate();
            else
                triggerAsyncUpdate();
        }

    private:
        void handleAsyncUpdate() override
        {
            ref.deviceFailed();
        }

        PipeWireAudioIODevice& ref;
    };

    ServerFailureDispatcher serverFailureDispatcher { *this };

    //==============================================================================
    String makeStreamName (bool isPlayback) const
    {
        if (isPlayback)
            return String (JUCE_PIPEWIRE_CLIENT_NAME);

        return String (JUCE_PIPEWIRE_CLIENT_NAME) + "-in";
    }

    struct pw_stream* createStream (const String& targetNodeName, const String& mediaName,
                                    const char* mediaCategory, enum pw_direction direction,
                                    const pw_stream_events& events, struct spa_hook& listener)
    {
        const bool isPlayback = (direction == PW_DIRECTION_OUTPUT);
        const String nodeName (makeStreamName (isPlayback));

        const String targetProp (targetNodeName.isNotEmpty() ? targetNodeName : String());

        spa_dict_item items[] = {
            spa_dict_item { PW_KEY_MEDIA_TYPE,     "Audio" },
            spa_dict_item { PW_KEY_MEDIA_CATEGORY, mediaCategory },
            spa_dict_item { PW_KEY_MEDIA_ROLE,     "Music" },
            spa_dict_item { PW_KEY_NODE_NAME,      nodeName.toRawUTF8() },
            spa_dict_item { PW_KEY_APP_NAME,       String (JUCE_PIPEWIRE_CLIENT_NAME).toRawUTF8() },
            spa_dict_item { PW_KEY_TARGET_OBJECT,  targetProp.toRawUTF8() }
        };

        const uint32_t numItems = (uint32_t) (targetProp.isNotEmpty() ? 6 : 5);
        spa_dict props = SPA_DICT_INIT (items, numItems);

        // Build the single format we're prepared to accept: 32-bit float at
        // whatever rate the graph is currently running at.
        uint8_t podBuffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT (podBuffer, sizeof podBuffer);

        const int numChannels = isPlayback ? numOutputChannels : numInputChannels;
        const StringArray& channelNames = isPlayback ? outputChannelNames : inputChannelNames;

        uint32_t positions[SPA_AUDIO_MAX_CHANNELS];

        for (int i = 0; i < jmin (numChannels, (int) SPA_AUDIO_MAX_CHANNELS); ++i)
            positions[i] = i < channelNames.size() ? getPipeWireAudioChannelPosition (channelNames[i])
                                                   : (uint32_t) SPA_AUDIO_CHANNEL_UNKNOWN;

        struct spa_audio_info_raw info = {};
        info.format   = SPA_AUDIO_FORMAT_F32;
        info.rate     = 0;
        info.channels = (uint32_t) numChannels;

        for (int i = 0; i < jmin (numChannels, (int) SPA_AUDIO_MAX_CHANNELS); ++i)
            info.position[i] = positions[i];

        const struct spa_pod* params[1];
        params[0] = (const struct spa_pod*) spa_format_audio_raw_build (&builder, SPA_PARAM_EnumFormat, &info);

        JUCE_PIPEWIRE_LOG ("Opening PipeWire stream '" + nodeName + "' with " + String (numChannels) + " channels");

        struct pw_stream* stream = juce::pw_stream_new (core, mediaName.toUTF8(), juce::pw_properties_new_dict (&props));

        if (stream == nullptr)
            return nullptr;

        juce::pw_stream_add_listener (stream, &listener, &events, this);

        const auto result = juce::pw_stream_connect (stream, direction, PW_ID_ANY,
                                                     (enum pw_stream_flags) (PW_STREAM_FLAG_AUTOCONNECT
                                                                             | PW_STREAM_FLAG_MAP_BUFFERS),
                                                     params, 1);

        if (result < 0)
        {
            juce::pw_stream_destroy (stream);
            return nullptr;
        }

        return stream;
    }


    void allocateChannelScratchSpace()
    {
        inputScratchStorage.malloc (jmax (1, numInputChannels) * maxFramesPerBlock);
        outputScratchStorage.malloc (jmax (1, numOutputChannels) * maxFramesPerBlock);
        captureScratchStorage.malloc (jmax (1, numInputChannels) * maxFramesPerBlock);
        inputScratchChannels.calloc (jmax (1, numInputChannels));
        outputScratchChannels.calloc (jmax (1, numOutputChannels));
        captureScratchChannels.calloc (jmax (1, numInputChannels));

        for (int i = 0; i < numInputChannels; ++i)
            inputScratchChannels[i] = inputScratchStorage + ((size_t) i * (size_t) maxFramesPerBlock);

        for (int i = 0; i < numInputChannels; ++i)
            captureScratchChannels[i] = captureScratchStorage + ((size_t) i * (size_t) maxFramesPerBlock);

        for (int i = 0; i < numOutputChannels; ++i)
            outputScratchChannels[i] = outputScratchStorage + ((size_t) i * (size_t) maxFramesPerBlock);

        activeInputChannelIndices.clear();
        activeOutputChannelIndices.clear();

        for (int i = 0; i < numInputChannels; ++i)
            if (activeInputChannels[i])
                activeInputChannelIndices.add (i);

        for (int i = 0; i < numOutputChannels; ++i)
            if (activeOutputChannels[i])
                activeOutputChannelIndices.add (i);
    }

    //==============================================================================
    static int getNumFramesInBuffer (const struct spa_buffer* buffer)
    {
        if (buffer == nullptr || buffer->n_datas == 0 || buffer->datas[0].chunk == nullptr)
            return 0;

        const auto& chunk = *buffer->datas[0].chunk;

        if (chunk.size == 0 || chunk.stride == 0)
            return 0;

        // For a single interleaved plane the stride is the size of a frame; for
        // planar data it's the size of a single sample. Either way, the number
        // of frames is size / stride.
        return (int) (chunk.size / chunk.stride);
    }

    // Copies the samples of one buffer into the interleaved per-channel scratch
    // space (handling both interleaved and planar layouts).
    void copyBufferToScratch (struct spa_buffer* buffer, int frames, int channels, float* const* scratch)
    {
        if (buffer == nullptr || frames <= 0)
            return;

        const bool planar = (buffer->n_datas > 1);

        if (! planar)
        {
            const auto& data = buffer->datas[0];

            if (data.data == nullptr || data.chunk == nullptr)
                return;

            const float* src = (const float*) ((const char*) data.data + data.chunk->offset);
            const int stride = jmax (1, channels);

            for (int f = 0; f < frames; ++f)
                for (int c = 0; c < channels; ++c)
                    scratch[c][f] = src[(size_t) f * (size_t) stride + (size_t) c];
        }
        else
        {
            for (int c = 0; c < channels && c < (int) buffer->n_datas; ++c)
            {
                const auto& data = buffer->datas[c];

                if (data.data == nullptr || data.chunk == nullptr)
                    continue;

                const float* src = (const float*) ((const char*) data.data + data.chunk->offset);
                memcpy (scratch[c], src, (size_t) frames * sizeof (float));
            }
        }
    }

    void copyScratchToBuffer (struct spa_buffer* buffer, int frames, int channels, float* const* scratch)
    {
        if (buffer == nullptr || frames <= 0)
            return;

        const bool planar = (buffer->n_datas > 1);

        if (! planar)
        {
            auto& data = buffer->datas[0];
            auto& chunk = *data.chunk;

            if (data.data == nullptr)
                return;

            float* dst = (float*) ((char*) data.data + chunk.offset);
            const int stride = jmax (1, channels);

            for (int f = 0; f < frames; ++f)
                for (int c = 0; c < channels; ++c)
                    dst[(size_t) f * (size_t) stride + (size_t) c] = scratch[c][f];

            chunk.stride = (uint32_t) (stride * (int) sizeof (float));
            chunk.size   = (uint32_t) ((size_t) frames * (size_t) chunk.stride);
        }
        else
        {
            for (int c = 0; c < channels && c < (int) buffer->n_datas; ++c)
            {
                auto& data = buffer->datas[c];
                auto& chunk = *data.chunk;

                if (data.data == nullptr)
                    continue;

                memcpy ((char*) data.data + chunk.offset, scratch[c], (size_t) frames * sizeof (float));

                chunk.stride = (uint32_t) sizeof (float);
                chunk.size   = (uint32_t) ((size_t) frames * sizeof (float));
            }
        }
    }

    //==============================================================================
    // Queries how many frames the graph wants the playback stream to produce in
    // the current cycle. Playback buffers don't carry a frame count until we
    // fill them, so this comes from the stream's clock instead.
    int getPlaybackFramesToFill()
    {
        if (playbackStream != nullptr)
        {
            struct pw_time time;
            std::memset (&time, 0, sizeof time);

            if (juce::pw_stream_get_time_n (playbackStream, &time, sizeof time) == 0 && time.size > 0)
                return (int) jmin<uint64_t> (time.size, (uint64_t) maxFramesPerBlock);
        }

        if (currentBufferSize > 0)
            return jmin (currentBufferSize.load(), maxFramesPerBlock);

        return 1024;
    }

    //==============================================================================
    // Handles a process event on the playback stream (or on the capture stream
    // for an input-only device). This is where the JUCE callback is invoked.
    void handleProcess (bool isPlayback)
    {
        struct pw_stream* stream = isPlayback ? playbackStream : captureStream;

        if (stream == nullptr)
            return;

        const bool duplex = (captureStream != nullptr && playbackStream != nullptr);
        float* inputChannelPtrs[128];
        float* outputChannelPtrs[128];

        struct pw_buffer* buffer;

        while ((buffer = juce::pw_stream_dequeue_buffer (stream)) != nullptr)
        {
            int numFrames = isPlayback ? getPlaybackFramesToFill()
                                       : jmin (getNumFramesInBuffer (buffer->buffer), maxFramesPerBlock);

            if (numFrames > 0)
            {
                currentBufferSize = numFrames;

                int numActiveIn = 0, numActiveOut = 0;

                if (isPlayback)
                {
                    // Pull the captured audio from the ring buffer. If the
                    // capture side hasn't produced this cycle yet, the missing
                    // frames are silence.
                    if (duplex)
                        captureBuffer.readBlock (inputScratchChannels.getData(), numFrames);

                    // Fill the output scratch space with silence so disabled
                    // channels stay quiet, then let the callback write to the
                    // enabled ones.
                    for (int c = 0; c < numOutputChannels; ++c)
                        zeromem (outputScratchChannels[c], (size_t) numFrames * sizeof (float));

                    const ScopedLock sl (callbackLock);

                    for (int i = 0; i < jmin (activeOutputChannelIndices.size(), 128); ++i)
                        outputChannelPtrs[numActiveOut++] = outputScratchChannels[activeOutputChannelIndices[i]];

                    if (duplex)
                        for (int i = 0; i < jmin (activeInputChannelIndices.size(), 128); ++i)
                            inputChannelPtrs[numActiveIn++] = inputScratchChannels[activeInputChannelIndices[i]];

                    if (callback != nullptr)
                        callback->audioDeviceIOCallbackWithContext (inputChannelPtrs, numActiveIn,
                                                                    outputChannelPtrs, numActiveOut,
                                                                    numFrames, {});
                }
                else
                {
                    // A capture-only device - the capture stream is the clock
                    // which drives the callback.
                    copyBufferToScratch (buffer->buffer, numFrames, numInputChannels,
                                         inputScratchChannels.getData());

                    const ScopedLock sl (callbackLock);

                    for (int i = 0; i < jmin (activeInputChannelIndices.size(), 128); ++i)
                        inputChannelPtrs[numActiveIn++] = inputScratchChannels[activeInputChannelIndices[i]];

                    if (callback != nullptr)
                        callback->audioDeviceIOCallbackWithContext (inputChannelPtrs, numActiveIn,
                                                                    nullptr, 0,
                                                                    numFrames, {});
                }

                if (isPlayback)
                {
                    // Write the output back out to the buffer.
                    copyScratchToBuffer (buffer->buffer, numFrames, numOutputChannels,
                                         outputScratchChannels.getData());
                }
            }

            juce::pw_stream_queue_buffer (stream, buffer);
        }
    }

    // Handles a process event on the capture stream. When the device is duplex
    // the captured audio is pushed into the ring buffer; the actual JUCE
    // callback happens on the playback side, so both directions stay aligned
    // in one graph cycle.
    void handleCaptureProcess()
    {
        if (playbackStream != nullptr)
        {
            // Duplex: buffer the capture data until the playback side runs.
            struct pw_buffer* buffer;

            while ((buffer = juce::pw_stream_dequeue_buffer (captureStream)) != nullptr)
            {
                const int numFrames = jmin (getNumFramesInBuffer (buffer->buffer), maxFramesPerBlock);

                if (numFrames > 0)
                {
                    copyBufferToScratch (buffer->buffer, numFrames, numInputChannels,
                                         captureScratchChannels.getData());

                    if (! captureBuffer.writeBlock (captureScratchChannels.getData(), numFrames))
                        xruns.fetch_add (1, std::memory_order_relaxed);
                }

                juce::pw_stream_queue_buffer (captureStream, buffer);
            }

            return;
        }

        // No playback stream - this device is input only, so the capture
        // stream drives the callback directly.
        handleProcess (false);
    }

    static void streamProcess (void* data)
    {
        static_cast<PipeWireAudioIODevice*> (data)->handleProcess (true);
    }

    static void streamProcessCapture (void* data)
    {
        static_cast<PipeWireAudioIODevice*> (data)->handleCaptureProcess();
    }

    static void streamStateChanged (void* data, enum pw_stream_state, enum pw_stream_state newState, const char* error)
    {
        auto* device = static_cast<PipeWireAudioIODevice*> (data);
        juce::ignoreUnused (error);

        JUCE_PIPEWIRE_LOG ("PipeWire stream state changed to " + String ((int) newState)
                           + (error != nullptr ? (" (" + String (error) + ")") : String()));

        if (newState == PW_STREAM_STATE_ERROR)
        {
            device->streamsFailed = true;

            // If the device has already been opened, deal with the failure on
            // the message thread so we never stop the PipeWire thread from one
            // of its own callbacks. If it's still being opened, the caller's
            // open() method will notice the failed flag instead.
            if (device->deviceIsOpen)
                device->serverFailureDispatcher.trigger();
        }
        else if (newState == PW_STREAM_STATE_STREAMING || newState == PW_STREAM_STATE_PAUSED)
        {
            device->streamsReady = true;
        }
    }

    static void streamParamChanged (void* data, uint32_t id, const struct spa_pod* param)
    {
        // A null param is delivered when the format is being removed, e.g. as
        // part of disconnecting the stream.
        if (id != SPA_PARAM_Format || param == nullptr)
            return;

        auto* device = static_cast<PipeWireAudioIODevice*> (data);

        // The negotiated format tells us the actual sample rate and channel
        // count that the graph is using.
        struct spa_audio_info_raw info;
        std::memset (&info, 0, sizeof info);

        if (spa_format_audio_raw_parse (param, &info) == 0)
        {
            if (info.rate > 0)
                device->currentSampleRate = info.rate;

            device->currentBufferSize = 0; // filled in from the first process callback
        }
    }

    void deviceFailed()
    {
        // Called on the message thread via the dispatcher, so that we never
        // try to join the PipeWire thread from one of its own callbacks.
        streamsFailed = true;
        stop();
        close();
    }

    int getStreamLatencyInSamples (struct pw_stream* stream)
    {
        struct pw_time time;
        std::memset (&time, 0, sizeof time);

        if (juce::pw_stream_get_time_n (stream, &time, sizeof time) == 0 && time.rate.denom > 0)
        {
            // time.delay is in nanoseconds; rate is expressed as 1/<samplerate>.
            const double rate = time.rate.num != 0 ? (double) time.rate.denom / (double) time.rate.num
                                                   : (double) time.rate.denom;

            if (time.delay > 0)
                return jmax (1, (int) ((double) time.delay * rate / 1000000000.0));
        }

        return 0;
    }

    bool streamsAreReady() const
    {
        if (streamsReady)
            return true;

        // If only one stream is needed, it's enough for that one to be ready.
        if (playbackStream == nullptr)
            return captureReady();
        if (captureStream == nullptr)
            return playbackReady();

        return playbackReady() && captureReady();
    }

    bool streamsHaveFailed() const { return streamsFailed; }

    bool playbackReady() const
    {
        if (playbackStream == nullptr)
            return true;

        const auto state = juce::pw_stream_get_state (playbackStream, nullptr);
        return state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING;
    }

    bool captureReady() const
    {
        if (captureStream == nullptr)
            return true;

        const auto state = juce::pw_stream_get_state (captureStream, nullptr);
        return state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PipeWireAudioIODevice)
};

//==============================================================================
class PipeWireAudioIODeviceType final : public AudioIODeviceType
{
public:
    PipeWireAudioIODeviceType()
        : AudioIODeviceType ("PipeWire")
    {}

    void scanForDevices() override
    {
        hasScanned = true;
        inputNames.clear();
        inputNodeNames.clear();
        inputChannelLists.clear();
        outputNames.clear();
        outputNodeNames.clear();
        outputChannelLists.clear();

        if (! juce_initialisePipeWire())
            return;

        PipeWireScanResults results;

        PipeWireRegistryScanner scanner;

        if (! scanner.scan (results))
        {
            JUCE_PIPEWIRE_LOG ("Unable to connect to the PipeWire server");
            return;
        }

        JUCE_PIPEWIRE_LOG ("Found " + String (results.sinks.size()) + " PipeWire sinks and "
                           + String (results.sources.size()) + " PipeWire sources");

        for (const auto& sink : results.sinks)
        {
            outputNames.add (sink.displayName);
            outputNodeNames.add (sink.nodeName);
            outputChannelLists.add (getChannelNames (sink.inputPorts));
        }

        for (const auto& source : results.sources)
        {
            inputNames.add (source.displayName);
            inputNodeNames.add (source.nodeName);
            inputChannelLists.add (getChannelNames (source.outputPorts));
        }
    }

    StringArray getDeviceNames (bool wantInputNames) const override
    {
        jassert (hasScanned); // need to call scanForDevices() before doing this
        return wantInputNames ? inputNames : outputNames;
    }

    int getDefaultDeviceIndex (bool) const override
    {
        jassert (hasScanned); // need to call scanForDevices() before doing this

        // There's no reliable way to discover which sink/source the session
        // manager considers to be the default without talking to its metadata,
        // so just point at the first one in the list.
        return 0;
    }

    bool hasSeparateInputsAndOutputs() const override    { return true; }

    int getIndexOfDevice (AudioIODevice* device, bool asInput) const override
    {
        jassert (hasScanned); // need to call scanForDevices() before doing this

        if (auto* d = dynamic_cast<PipeWireAudioIODevice*> (device))
            return asInput ? inputNames.indexOf (d->inputName)
                           : outputNames.indexOf (d->outputName);

        return -1;
    }

    AudioIODevice* createDevice (const String& outputDeviceName,
                                 const String& inputDeviceName) override
    {
        jassert (hasScanned); // need to call scanForDevices() before doing this

        const int inputIndex  = inputNames.indexOf (inputDeviceName);
        const int outputIndex = outputNames.indexOf (outputDeviceName);

        if (inputIndex < 0 && outputIndex < 0)
            return nullptr;

        const String inputNodeName  (inputIndex  >= 0 ? inputNodeNames [inputIndex]  : String());
        const String outputNodeName (outputIndex >= 0 ? outputNodeNames [outputIndex] : String());

        const StringArray inputChannels  (inputIndex  >= 0 ? inputChannelLists [inputIndex]  : StringArray());
        const StringArray outputChannels (outputIndex >= 0 ? outputChannelLists [outputIndex] : StringArray());

        return new PipeWireAudioIODevice (inputDeviceName, outputDeviceName,
                                          inputNodeName, outputNodeName,
                                          inputChannels, outputChannels);
    }

private:
    static StringArray getChannelNames (const Array<PipeWirePortDescription>& ports)
    {
        StringArray names;

        for (const auto& port : ports)
            names.add (port.channelName);

        names.appendNumbersToDuplicates (false, true);
        return names;
    }

    StringArray inputNames, outputNames;
    StringArray inputNodeNames, outputNodeNames;
    Array<StringArray> inputChannelLists, outputChannelLists;
    bool hasScanned = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PipeWireAudioIODeviceType)
};

} // namespace juce
