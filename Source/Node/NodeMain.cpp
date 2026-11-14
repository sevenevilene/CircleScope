// CircleScopeNode: one machine of the display cluster.
//
// It opens a clean full-screen scope on every display (or one window across
// all of them), receives audio and parameters from CircleScope plugin
// instances over the network, and draws its configured part of the picture.
// It announces itself with a broadcast beacon so plugins and the control app
// find it without any configuration, and serves live previews and remote
// configuration to the control app.
//
// Keys:  Esc / Q  quit      T  local test tone      I  identify this node

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>
#include <array>
#include <csignal>
#include "../NetProtocol.h"
#include "../ScopeComponent.h"

namespace
{
//==============================================================================
// What one output (window) shows: a stream, and which cell of a wall of
// cols x rows equal screens this output covers. A 1x1 wall is the whole picture.
struct OutputConfig
{
    int stream = 1, cols = 1, rows = 1, col = 0, row = 0, spanW = 1, spanH = 1;

    void sanitise()
    {
        stream = juce::jlimit (1, net::maxStreams, stream);
        cols = juce::jlimit (1, 64, cols);
        rows = juce::jlimit (1, 64, rows);
        col = juce::jlimit (0, cols - 1, col);
        row = juce::jlimit (0, rows - 1, row);
        spanW = juce::jlimit (1, cols - col, spanW);
        spanH = juce::jlimit (1, rows - row, spanH);
    }

    ScopeComponent::View view() const
    {
        return { (float) col / (float) cols, (float) row / (float) rows,
                 (float) spanW / (float) cols, (float) spanH / (float) rows };
    }

    juce::DynamicObject::Ptr toObject() const
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("stream", stream);
        o->setProperty ("cols", cols);
        o->setProperty ("rows", rows);
        o->setProperty ("col", col);
        o->setProperty ("row", row);
        o->setProperty ("spanw", spanW);
        o->setProperty ("spanh", spanH);
        return o;
    }

    // Takes whichever of the fields are present.
    void apply (const juce::var& v)
    {
        auto take = [&v] (const char* key, int& field) { if (v.hasProperty (key)) field = (int) v[key]; };
        take ("stream", stream);
        take ("cols", cols);
        take ("rows", rows);
        take ("col", col);
        take ("row", row);
        take ("spanw", spanW);
        take ("spanh", spanH);
        sanitise();
    }
};

