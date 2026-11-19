#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <bitset>
#include "NetProtocol.h"
#include "CurveAnalyser.h"

// Receiving side of the network link, shared by the cluster node and the
// wall. Every plugin instance heard on the network gets a slot of its own,
// holding the newest frame of its drawing and its parameter values: one
// layer. A stream is drawn from all the layers whose instance is on that
// stream. Nothing is analysed here: the frames arrive finished.
//
// Slots all exist for the whole run and are only ever handed from one
// instance to the next, so the pointers given out stay valid.
class StreamReceiver : private juce::Thread
{
public:
    static constexpr int maxLayers = 32;         // instances heard at once, all streams together
    static constexpr int maxLayersPerStream = 8;

    struct Layer
    {
        const scope::CurveFeed* feed = nullptr;
        juce::uint32 streamMask = 0;             // every stream the instance is on
    };
    using LayerList = std::array<Layer, maxLayersPerStream>;

    StreamReceiver() : Thread ("CircleScope receiver")
    {
        for (int i = 0; i <= net::maxStreams; ++i)
        {
            lastPacketMs[i].store (0);
            lostPackets[i].store (0);
        }
    }

    ~StreamReceiver() override
    {
        signalThreadShouldExit();
        stopThread (2000);
    }

    bool start()
    {
        for (int i = 0; i < net::portTries && audioPort == 0; ++i)
            if (socket.bindToPort (net::audioPortBase + i))
                audioPort = net::audioPortBase + i;

        if (audioPort != 0)
            startThread();
        return audioPort != 0;
    }

    int getAudioPort() const { return audioPort; }

    // The layers of a stream, bottom first: by the instances' Layer setting,
    // then in a fixed order of their own. Safe from any thread.
    int getLayers (int stream, LayerList& list) const
    {
        if (stream < 1 || stream > net::maxStreams)
            return 0;

        if (testTone.load())
        {
            list[0] = { &test.feed, allStreams };
            return 1;
        }

        struct Found { const Slot* slot; float order; };
        Found found[maxLayers];
        int count = 0;
        for (const auto& slot : slots)
            if ((slot.streamMask.load() & (1u << (stream - 1))) != 0)
                found[count++] = { &slot, slot.params.get (scope::pLayer) * 65536.0f + (float) slot.senderId.load() };

        std::sort (found, found + count, [] (const Found& a, const Found& b) { return a.order < b.order; });
        count = juce::jmin (count, maxLayersPerStream);
        for (int i = 0; i < count; ++i)
            list[(size_t) i] = { &found[i].slot->feed, found[i].slot->streamMask.load() };
        return count;
    }

    int getNumLayers (int stream) const
    {
        LayerList list;
        return getLayers (stream, list);
    }

    // Milliseconds since the last packet of a stream, or -1 if none yet.
    int getPacketAgeMs (int stream) const
    {
        const auto last = lastPacketMs[stream].load();
        return last == 0 ? -1 : (int) (juce::Time::getMillisecondCounter() - last);
    }

    // Frames of a stream that never arrived whole.
    int getLostPackets (int stream) const { return lostPackets[stream].load(); }

    std::atomic<bool> testTone { false };   // replaces everything from the network with the test signal

private:
    static constexpr juce::uint32 allStreams = (1u << net::maxStreams) - 1;
    static constexpr juce::uint32 silentAfterMs = 1500;   // an instance this quiet has gone

    struct Slot
    {
        ScopeParamSet params;
        scope::CurveFeed feed { params };
        std::atomic<juce::uint32> streamMask { 0 };   // 0: not drawn (free, or its instance went quiet)
        std::atomic<int> senderId { 0 };

        // Receiver thread only.
        juce::String address;
        bool used = false;
        juce::uint32 lastHeardMs = 0;

        // The frame being put together from its chunks.
        scope::FramePool pool;
        std::shared_ptr<scope::Frame> building;
        juce::uint16 frameNumber = 0;
        std::bitset<256> chunksHere;                  // one bit per chunk that has arrived
        int numChunksHere = 0;
        bool frameSeen = false;                       // frameNumber means something
    };

    // The local test tone, drawn like the plugin would draw it: this is the
    // one thing a receiver analyses itself.
    struct Test
    {
        ScopeSource source;
        scope::CurveFeed feed { source };
        CurveAnalyser analyser { source, feed };
        TestTone tone;
    };

