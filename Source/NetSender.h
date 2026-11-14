#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <map>
#include <vector>
#include "NetProtocol.h"
#include "ScopeSource.h"

// Plugin side of the network link. Listens for node beacons and, while at
// least one stream is switched on, sends the displayed audio and the
// parameter values to every node (or wall) that shows one of those streams.
//
// It reads the same rings the scope draws from (they allow any number of
// readers), on its own thread, so the audio callback does no network work.
class NetSender : private juce::Thread
{
public:
    using StreamSwitches = std::array<const std::atomic<float>*, net::maxStreams>;

    // streamSwitches: one on/off value per stream (the raw values of host parameters).
    NetSender (const ScopeSource& sourceToSend, StreamSwitches streamSwitches)
        : Thread ("CircleScope net sender"), source (sourceToSend), switches (streamSwitches),
          senderId ((juce::uint16) juce::Random::getSystemRandom().nextInt (65536))
    {
        startThread();
    }

    ~NetSender() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (2000);
    }

    // Nodes currently receiving this instance.
    int getNumTargets() const noexcept { return numTargets.load(); }

private:
    struct Node
    {
        juce::String address;
        int audioPort = 0;
        juce::uint32 lastSeenMs = 0;
        juce::uint32 wants = 0;                         // stream mask, as in AudioHeader
        std::unique_ptr<juce::DatagramSocket> socket;   // one each: it caches the resolved address
    };

    void run() override
    {
        juce::uint32 lastParamSendMs = 0;
        float sentParams[scope::numParams] {};
        size_t readPosition = 0;
        bool streaming = false;
        std::vector<char> packet ((size_t) net::maxPacketBytes);
        float left[net::maxFramesPerPacket], right[net::maxFramesPerPacket];

        while (! threadShouldExit())
        {
            juce::uint32 mask = 0;
            for (int i = 0; i < net::maxStreams; ++i)
                if (switches[(size_t) i]->load() > 0.5f)
                    mask |= 1u << i;
            const auto now = juce::Time::getMillisecondCounter();

            readBeacons (now);

            if (mask == 0)
            {
                streaming = false;
                numTargets.store (0);
                wait (200);
                continue;
            }

            std::vector<Node*> targets;
            for (auto& [key, node] : nodes)
                if ((node.wants & mask) != 0 && now - node.lastSeenMs < 5000)
                    targets.push_back (&node);
            numTargets.store ((int) targets.size());

            // Ring 1 is pushed after ring 0, so its position is the safe one.
            const size_t writePosition = source.rings[1].getWritePosition();
            if (! streaming || writePosition - readPosition > ScopeSource::ringSize / 2)
                readPosition = writePosition;
            streaming = true;

            while (writePosition - readPosition > 0)
            {
                const int frames = (int) juce::jmin ((size_t) net::maxFramesPerPacket, writePosition - readPosition);

                if (! targets.empty())
                {
                    source.rings[0].read (readPosition, left, frames);
                    source.rings[1].read (readPosition, right, frames);

                    net::AudioHeader header { net::audioMagic, senderId, (juce::uint16) frames,
                                              (juce::uint32) juce::roundToInt (source.sampleRate.load()), mask,
                                              (juce::uint64) readPosition };
                    std::memcpy (packet.data(), &header, sizeof (header));
                    auto* samples = reinterpret_cast<float*> (packet.data() + sizeof (header));
                    for (int i = 0; i < frames; ++i)
                    {
                        samples[2 * i] = left[i];
                        samples[2 * i + 1] = right[i];
                    }

                    const int bytes = (int) sizeof (header) + frames * 2 * (int) sizeof (float);
                    for (auto* node : targets)
                        node->socket->write (node->address, node->audioPort, packet.data(), bytes);
                }

                readPosition += (size_t) frames;
            }

            // Parameters: at once when one changes, and twice a second regardless
            // so a node that just joined catches up.
            float params[scope::numParams];
            bool changed = false;
            for (int i = 0; i < scope::numParams; ++i)
            {
                params[i] = source.get (i);
                changed = changed || ! juce::exactlyEqual (params[i], sentParams[i]);
            }

            if (! targets.empty() && (changed || now - lastParamSendMs > 500))
            {
                net::ParamHeader header { net::paramMagic, senderId, (juce::uint16) scope::numParams };
                std::memcpy (packet.data(), &header, sizeof (header));
                std::memcpy (packet.data() + sizeof (header), params, sizeof (params));
                for (auto* node : targets)
                    node->socket->write (node->address, node->audioPort, packet.data(),
                                         (int) (sizeof (header) + sizeof (params)));

                std::memcpy (sentParams, params, sizeof (params));
                lastParamSendMs = now;
            }

            wait (3);
        }
    }

    void readBeacons (juce::uint32 now)
    {
        juce::String address;
        juce::var beacon;

        for (int i = 0; i < 64 && beacons.read (address, beacon); ++i)
        {
            const int audioPort = (int) beacon["audio"];
            if (audioPort <= 0 || audioPort > 65535)
                continue;

            auto& node = nodes[address + ":" + juce::String (audioPort)];
            if (node.socket == nullptr)
                node.socket = std::make_unique<juce::DatagramSocket> (false);
            node.address = address;
            node.audioPort = audioPort;
            node.lastSeenMs = now;

            node.wants = 0;
            if (const auto* list = beacon["streams"].getArray())
                for (const auto& s : *list)
                    if ((int) s >= 1 && (int) s <= net::maxStreams)
                        node.wants |= 1u << ((int) s - 1);
        }
    }

    const ScopeSource& source;
    const StreamSwitches switches;
    const juce::uint16 senderId;
    net::BeaconReceiver beacons;
    std::map<juce::String, Node> nodes;
    std::atomic<int> numTargets { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetSender)
};
