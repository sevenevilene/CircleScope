#pragma once

#include <juce_core/juce_core.h>
#include <cstring>

// Wire format shared by the plugin (sender), the cluster nodes and the
// control app. Everything is little-endian, which is what every machine
// involved (Apple Silicon, Intel, Raspberry Pi) uses natively.
//
//   UDP 47800  beacon   node -> broadcast, once a second, JSON:
//                       { "app": "circlescope-node", "name": ..., "audio": port,
//                         "control": port, "streams": [1, 2] }
//   UDP 47780+ the same beacon, sent by a node to loopback for listeners
//                       on its own machine (see BeaconReceiver)
//   UDP audio  stream   plugin -> each node that lists the plugin's stream:
//                       AudioHeader + interleaved float32 L/R, and ParamHeader
//                       + one float per entry of scope::paramTable
//   TCP control         control app <-> node, framed as
//                       [u32 length][u8 type][payload]; type 'J' is JSON text,
//                       type 'P' is a preview: [u8 output index][JPEG]
namespace net
{
    constexpr int beaconPort = 47800;
    constexpr int localBeaconPortBase = 47780;   // same-machine beacons, see BeaconReceiver
    constexpr int localBeaconPorts = 16;
    constexpr int audioPortBase = 47801;     // a node takes the first free port from here
    constexpr int controlPortBase = 47901;
    constexpr int portTries = 32;
    constexpr int maxStreams = 16;           // stream ids are 1..16; 0 means off
    constexpr int maxFramesPerPacket = 160;  // keeps a packet under one Ethernet MTU
    constexpr const char* nodeAppId = "circlescope-node";

    constexpr juce::uint32 audioMagic = 0x31415343;   // "CSA1"
    constexpr juce::uint32 paramMagic = 0x31505343;   // "CSP1"

    struct AudioHeader
    {
        juce::uint32 magic;
        juce::uint16 stream;
        juce::uint16 frames;
        juce::uint32 sampleRate;
        juce::uint32 reserved;
        juce::uint64 samplePosition;   // of the first frame; gaps mean lost packets
    };

    struct ParamHeader
    {
        juce::uint32 magic;
        juce::uint16 stream;
        juce::uint16 count;
    };

    static_assert (sizeof (AudioHeader) == 24 && sizeof (ParamHeader) == 8, "wire structs must not be padded");

    constexpr int maxPacketBytes = (int) sizeof (AudioHeader) + maxFramesPerPacket * 2 * (int) sizeof (float);
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