//==============================================================================
// Receives every stream into its own ScopeSource. All sixteen exist for the
// whole run, so an output can switch stream by swapping a pointer.
class StreamReceiver : private juce::Thread
{
public:
    StreamReceiver() : Thread ("CircleScope receiver")
    {
        for (int i = 1; i <= net::maxStreams; ++i)
        {
            sources[(size_t) i] = std::make_unique<ScopeSource>();
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

    ScopeSource& getSource (int stream) { return *sources[(size_t) juce::jlimit (1, net::maxStreams, stream)]; }
    int getAudioPort() const { return audioPort; }

    // Milliseconds since the last audio packet of a stream, or -1 if none yet.
    int getPacketAgeMs (int stream) const
    {
        const auto last = lastPacketMs[stream].load();
        return last == 0 ? -1 : (int) (juce::Time::getMillisecondCounter() - last);
    }

    int getLostPackets (int stream) const { return lostPackets[stream].load(); }

    std::atomic<bool> testTone { false };   // replaces all network audio with the test signal

private:
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
                    const int n = socket.read (packet.data(), (int) packet.size(), false);
                    if (n <= 0)
                        break;
                    handlePacket (packet.data(), n, left, right);
                }
            }

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
                for (int s = 1; s <= net::maxStreams; ++s)
                {
                    sources[(size_t) s]->sampleRate.store (rate);
                    sources[(size_t) s]->rings[0].push (left, n);
                    sources[(size_t) s]->rings[1].push (right, n);
                }
                due -= n;
            }
        }
    }

    void handlePacket (const char* data, int size, float* left, float* right)
    {
        juce::uint32 magic = 0;
        if (size < (int) sizeof (net::ParamHeader))
            return;
        std::memcpy (&magic, data, 4);

        if (magic == net::paramMagic)
        {
            net::ParamHeader header;
            std::memcpy (&header, data, sizeof (header));
            const int count = juce::jmin ((int) header.count, (int) scope::numParams);
            if (header.stream < 1 || header.stream > net::maxStreams
                || size < (int) sizeof (header) + (int) header.count * (int) sizeof (float))
                return;

            auto& source = *sources[header.stream];
            for (int i = 0; i < count; ++i)
            {
                float value = 0.0f;
                std::memcpy (&value, data + sizeof (header) + (size_t) i * sizeof (float), sizeof (float));
                if (std::isfinite (value))
                    source.local[i].store (juce::jlimit (scope::paramTable[i].min, scope::paramTable[i].max, value));
            }
            return;
        }

        if (magic != net::audioMagic || size < (int) sizeof (net::AudioHeader))
            return;

        net::AudioHeader header;
        std::memcpy (&header, data, sizeof (header));
        const int frames = header.frames;
        if (header.stream < 1 || header.stream > net::maxStreams || frames < 1 || frames > net::maxFramesPerPacket
            || size < (int) sizeof (header) + frames * 2 * (int) sizeof (float))
            return;

        const int stream = header.stream;
        lastPacketMs[stream].store (juce::jmax ((juce::uint32) 1, juce::Time::getMillisecondCounter()));

        // Count gaps; drop packets that arrive late or twice. A big jump back
        // means the sender restarted, so follow it.
        auto& expected = nextPosition[stream];
        if (expected != 0 && header.samplePosition != expected)
        {
            if (header.samplePosition < expected && expected - header.samplePosition < 96000)
                return;
            ++lostPackets[stream];
        }
        expected = header.samplePosition + (juce::uint64) frames;

        if (testTone.load())
            return;

        const char* payload = data + sizeof (header);
        for (int i = 0; i < frames; ++i)
        {
            std::memcpy (&left[i], payload + (size_t) (2 * i) * sizeof (float), sizeof (float));
            std::memcpy (&right[i], payload + (size_t) (2 * i + 1) * sizeof (float), sizeof (float));
        }

        auto& source = *sources[(size_t) stream];
        source.sampleRate.store ((double) juce::jlimit ((juce::uint32) 8000, (juce::uint32) 384000, header.sampleRate));
        source.rings[0].push (left, frames);
        source.rings[1].push (right, frames);
    }

    juce::DatagramSocket socket { false };
    int audioPort = 0;
    std::array<std::unique_ptr<ScopeSource>, net::maxStreams + 1> sources;
    std::atomic<juce::uint32> lastPacketMs[net::maxStreams + 1];
    std::atomic<int> lostPackets[net::maxStreams + 1];
    juce::uint64 nextPosition[net::maxStreams + 1] {};
    TestTone tone;
};

//==============================================================================
// TCP side of the node: sends status and previews to every connected control
// app and passes their commands to the message thread. One thread does it all.
class ControlServer : private juce::Thread
{
public:
    explicit ControlServer (std::function<void (juce::var)> commandHandler)
        : Thread ("CircleScope control"), onCommand (std::move (commandHandler)) {}

    ~ControlServer() override
    {
        signalThreadShouldExit();
        stopThread (3000);
    }

    bool start()
    {
        for (int i = 0; i < net::portTries && port == 0; ++i)
            if (listener.createListener (net::controlPortBase + i))
                port = net::controlPortBase + i;

        if (port != 0)
            startThread();
        return port != 0;
    }

    int getPort() const { return port; }
    bool hasClients() const { return numClients.load() > 0; }

    void setStatus (const juce::String& json)
    {
        const juce::ScopedLock lock (pendingLock);
        pendingStatus = json;
        statusPending = true;
    }

