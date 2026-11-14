#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include "NetProtocol.h"
#include "ScopeSource.h"

// Receiving side of the network link, shared by the cluster node and the
// wall. Every plugin instance heard on the network gets a slot of its own,
// holding its samples and its parameter values: one layer. A stream is drawn
// from all the layers whose instance is on that stream.
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
        const ScopeSource* source = nullptr;
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
            list[0] = { &testSlot.source, allStreams };
            return 1;
        }

        struct Found { const Slot* slot; float order; };
        Found found[maxLayers];
        int count = 0;
        for (const auto& slot : slots)
            if ((slot.streamMask.load() & (1u << (stream - 1))) != 0)
                found[count++] = { &slot, slot.source.get (scope::pLayer) * 65536.0f + (float) slot.senderId.load() };

        std::sort (found, found + count, [] (const Found& a, const Found& b) { return a.order < b.order; });
        count = juce::jmin (count, maxLayersPerStream);
        for (int i = 0; i < count; ++i)
            list[(size_t) i] = { &found[i].slot->source, found[i].slot->streamMask.load() };
        return count;
    }

    int getNumLayers (int stream) const
    {
        LayerList list;
        return getLayers (stream, list);
    }

    // Milliseconds since the last audio packet of a stream, or -1 if none yet.
    int getPacketAgeMs (int stream) const
    {
        const auto last = lastPacketMs[stream].load();
        return last == 0 ? -1 : (int) (juce::Time::getMillisecondCounter() - last);
    }

    int getLostPackets (int stream) const { return lostPackets[stream].load(); }

    std::atomic<bool> testTone { false };   // replaces all network audio with the test signal

private:
    static constexpr juce::uint32 allStreams = (1u << net::maxStreams) - 1;
    static constexpr juce::uint32 silentAfterMs = 1500;   // an instance this quiet has gone

    struct Slot
    {
        ScopeSource source;
        std::atomic<juce::uint32> streamMask { 0 };   // 0: not drawn (free, or its instance went quiet)
        std::atomic<int> senderId { 0 };

        // Receiver thread only.
        juce::String address;
        bool used = false;
        juce::uint32 lastHeardMs = 0;
        juce::uint64 nextPosition = 0;
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
                for (int i = 0; i < 256 && socket.waitUntilReady (true, 0) > 0; ++i)
                {
                    juce::String address;
                    int port = 0;
                    const int n = socket.read (packet.data(), (int) packet.size(), false, address, port);
                    if (n <= 0)
                        break;
                    handlePacket (address, packet.data(), n, left, right);
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
                tone.render (left, right, n, rate);
                testSlot.source.sampleRate.store (rate);
                testSlot.source.rings[0].push (left, n);
                testSlot.source.rings[1].push (right, n);
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
            spare->nextPosition = 0;
            for (int i = 0; i < scope::numParams; ++i)
                spare->source.local[i].store (scope::paramTable[i].def);
        }
        return spare;
    }

    void handlePacket (const juce::String& address, const char* data, int size, float* left, float* right)
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
                        slot->source.local[i].store (juce::jlimit (scope::paramTable[i].min, scope::paramTable[i].max, value));
                }
            }
            return;
        }

        if (magic != net::audioMagic || size < (int) sizeof (net::AudioHeader))
            return;

        net::AudioHeader header;
        std::memcpy (&header, data, sizeof (header));
        const int frames = header.frames;
        const juce::uint32 mask = header.streamMask & allStreams;
        if (frames < 1 || frames > net::maxFramesPerPacket
            || size < (int) sizeof (header) + frames * 2 * (int) sizeof (float))
            return;

        auto* slot = slotFor (address, header.sender, nowMs);
        if (slot == nullptr)
            return;

        // Count gaps; drop packets that arrive late or twice. A big jump back
        // means the sender restarted, so follow it.
        bool gap = false;
        if (slot->nextPosition != 0 && header.samplePosition != slot->nextPosition)
        {
            if (header.samplePosition < slot->nextPosition && slot->nextPosition - header.samplePosition < 96000)
                return;
            gap = true;
        }
        slot->nextPosition = header.samplePosition + (juce::uint64) frames;
        slot->lastHeardMs = nowMs;

        for (int stream = 1; stream <= net::maxStreams; ++stream)
        {
            if ((mask & (1u << (stream - 1))) != 0)
            {
                lastPacketMs[stream].store (nowMs);
                if (gap)
                    ++lostPackets[stream];
            }
        }

        const char* payload = data + sizeof (header);
        for (int i = 0; i < frames; ++i)
        {
            std::memcpy (&left[i], payload + (size_t) (2 * i) * sizeof (float), sizeof (float));
            std::memcpy (&right[i], payload + (size_t) (2 * i + 1) * sizeof (float), sizeof (float));
        }

        slot->source.sampleRate.store ((double) juce::jlimit ((juce::uint32) 8000, (juce::uint32) 384000, header.sampleRate));
        slot->source.rings[0].push (left, frames);
        slot->source.rings[1].push (right, frames);
        slot->streamMask.store (mask);
    }

    juce::DatagramSocket socket { false };
    int audioPort = 0;
    std::array<Slot, maxLayers> slots;
    Slot testSlot;
    std::atomic<juce::uint32> lastPacketMs[net::maxStreams + 1];
    std::atomic<int> lostPackets[net::maxStreams + 1];
    TestTone tone;
};
