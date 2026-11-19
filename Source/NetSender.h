#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <map>
#include <vector>
#include "NetProtocol.h"
#include "ScopeCurve.h"

// Plugin side of the network link. Listens for node beacons and, while at
// least one stream is switched on, sends the instance's drawing (the frames
// its analyser publishes to the feed) and its parameter values to every
// node or wall that shows one of those streams.
//
// It works on its own thread, so neither the audio callback nor the
// analyser does any network work.
class NetSender : private juce::Thread
{
public:
    using StreamSwitches = std::array<const std::atomic<float>*, net::maxStreams>;

    // streamSwitches: one on/off value per stream (the raw values of host parameters).
    NetSender (scope::CurveFeed& feedToSend, StreamSwitches streamSwitches)
        : Thread ("CircleScope net sender"), feed (feedToSend), switches (streamSwitches),
          senderId ((juce::uint16) juce::Random::getSystemRandom().nextInt (65536))
    {
        startThread();
    }

    ~NetSender() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (2000);
        feed.streamed.store (false);
    }

    // Nodes currently receiving this instance.
    int getNumTargets() const noexcept { return numTargets.load(); }

private:
    struct Node
    {
        juce::String address;
        int audioPort = 0;
        juce::uint32 lastSeenMs = 0;
        juce::uint32 wants = 0;                         // stream mask, as in FrameHeader
        std::unique_ptr<juce::DatagramSocket> socket;   // one each: it caches the resolved address
    };

    static constexpr double minFrameIntervalMs = 14.0;   // about sixty frames a second at most

    void run() override
    {
        juce::uint32 lastParamSendMs = 0, sentSerial = 0;
        double lastFrameSendMs = 0.0;
        juce::uint16 frameNumber = 0;
        float sentParams[scope::numParams] {};
        std::vector<char> packet ((size_t) net::maxPacketBytes);

        while (! threadShouldExit())
        {
            juce::uint32 mask = 0;
            for (int i = 0; i < net::maxStreams; ++i)
                if (switches[(size_t) i]->load() > 0.5f)
                    mask |= 1u << i;
            const auto now = juce::Time::getMillisecondCounter();

            readBeacons (now);

            std::vector<Node*> targets;
            for (auto& [key, node] : nodes)
                if ((node.wants & mask) != 0 && now - node.lastSeenMs < 5000)
                    targets.push_back (&node);
            numTargets.store ((int) targets.size());

            // With someone to send to, the analyser makes frames whether or
            // not a scope in the plugin is looking.
            feed.streamed.store (! targets.empty());

            if (targets.empty())
            {
                wait (mask == 0 ? 200 : 50);
                continue;
            }

            auto send = [&] (int bytes)
            {
                for (auto* node : targets)
                    node->socket->write (node->address, node->audioPort, packet.data(), bytes);
            };

            // Parameters: at once when one changes, and twice a second regardless
            // so a node that just joined catches up.
            float params[scope::numParams];
            bool changed = false;
            for (int i = 0; i < scope::numParams; ++i)
            {
                params[i] = feed.params.get (i);
                changed = changed || ! juce::exactlyEqual (params[i], sentParams[i]);
            }

            if (changed || now - lastParamSendMs > 500)
            {
                const net::ParamHeader header { net::paramMagic, senderId, (juce::uint16) scope::numParams };
                std::memcpy (packet.data(), &header, sizeof (header));
                std::memcpy (packet.data() + sizeof (header), params, sizeof (params));
                send ((int) (sizeof (header) + sizeof (params)));

                std::memcpy (sentParams, params, sizeof (params));
                lastParamSendMs = now;
            }

            // The newest frame, if there is one we have not sent.
            juce::uint32 serial = 0;
            const auto frame = feed.getLatest (&serial);
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            if (frame != nullptr && serial != sentSerial && nowMs - lastFrameSendMs >= minFrameIntervalMs)
            {
                sendFrame (*frame, ++frameNumber, mask, packet, send);
                sentSerial = serial;
                lastFrameSendMs = nowMs;
            }

            wait (2);
        }
    }

    template <typename Send>
    void sendFrame (const scope::Frame& frame, juce::uint16 number, juce::uint32 mask, std::vector<char>& packet, Send&& send)
    {
        int numPoints[2];
        int chunks = 1;
        for (int p = 0; p < 2; ++p)
        {
            numPoints[p] = juce::jmin ((int) frame.panels[p].points.size(), scope::maxPanelPoints);
            chunks += (numPoints[p] + net::maxPointsPerChunk - 1) / net::maxPointsPerChunk;
        }
        const int numSources = juce::jlimit (0, scope::maxFieldSources, frame.numSources);
        const int waveSamples = scope::fieldSamples * scope::fieldBands;
        const int chunksPerWave = (waveSamples + net::maxSamplesPerChunk - 1) / net::maxSamplesPerChunk;
        chunks += numSources * chunksPerWave;
        if (chunks > net::maxChunks)
            return;

        net::FrameHeader header { net::frameMagic, senderId, number, mask, 0, (juce::uint8) chunks, 0, 0, 0, 0 };

        // The head: what the frame is, and how much of it is coming.
        {
            const int numGuides = juce::jlimit (0, scope::maxGuidePlanes, frame.numGuides);
            const net::FrameHead head { (juce::uint8) frame.layout, (juce::uint8) ((frame.flat ? 1 : 0) | (frame.rings ? 2 : 0) | (frame.eq ? 4 : 0) | (frame.spectrum ? 8 : 0) | (frame.shells ? 16 : 0)), (juce::uint8) numGuides, (juce::uint8) numSources,
                                        frame.correlation,
                                        { (juce::uint16) numPoints[0], (juce::uint16) numPoints[1] },
                                        { (juce::int16) frame.panels[0].secondStart, (juce::int16) frame.panels[1].secondStart } };

            auto* out = packet.data();
            std::memcpy (out, &header, sizeof (header));
            std::memcpy (out + sizeof (header), &head, sizeof (head));
            out += sizeof (header) + sizeof (head);

            auto wirePlane = [] (const scope::Mat3& plane, const scope::Vec3& at)
            {
                net::WirePlane wire;
                std::memcpy (wire.plane, plane.m, sizeof (wire.plane));
                wire.at[0] = at.x;
                wire.at[1] = at.y;
                wire.at[2] = at.z;
                return wire;
            };

            for (int k = 0; k < numGuides; ++k)
            {
                const auto wire = wirePlane (frame.guides[k], frame.guidesAt[k]);
                std::memcpy (out, &wire, sizeof (wire));
                out += sizeof (wire);
            }
            for (int k = 0; k < numSources; ++k)
            {
                const auto& s = frame.sources[k];
                net::WireSource wire { wirePlane (s.plane, s.at), {}, s.fade };
                std::memcpy (wire.level, s.level, sizeof (wire.level));
                std::memcpy (out, &wire, sizeof (wire));
                out += sizeof (wire);
            }
            if (numSources > 0)
            {
                std::memcpy (out, &frame.reach, sizeof (float));
                std::memcpy (out + sizeof (float), frame.bandColour, sizeof (frame.bandColour));
                out += sizeof (float) + sizeof (frame.bandColour);
            }
            if (numSources > 0 && frame.eq)
            {
                std::memcpy (out, &frame.eqWidth, sizeof (float));
                std::memcpy (out + sizeof (float), frame.curve, sizeof (frame.curve));
                out += sizeof (float) + sizeof (frame.curve);
            }
            for (int k = 0; k < numSources; ++k)
                for (float v : frame.sources[k].fadeCurve)
                    *out++ = (char) (juce::uint8) juce::roundToInt (juce::jlimit (0.0f, 1.0f, v) * 255.0f);
            if (numSources > 0)
                for (float v : frame.show)
                    *out++ = (char) (juce::uint8) juce::roundToInt (juce::jlimit (0.0f, 1.0f, v) * 255.0f);
            send ((int) (out - packet.data()));
        }

        auto unit = [] (float v) { return (juce::uint8) juce::roundToInt (juce::jlimit (0.0f, 1.0f, v) * 255.0f); };
        auto coord = [] (float v) { return (juce::int16) juce::roundToInt (juce::jlimit (-1.999f, 1.999f, v) * 16384.0f); };

        header.kind = 1;
        for (int p = 0; p < 2; ++p)
        {
            for (int first = 0; first < numPoints[p]; first += net::maxPointsPerChunk)
            {
                const int count = juce::jmin (net::maxPointsPerChunk, numPoints[p] - first);
                ++header.chunk;
                header.panel = (juce::uint8) p;
                header.first = (juce::uint16) first;
                header.count = (juce::uint16) count;
                std::memcpy (packet.data(), &header, sizeof (header));

                for (int i = 0; i < count; ++i)
                {
                    const auto& point = frame.panels[p].points[(size_t) (first + i)];
                    const net::WirePoint wire { coord (point.x), coord (point.y), coord (point.z),
                                                unit (point.r), unit (point.g), unit (point.b), unit (point.a) };
                    std::memcpy (packet.data() + sizeof (header) + (size_t) i * sizeof (wire), &wire, sizeof (wire));
                }
                send ((int) (sizeof (header) + (size_t) count * sizeof (net::WirePoint)));
            }
        }

        // The waves of a field's sources.
        header.kind = 2;
        for (int k = 0; k < numSources; ++k)
        {
            const auto& samples = frame.sources[k].samples;
            for (int first = 0; first < waveSamples; first += net::maxSamplesPerChunk)
            {
                const int count = juce::jmin (net::maxSamplesPerChunk, waveSamples - first);
                ++header.chunk;
                header.panel = (juce::uint8) k;
                header.first = (juce::uint16) first;
                header.count = (juce::uint16) count;
                std::memcpy (packet.data(), &header, sizeof (header));

                for (int i = 0; i < count; ++i)
                {
                    const auto index = (size_t) (first + i);
                    const float v = index < samples.size() ? samples[index] : 0.0f;
                    const auto wire = (juce::int16) juce::roundToInt (juce::jlimit (-1.0f, 1.0f, v) * 32767.0f);
                    std::memcpy (packet.data() + sizeof (header) + (size_t) i * sizeof (wire), &wire, sizeof (wire));
                }
                send ((int) (sizeof (header) + (size_t) count * sizeof (juce::int16)));
            }
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

    scope::CurveFeed& feed;
    const StreamSwitches switches;
    const juce::uint16 senderId;
    net::BeaconReceiver beacons;
    std::map<juce::String, Node> nodes;
    std::atomic<int> numTargets { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetSender)
};