    // Called on a render thread. Keeps only the newest frame per output.
    void submitPreview (int output, const juce::Image& image)
    {
        const juce::ScopedLock lock (pendingLock);
        if (output >= 0 && output < (int) pendingPreviews.size())
            pendingPreviews[(size_t) output] = image;
    }

private:
    void run() override
    {
        juce::OwnedArray<juce::StreamingSocket> clients;
        juce::String lastStatus;
        juce::JPEGImageFormat jpeg;
        jpeg.setQuality (0.75f);

        while (! threadShouldExit())
        {
            bool newClient = false;
            if (listener.waitUntilReady (true, clients.isEmpty() ? 100 : 15) > 0)
            {
                if (auto* client = listener.waitForNextConnection())
                {
                    clients.add (client);
                    newClient = true;
                }
            }

            for (int i = clients.size(); --i >= 0;)
            {
                auto* client = clients[i];
                int ready = 0;
                while ((ready = client->waitUntilReady (true, 0)) > 0)
                {
                    char type = 0;
                    juce::MemoryBlock payload;
                    if (! net::readFrame (*client, type, payload))
                    {
                        ready = -1;
                        break;
                    }

                    if (type == 'J')
                        juce::MessageManager::callAsync ([handler = onCommand, command = net::parseJson (payload.getData(), payload.getSize())]
                                                         { handler (command); });
                }

                if (ready < 0)
                    clients.remove (i);
            }

            // Collect what is waiting, then send without holding the lock.
            std::array<juce::Image, maxOutputs> previews;
            bool sendStatus = newClient;
            {
                const juce::ScopedLock lock (pendingLock);
                std::swap (previews, pendingPreviews);
                if (statusPending)
                {
                    lastStatus = pendingStatus;
                    statusPending = false;
                    sendStatus = true;
                }
            }

            if (! clients.isEmpty())
            {
                for (int output = 0; output < maxOutputs; ++output)
                {
                    if (! previews[(size_t) output].isValid())
                        continue;

                    juce::MemoryOutputStream frame;
                    frame.writeByte ((char) output);
                    jpeg.writeImageToStream (previews[(size_t) output], frame);
                    for (int i = clients.size(); --i >= 0;)
                        if (! net::writeFrame (*clients[i], 'P', frame.getData(), (int) frame.getDataSize()))
                            clients.remove (i);
                }

                if (sendStatus && lastStatus.isNotEmpty())
                    for (int i = clients.size(); --i >= 0;)
                        if (! net::writeFrame (*clients[i], 'J', lastStatus.toRawUTF8(), (int) lastStatus.getNumBytesAsUTF8()))
                            clients.remove (i);
            }

            numClients.store (clients.size());
        }
    }

public:
    static constexpr int maxOutputs = 8;

private:
    std::function<void (juce::var)> onCommand;
    juce::StreamingSocket listener;
    int port = 0;
    std::atomic<int> numClients { 0 };

    juce::CriticalSection pendingLock;
    std::array<juce::Image, maxOutputs> pendingPreviews;
    juce::String pendingStatus;
    bool statusPending = false;
};

//==============================================================================
// One output: a window holding nothing but a scope.
class OutputWindow : public juce::Component
{
public:
    OutputWindow (ScopeSource& source, juce::Rectangle<int> bounds, bool fullScreen, const juce::String& title,
                  std::function<bool (const juce::KeyPress&)> keyHandler)
        : scope (source, false), onKey (std::move (keyHandler))
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);
        setName (title);
        addAndMakeVisible (scope);

        if (fullScreen)
        {
            setMouseCursor (juce::MouseCursor::NoCursor);
            scope.setMouseCursor (juce::MouseCursor::NoCursor);
            addToDesktop (0);
        }
        else
        {
            using Peer = juce::ComponentPeer;
            addToDesktop (Peer::windowHasTitleBar | Peer::windowIsResizable | Peer::windowHasCloseButton
                          | Peer::windowHasMinimiseButton | Peer::windowAppearsOnTaskbar);
        }

        setBounds (bounds);
        setVisible (true);
        toFront (true);
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colours::black); }
    void resized() override { scope.setBounds (getLocalBounds()); }
    bool keyPressed (const juce::KeyPress& key) override { return onKey (key); }
    void userTriedToCloseWindow() override { juce::JUCEApplicationBase::quit(); }

    ScopeComponent scope;

