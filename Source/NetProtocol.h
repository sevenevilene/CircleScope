#pragma once

#include <juce_core/juce_core.h>
#include <cstring>

// Wire format shared by the plugin (sender), the receivers (cluster nodes
// and CircleScope Wall) and the control app. Everything is little-endian, which is what every machine
// involved (Apple Silicon, Intel, Raspberry Pi) uses natively.
//
//   UDP 47800  beacon   node -> broadcast, once a second, JSON:
//                       { "app": "circlescope-node", "name": ..., "audio": port,
//                         "control": port, "streams": [1, 2] }
//                       ("audio" is the port for the stream below; the name
//                       is from when that was audio.) CircleScope Wall sends
//                       the same beacon without "control", so plugins find
//                       it and the control app leaves it alone.
//   UDP 47780+ the same beacon, sent by a node to loopback for listeners
//                       on its own machine (see BeaconReceiver)
//   UDP stream          plugin -> each node that lists one of the plugin's
//                       streams. The plugin sends its finished drawing, not
//                       audio: about sixty frames a second, each the points
//                       of its curves in 3D with their colours, or the
//                       sources of its wave field with their waves
//                       (scope::Frame). A frame is too big for one packet,
//                       so it goes as chunks, every one a FrameHeader and
//                       then a FrameHead (the frame's layout, guides,
//                       sources and sizes), a run of WirePoints of one
//                       panel, or a run of samples of one source's wave. A
//                       receiver draws a frame once it has all its chunks
//                       and drops one that stays incomplete. Besides that,
//                       ParamHeader + one float per entry of
//                       scope::paramTable says how to show it. A receiver
//                       treats every instance it hears on a stream as one
//                       layer of that stream.
//   TCP control         control app <-> node, framed as
//                       [u32 length][u8 type][payload]; type 'J' is JSON text,
//                       type 'P' is a preview: [u8 output index][JPEG]
namespace net
{
    constexpr int beaconPort = 47800;
    constexpr int localBeaconPortBase = 47780;   // same-machine beacons, see BeaconReceiver
    constexpr int localBeaconPorts = 16;
    constexpr int audioPortBase = 47801;     // for the stream; a node takes the first free port from here
    constexpr int controlPortBase = 47901;
    constexpr int portTries = 32;
    constexpr int maxStreams = 16;           // stream ids are 1..16; 0 means off
    constexpr const char* nodeAppId = "circlescope-node";

    constexpr juce::uint32 paramMagic = 0x33505343;   // "CSP3"
    constexpr juce::uint32 frameMagic = 0x34465343;   // "CSF4"

    constexpr int maxPointsPerChunk = 140;   // keeps a packet under one Ethernet MTU
    constexpr int maxSamplesPerChunk = 700;  // of a wave, likewise
    constexpr int maxChunks = 255;           // of one frame: a field of eight sources needs about 190

    struct ParamHeader
    {
        juce::uint32 magic;
        juce::uint16 sender;           // random per plugin instance; with the address, names the layer
        juce::uint16 count;
    };

    struct FrameHeader
    {
        juce::uint32 magic;
        juce::uint16 sender;
        juce::uint16 frame;            // counts up; all the chunks of a frame carry the same
        juce::uint32 streamMask;       // bit (n - 1) set: this instance is on stream n
        juce::uint8 chunk, chunks;     // this chunk's number, and how many the frame has
        juce::uint8 panel;             // for points: the panel they belong to; for samples: the source
        juce::uint8 kind;              // 0: a FrameHead follows; 1: WirePoints; 2: samples of a wave, int16 (1/32767)
        juce::uint16 first, count;     // for points and samples: where they go, and how many
    };

    // Then numGuides WirePlanes, then numSources WireSources and, if there
    // are sources, one float, the reach of their waves, and the colours of
    // the bands of pitch (scope::fieldBands times r, g, b as floats). With
    // the EQ on, then its width (a float) and its curve
    // (scope::fieldCurvePoints floats). Every source has scope::fieldBands
    // times scope::fieldSamples samples.
    struct FrameHead
    {
        juce::uint8 layout;            // scope::Layout
        juce::uint8 flat;              // bit 0: flat; bit 1: a field of rings, not dots; bit 2: the field's EQ is on
        juce::uint8 numGuides;
        juce::uint8 numSources;        // of a wave field; 0 for curves
        float correlation;
        juce::uint16 points[2];        // in each panel
        juce::int16 secondStart[2];    // where each panel's second curve begins, or -1
    };

    struct WirePlane
    {
        float plane[9];                // scope::Mat3
        float at[3];                   // its centre
    };

    struct WireSource
    {
        WirePlane where;
        float level[8];                // of each band of pitch (scope::fieldBands)
        float fade;
    };