    void run() override
    {
        std::vector<char> packet (2048);
        float left[512], right[512];
        double testClockMs = 0.0;

        while (! threadShouldExit())
        {
            if (socket.waitUntilReady (true, 5) > 0)
            {
                // Drain what has arrived, but come up for air under a flood.
                for (int i = 0; i < 512 && socket.waitUntilReady (true, 0) > 0; ++i)
                {
                    juce::String address;
                    int port = 0;
                    const int n = socket.read (packet.data(), (int) packet.size(), false, address, port);
                    if (n <= 0)
                        break;
                    handlePacket (address, packet.data(), n);
                }
            }

            const auto nowMs = juce::Time::getMillisecondCounter();
            for (auto& slot : slots)
                if (slot.streamMask.load() != 0 && nowMs - slot.lastHeardMs > silentAfterMs)
                    slot.streamMask.store (0);

            if (! testTone.load())
            {
                testClockMs = 0.0;
                continue;
            }

            // Generate as many samples as real time has advanced.
            constexpr double rate = 48000.0;
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (testClockMs <= 0.0 || now - testClockMs > 500.0)
                testClockMs = now - 5.0;

            int due = (int) ((now - testClockMs) * rate / 1000.0);
            testClockMs += due * 1000.0 / rate;
            while (due > 0)
            {
                const int n = juce::jmin (512, due);
                test.tone.render (left, right, n, rate);
                test.source.sampleRate.store (rate);
                test.source.rings[0].push (left, n);
                test.source.rings[1].push (right, n);
                due -= n;
            }
        }
    }

    // The slot of an instance: its own if it has one, else one nobody is
    // using, else the one that has been quiet longest. Null if all are busy.
    Slot* slotFor (const juce::String& address, int senderId, juce::uint32 nowMs)
    {
        Slot* spare = nullptr;
        for (auto& slot : slots)
        {
            if (slot.used && slot.senderId.load() == senderId && slot.address == address)
                return &slot;

            if (slot.streamMask.load() == 0 && (spare == nullptr || (spare->used && (! slot.used || slot.lastHeardMs < spare->lastHeardMs))))
                spare = &slot;
        }

        if (spare != nullptr)
        {
            spare->used = true;
            spare->address = address;
            spare->senderId.store (senderId);
            spare->lastHeardMs = nowMs;
            spare->building = nullptr;
            spare->frameSeen = false;
            spare->feed.publish (nullptr);
            for (int i = 0; i < scope::numParams; ++i)
                spare->params.local[i].store (scope::paramTable[i].def);
        }
        return spare;
    }