private:
    std::function<bool (const juce::KeyPress&)> onKey;
};

// Big label shown over an output for a few seconds, to tell screens apart.
class IdentifyOverlay : public juce::Component
{
public:
    IdentifyOverlay (juce::Rectangle<int> screenArea, juce::String titleText, juce::String detailText)
        : title (std::move (titleText)), detail (std::move (detailText))
    {
        setOpaque (true);
        setAlwaysOnTop (true);
        setInterceptsMouseClicks (false, false);
        addToDesktop (juce::ComponentPeer::windowIsTemporary);
        setBounds (screenArea.withSizeKeepingCentre (juce::jmin (760, screenArea.getWidth()),
                                                     juce::jmin (280, screenArea.getHeight())));
        setVisible (true);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff101014));
        g.setColour (juce::Colours::white);
        g.drawRect (getLocalBounds(), 3);

        auto area = getLocalBounds().reduced (24);
        g.setFont (juce::FontOptions (84.0f, juce::Font::bold));
        g.drawFittedText (title, area.removeFromTop (area.getHeight() / 2), juce::Justification::centred, 1);
        g.setFont (juce::FontOptions (30.0f));
        g.drawFittedText (detail, area, juce::Justification::centred, 3);
    }

private:
    juce::String title, detail;
};