    // Coordinates in 1/16384 (so -2 to 2), colour and alpha in 1/255.
    struct WirePoint
    {
        juce::int16 x, y, z;
        juce::uint8 r, g, b, a;
    };

    static_assert (sizeof (ParamHeader) == 8 && sizeof (FrameHeader) == 20 && sizeof (FrameHead) == 16 && sizeof (WirePoint) == 10
                   && sizeof (WirePlane) == 48 && sizeof (WireSource) == 84,
                   "wire structs must not be padded");

    constexpr int maxPacketBytes = 1472;
    constexpr int maxFrameBytes = 8 * 1024 * 1024;

    //==========================================================================
    inline bool writeAll (juce::StreamingSocket& socket, const void* data, int size)
    {
        auto* bytes = static_cast<const char*> (data);
        while (size > 0)
        {
            const int n = socket.write (bytes, size);
            if (n <= 0)
                return false;
            bytes += n;
            size -= n;
        }
        return true;
    }

    inline bool writeFrame (juce::StreamingSocket& socket, char type, const void* payload, int size)
    {
        char header[5];
        const juce::uint32 length = (juce::uint32) size + 1;
        std::memcpy (header, &length, 4);
        header[4] = type;
        return writeAll (socket, header, 5) && (size == 0 || writeAll (socket, payload, size));
    }

    inline bool writeJson (juce::StreamingSocket& socket, const juce::var& value)
    {
        const auto text = juce::JSON::toString (value, true);
        return writeFrame (socket, 'J', text.toRawUTF8(), (int) text.getNumBytesAsUTF8());
    }

    // Blocks until a whole frame has arrived. False means the link is dead.
    inline bool readFrame (juce::StreamingSocket& socket, char& type, juce::MemoryBlock& payload)
    {
        char header[5];
        if (socket.read (header, 5, true) != 5)
            return false;

        juce::uint32 length = 0;
        std::memcpy (&length, header, 4);
        if (length < 1 || length > (juce::uint32) maxFrameBytes)
            return false;

        type = header[4];
        payload.setSize (length - 1);
        return length == 1 || socket.read (payload.getData(), (int) length - 1, true) == (int) length - 1;
    }

    inline juce::var parseJson (const void* data, size_t size)
    {
        return juce::JSON::parse (juce::String::fromUTF8 (static_cast<const char*> (data), (int) size));
    }

    //==========================================================================
    // Hears node beacons, for the plugin and the control app. Two sockets:
    //   - the shared broadcast port, for nodes elsewhere on the network;
    //   - a loopback port of its own, for nodes on this machine. A firewall
    //     (macOS's, for one) can drop broadcasts between local programs, and
    //     a loopback datagram only reaches one listener per port, so every
    //     listener takes its own port from a small range and a node sends its
    //     beacon to each port in that range.
    // Use from one thread.
    class BeaconReceiver
    {
    public:
        bool isListening() const { return broadcastBound || localBound; }

        // Next waiting beacon, if any. Nodes on this machine are reported at
        // 127.0.0.1 whichever way their beacon came, so each is seen once.
        bool read (juce::String& address, juce::var& beacon)
        {
            bind();
            char text[2048];

            for (auto* socket : { &localSocket, &broadcastSocket })
            {
                if ((socket == &localSocket ? localBound : broadcastBound) && socket->waitUntilReady (true, 0) > 0)
                {
                    int port = 0;
                    const int n = socket->read (text, (int) sizeof (text), false, address, port);
                    if (n <= 0)
                        continue;

                    beacon = parseJson (text, (size_t) n);
                    if (beacon["app"].toString() != nodeAppId)
                        continue;

                    if (juce::IPAddress::getAllAddresses().contains (juce::IPAddress (address)))
                        address = "127.0.0.1";
                    return true;
                }
            }
            return false;
        }

    private:
        void bind()
        {
            const auto now = juce::Time::getMillisecondCounter();
            if ((broadcastBound && localBound) || (lastAttemptMs != 0 && now - lastAttemptMs < 2000))
                return;
            lastAttemptMs = now;

            if (! broadcastBound)
            {
                broadcastSocket.setEnablePortReuse (true);   // shared with other listeners
                broadcastBound = broadcastSocket.bindToPort (beaconPort);
            }

            for (int i = 0; i < localBeaconPorts && ! localBound; ++i)
                localBound = localSocket.bindToPort (localBeaconPortBase + i, "127.0.0.1");
        }

        juce::DatagramSocket broadcastSocket { false }, localSocket { false };
        bool broadcastBound = false, localBound = false;
        juce::uint32 lastAttemptMs = 0;
    };
}