    void handlePacket (const juce::String& address, const char* data, int size)
    {
        juce::uint32 magic = 0;
        if (size < (int) sizeof (net::ParamHeader))
            return;
        std::memcpy (&magic, data, 4);
        const auto nowMs = juce::jmax ((juce::uint32) 1, juce::Time::getMillisecondCounter());

        if (magic == net::paramMagic)
        {
            net::ParamHeader header;
            std::memcpy (&header, data, sizeof (header));
            const int count = juce::jmin ((int) header.count, (int) scope::numParams);
            if (size < (int) sizeof (header) + (int) header.count * (int) sizeof (float))
                return;

            if (auto* slot = slotFor (address, header.sender, nowMs))
            {
                for (int i = 0; i < count; ++i)
                {
                    float value = 0.0f;
                    std::memcpy (&value, data + sizeof (header) + (size_t) i * sizeof (float), sizeof (float));
                    if (std::isfinite (value))
                        slot->params.local[i].store (juce::jlimit (scope::paramTable[i].min, scope::paramTable[i].max, value));
                }
            }
            return;
        }

        if (magic != net::frameMagic || size < (int) sizeof (net::FrameHeader))
            return;

        net::FrameHeader header;
        std::memcpy (&header, data, sizeof (header));
        const juce::uint32 mask = header.streamMask & allStreams;
        if (header.chunks < 1 || header.chunks > net::maxChunks || header.chunk >= header.chunks
            || header.panel >= (header.kind == 2 ? scope::maxFieldSources : 2))
            return;

        auto* slot = slotFor (address, header.sender, nowMs);
        if (slot == nullptr)
            return;
        slot->lastHeardMs = nowMs;

        auto forEachStream = [mask] (auto&& fn)
        {
            for (int stream = 1; stream <= net::maxStreams; ++stream)
                if ((mask & (1u << (stream - 1))) != 0)
                    fn (stream);
        };
        forEachStream ([&] (int stream) { lastPacketMs[stream].store (nowMs); });

        // A chunk of a frame we are not on: start that frame, unless it is
        // an older one straggling in. What was unfinished is lost.
        if (! slot->frameSeen || header.frame != slot->frameNumber)
        {
            if (slot->frameSeen && (juce::int16) (header.frame - slot->frameNumber) < 0)
                return;
            if (slot->building != nullptr)
                forEachStream ([&] (int stream) { ++lostPackets[stream]; });

            slot->building = slot->pool.acquire();
            slot->frameNumber = header.frame;
            slot->frameSeen = true;
            slot->chunksHere.reset();
            slot->numChunksHere = 0;
        }

        if (slot->building == nullptr)
            return;   // this frame is finished already
        auto& frame = *slot->building;
        const char* payload = data + sizeof (header);
        const int payloadSize = size - (int) sizeof (header);

        if (header.kind == 0)
        {
            net::FrameHead head;
            if (payloadSize < (int) sizeof (head))
                return;
            std::memcpy (&head, payload, sizeof (head));
            const int numGuides = juce::jmin ((int) head.numGuides, scope::maxGuidePlanes);
            const int numSources = juce::jmin ((int) head.numSources, scope::maxFieldSources);
            const bool eq = numSources > 0 && (head.flat & 4) != 0;
            const int eqBytes = eq ? (int) (sizeof (float) + sizeof (frame.curve)) : 0;
            if (head.numGuides > scope::maxGuidePlanes || head.numSources > scope::maxFieldSources
                || payloadSize < (int) sizeof (head) + numGuides * (int) sizeof (net::WirePlane)
                                     + numSources * (int) sizeof (net::WireSource) + (numSources > 0 ? (int) (sizeof (float) + sizeof (frame.bandColour)) : 0) + eqBytes)
                return;

            auto finite = [] (float v, float low, float high) { return std::isfinite (v) ? juce::jlimit (low, high, v) : 0.0f; };
            auto readPlane = [&finite] (const net::WirePlane& wire, scope::Mat3& plane, scope::Vec3& at)
            {
                for (int i = 0; i < 9; ++i)
                    plane.m[i] = finite (wire.plane[i], -1.0f, 1.0f);
                at = { finite (wire.at[0], -4.0f, 4.0f), finite (wire.at[1], -4.0f, 4.0f), finite (wire.at[2], -4.0f, 4.0f) };
            };
            const char* in = payload + sizeof (head);

            frame.layout = (scope::Layout) juce::jlimit (0, 3, (int) head.layout);
            frame.flat = (head.flat & 1) != 0;
            frame.rings = (head.flat & 2) != 0;
            frame.spectrum = (head.flat & 8) != 0;
            frame.shells = (head.flat & 16) != 0;
            frame.correlation = std::isfinite (head.correlation) ? juce::jlimit (-1.0f, 1.0f, head.correlation) : 1.0f;
            frame.numGuides = numGuides;
            for (int k = 0; k < numGuides; ++k, in += sizeof (net::WirePlane))
            {
                net::WirePlane wire;
                std::memcpy (&wire, in, sizeof (wire));
                readPlane (wire, frame.guides[k], frame.guidesAt[k]);
            }

            frame.numSources = numSources;
            for (int k = 0; k < numSources; ++k, in += sizeof (net::WireSource))
            {
                net::WireSource wire;
                std::memcpy (&wire, in, sizeof (wire));
                auto& s = frame.sources[k];
                readPlane (wire.where, s.plane, s.at);
                for (int band = 0; band < scope::fieldBands; ++band)
                    s.level[band] = finite (wire.level[band], 0.0f, 4.0f);
                s.fade = finite (wire.fade, 0.0f, 10.0f);
                for (int i = 0; i < scope::fieldCurvePoints; ++i)
                    s.fadeCurve[i] = 1.0f - (float) i / (float) (scope::fieldCurvePoints - 1);
                s.samples.resize ((size_t) (scope::fieldSamples * scope::fieldBands));   // keeps what has arrived already
            }
            if (numSources > 0)
            {
                float reach = 1.0f;
                std::memcpy (&reach, in, sizeof (reach));
                frame.reach = std::isfinite (reach) ? juce::jlimit (1.0e-3f, scope::fieldMaxReach, reach) : 1.0f;
                in += sizeof (reach);

                std::memcpy (frame.bandColour, in, sizeof (frame.bandColour));
                for (auto& colour : frame.bandColour)
                    for (auto& v : colour)
                        v = finite (v, 0.0f, 1.0f);
                in += sizeof (frame.bandColour);
            }

            frame.eq = eq;
            if (eq)
            {
                std::memcpy (&frame.eqWidth, in, sizeof (float));
                std::memcpy (frame.curve, in + sizeof (float), sizeof (frame.curve));
                frame.eqWidth = finite (frame.eqWidth, 0.1f, 20.0f);
                for (auto& v : frame.curve)
                    v = finite (v, 0.0f, 1.0f);
                in += eqBytes;
            }

            // The fade curves, if the sender has them.
            for (int i = 0; i < scope::fieldCurvePoints; ++i)
                frame.show[i] = (float) i / (float) (scope::fieldCurvePoints - 1);

            if (payload + payloadSize - in >= numSources * scope::fieldCurvePoints)
            {
                for (int k = 0; k < numSources; ++k)
                    for (auto& v : frame.sources[k].fadeCurve)
                        v = (float) (juce::uint8) *in++ / 255.0f;

                // And the curve of what is seen, likewise.
                if (numSources > 0 && payload + payloadSize - in >= scope::fieldCurvePoints)
                    for (auto& v : frame.show)
                        v = (float) (juce::uint8) *in++ / 255.0f;
            }
            for (int p = 0; p < 2; ++p)
            {
                frame.panels[p].points.resize ((size_t) juce::jmin ((int) head.points[p], scope::maxPanelPoints));
                frame.panels[p].secondStart = head.secondStart[p];
            }
        }
        else if (header.kind == 2)
        {
            const int first = header.first, count = header.count;
            if (count < 1 || count > net::maxSamplesPerChunk || first + count > scope::fieldSamples * scope::fieldBands
                || payloadSize < count * (int) sizeof (juce::int16))
                return;

            auto& samples = frame.sources[header.panel].samples;
            if ((int) samples.size() < first + count)
                samples.resize ((size_t) (first + count));   // the head, which has the size, is still to come
            for (int i = 0; i < count; ++i)
            {
                juce::int16 wire = 0;
                std::memcpy (&wire, payload + (size_t) i * sizeof (wire), sizeof (wire));
                samples[(size_t) (first + i)] = (float) wire / 32767.0f;
            }
        }
        else if (header.kind == 1)
        {
            const int first = header.first, count = header.count;
            if (count < 1 || count > net::maxPointsPerChunk || first + count > scope::maxPanelPoints
                || payloadSize < count * (int) sizeof (net::WirePoint))
                return;

            auto& points = frame.panels[header.panel].points;
            if ((int) points.size() < first + count)
                points.resize ((size_t) (first + count));   // the head, which has the size, is still to come

            for (int i = 0; i < count; ++i)
            {
                net::WirePoint wire;
                std::memcpy (&wire, payload + (size_t) i * sizeof (wire), sizeof (wire));
                constexpr float c = 1.0f / 16384.0f, u = 1.0f / 255.0f;
                points[(size_t) (first + i)] = { (float) wire.x * c, (float) wire.y * c, (float) wire.z * c,
                                                 (float) wire.r * u, (float) wire.g * u, (float) wire.b * u, (float) wire.a * u };
            }
        }
        else
        {
            return;
        }

        // All there: this is the frame to draw now.
        if (! slot->chunksHere.test (header.chunk))
        {
            slot->chunksHere.set (header.chunk);
            ++slot->numChunksHere;
        }
        if (slot->numChunksHere == header.chunks)
        {
            frame.buildTriangles();
            slot->feed.publish (std::move (slot->building));
            slot->building = nullptr;
            slot->streamMask.store (mask);
        }
    }

    juce::DatagramSocket socket { false };
    int audioPort = 0;
    std::array<Slot, maxLayers> slots;
    Test test;
    std::atomic<juce::uint32> lastPacketMs[net::maxStreams + 1];
    std::atomic<int> lostPackets[net::maxStreams + 1];
};