//==============================================================================
class NodeApplication : public juce::JUCEApplication,
                        private juce::Timer
{
public:
    const juce::String getApplicationName() override { return "CircleScopeNode"; }
    const juce::String getApplicationVersion() override { return "0.2.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
       #if ! JUCE_WINDOWS
        std::signal (SIGPIPE, SIG_IGN);   // a control app that vanishes must not kill the node
       #endif

        const auto args = getCommandLineParameterArray();
        auto option = [&args] (const char* name) { const int i = args.indexOf (name); return i >= 0 ? args[i + 1] : juce::String(); };

        if (args.contains ("--help") || args.contains ("-h"))
        {
            std::cout << "CircleScopeNode [options]\n"
                         "  --windowed       ordinary windows instead of full screen (for testing)\n"
                         "  --outputs N      number of windows in --windowed mode (default 1)\n"
                         "  --geometry X,Y,W,H  position and size of the first --windowed window\n"
                         "  --span           one full-screen output across all displays instead of one each\n"
                         "  --name NAME      node name shown in the control app (default: host name)\n"
                         "  --config FILE    where the output settings are kept\n"
                         "  --test           start with the local test tone on\n"
                         "Keys: Esc/Q quit, T test tone, I identify\n";
            quit();
            return;
        }

        const bool windowed = args.contains ("--windowed");
        nodeName = option ("--name").isNotEmpty() ? option ("--name") : juce::SystemStats::getComputerName();
        configFile = option ("--config").isNotEmpty()
                         ? juce::File::getCurrentWorkingDirectory().getChildFile (option ("--config"))
                         : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                               .getChildFile ("CircleScope").getChildFile ("node.json");
        receiver.testTone.store (args.contains ("--test"));

        if (! receiver.start())
            juce::Logger::writeToLog ("CircleScopeNode: no free UDP port for audio from " + juce::String (net::audioPortBase));

        server = std::make_unique<ControlServer> ([this] (juce::var command) { handleCommand (command); });
        if (! server->start())
            juce::Logger::writeToLog ("CircleScopeNode: no free TCP port for control from " + juce::String (net::controlPortBase));

        // Decide where the outputs go.
        std::vector<juce::Rectangle<int>> areas;
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        if (windowed)
        {
            const int count = juce::jlimit (1, (int) ControlServer::maxOutputs, juce::jmax (1, option ("--outputs").getIntValue()));
            const auto geometry = juce::StringArray::fromTokens (option ("--geometry"), ",", "");
            const auto first = geometry.size() == 4 ? juce::Rectangle<int> (geometry[0].getIntValue(), geometry[1].getIntValue(),
                                                                            juce::jmax (160, geometry[2].getIntValue()),
                                                                            juce::jmax (90, geometry[3].getIntValue()))
                                                    : juce::Rectangle<int> (80, 80, 960, 540);
            for (int i = 0; i < count; ++i)
                areas.push_back (first.translated (40 * i, 40 * i));
        }
        else if (args.contains ("--span"))
        {
            areas.push_back (displays.getTotalBounds (false));
        }
        else
        {
            for (const auto& display : displays.displays)
                if ((int) areas.size() < ControlServer::maxOutputs)
                    areas.push_back (display.totalArea);
        }

        const auto saved = juce::JSON::parse (configFile);
        for (size_t i = 0; i < areas.size(); ++i)
        {
            auto& output = outputs.emplace_back();
            output.area = areas[i];
            if (const auto* list = saved["outputs"].getArray(); list != nullptr && (int) i < list->size())
                output.config.apply (list->getReference ((int) i));

            const int index = (int) i;
            output.window = std::make_unique<OutputWindow> (receiver.getSource (output.config.stream), areas[i], ! windowed,
                                                            nodeName + " - output " + juce::String (index + 1),
                                                            [this] (const juce::KeyPress& key) { return handleKey (key); });
            output.window->scope.setView (output.config.view());
            output.window->scope.onPreview = [this, index] (const juce::Image& image) { server->submitPreview (index, image); };
        }

       #if JUCE_MAC
        if (! windowed && outputs.size() == 1)
            juce::Desktop::getInstance().setKioskModeComponent (outputs[0].window.get(), false);
       #endif
        if (! outputs.empty())
            outputs[0].window->grabKeyboardFocus();

        juce::Logger::writeToLog ("CircleScopeNode \"" + nodeName + "\": " + juce::String ((int) outputs.size()) + " output(s), audio UDP "
                                  + juce::String (receiver.getAudioPort()) + ", control TCP " + juce::String (server->getPort())
                                  + ", config " + configFile.getFullPathName());

        timerCallback();
        startTimer (250);
    }

    void shutdown() override
    {
        stopTimer();
        juce::Desktop::getInstance().setKioskModeComponent (nullptr, false);
        overlays.clear();
        server.reset();
        outputs.clear();
    }

    void systemRequestedQuit() override { quit(); }

private:
    struct Output
    {
        OutputConfig config;
        juce::Rectangle<int> area;
        std::unique_ptr<OutputWindow> window;
    };

    bool handleKey (const juce::KeyPress& key)
    {
        const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
        if (key == juce::KeyPress::escapeKey || c == 'q')
        {
            quit();
            return true;
        }
        if (c == 't')
        {
            receiver.testTone.store (! receiver.testTone.load());
            return true;
        }
        if (c == 'i')
        {
            identify();
            return true;
        }
        return false;
    }

    void handleCommand (const juce::var& command)
    {
        const auto name = command["cmd"].toString();

        if (name == "set")
        {
            const int index = (int) command["output"];
            if (index < 0 || index >= (int) outputs.size())
                return;

            auto& output = outputs[(size_t) index];
            output.config.apply (command);
            output.window->scope.setSource (receiver.getSource (output.config.stream));
            output.window->scope.setView (output.config.view());
            saveConfig();
        }
        else if (name == "identify")
        {
            identify();
        }
        else if (name == "test")
        {
            receiver.testTone.store ((bool) command["on"]);
        }

        publishStatus();
    }

    void identify()
    {
        overlays.clear();
        for (size_t i = 0; i < outputs.size(); ++i)
        {
            const auto& c = outputs[i].config;
            auto detail = "output " + juce::String ((int) i + 1) + "   stream " + juce::String (c.stream)
                          + "\nwall " + juce::String (c.cols) + " x " + juce::String (c.rows)
                          + ", column " + juce::String (c.col + 1) + ", row " + juce::String (c.row + 1)
                          + "\n" + juce::IPAddress::getLocalAddress().toString();
            overlays.push_back (std::make_unique<IdentifyOverlay> (outputs[i].window->getScreenBounds(), nodeName, detail));
        }
        identifyUntilMs = juce::Time::getMillisecondCounter() + 5000;
    }

    void saveConfig()
    {
        juce::Array<juce::var> list;
        for (const auto& output : outputs)
            list.add (juce::var (output.config.toObject().get()));

        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty ("outputs", list);
        configFile.getParentDirectory().createDirectory();
        configFile.replaceWithText (juce::JSON::toString (juce::var (root.get())));
    }

    void publishStatus()
    {
        juce::Array<juce::var> list;
        juce::String renderer;
        for (const auto& output : outputs)
        {
            auto o = output.config.toObject();
            const float scale = (float) juce::Desktop::getInstance().getDisplays().getDisplayForRect (output.window->getScreenBounds())->scale;
            o->setProperty ("width", juce::roundToInt ((float) output.window->getWidth() * scale));
            o->setProperty ("height", juce::roundToInt ((float) output.window->getHeight() * scale));
            o->setProperty ("fps", juce::roundToInt (output.window->scope.getFramesPerSecond()));
            o->setProperty ("rxAgeMs", receiver.getPacketAgeMs (output.config.stream));
            o->setProperty ("lost", receiver.getLostPackets (output.config.stream));
            list.add (juce::var (o.get()));
            renderer = output.window->scope.getRendererName();
        }

        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty ("name", nodeName);
        root->setProperty ("renderer", renderer);
        root->setProperty ("test", receiver.testTone.load());
        root->setProperty ("outputs", list);
        server->setStatus (juce::JSON::toString (juce::var (root.get()), true));
    }

    void sendBeacon()
    {
        juce::Array<juce::var> streams;
        for (const auto& output : outputs)
            streams.addIfNotAlreadyThere (output.config.stream);

        juce::DynamicObject::Ptr beacon = new juce::DynamicObject();
        beacon->setProperty ("app", net::nodeAppId);
        beacon->setProperty ("name", nodeName);
        beacon->setProperty ("audio", receiver.getAudioPort());
        beacon->setProperty ("control", server->getPort());
        beacon->setProperty ("streams", streams);
        const auto text = juce::JSON::toString (juce::var (beacon.get()), true);

        // The limited broadcast only leaves by the default interface, so also
        // address each interface's own subnet.
        juce::StringArray destinations { "255.255.255.255" };
        for (const auto& address : juce::IPAddress::getAllAddresses())
            if (address != juce::IPAddress::local())
                destinations.addIfNotAlreadyThere (juce::IPAddress::getInterfaceBroadcastAddress (address).toString());

        for (const auto& destination : destinations)
            if (destination != "0.0.0.0")
                beaconSocket.write (destination, net::beaconPort, text.toRawUTF8(), (int) text.getNumBytesAsUTF8());

        // Listeners on this machine each have their own loopback port.
        for (int i = 0; i < net::localBeaconPorts; ++i)
            beaconSocket.write ("127.0.0.1", net::localBeaconPortBase + i, text.toRawUTF8(), (int) text.getNumBytesAsUTF8());
    }

    void timerCallback() override
    {
        if (! overlays.empty() && juce::Time::getMillisecondCounter() > identifyUntilMs)
            overlays.clear();

        // Previews cost a framebuffer read-back, so only make them when watched.
        for (auto& output : outputs)
            output.window->scope.setPreviewIntervalMs (server->hasClients() ? 125 : 0);

        if (++tick % 4 == 1)
        {
            sendBeacon();
            publishStatus();
        }
    }

    juce::String nodeName;
    juce::File configFile;
    StreamReceiver receiver;
    std::unique_ptr<ControlServer> server;
    std::vector<Output> outputs;
    std::vector<std::unique_ptr<IdentifyOverlay>> overlays;
    juce::DatagramSocket beaconSocket { true };
    juce::uint32 identifyUntilMs = 0;
    int tick = 0;
};
}

START_JUCE_APPLICATION (NodeApplication)
