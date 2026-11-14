// CircleScopeControl: the control center for a CircleScope cluster.
//
// Finds nodes by their broadcast beacons, connects to each one, and shows a
// live preview of every output in a grid, so the cluster can be watched and
// set up without a screen attached to any node. Select a tile to choose its
// stream and its place in a video wall.

#include <juce_gui_basics/juce_gui_basics.h>
#include <csignal>
#include <map>
#include <tuple>
#include "../NetProtocol.h"

namespace
{
const juce::Colour background (0xff121215), panelColour (0xff1c1c21), accent (0xff1d9e75), warning (0xffe24b4a);

struct OutputState
{
    int stream = 1, cols = 1, rows = 1, col = 0, row = 0, spanW = 1, spanH = 1;
    int width = 0, height = 0, fps = 0, rxAgeMs = -1, lost = 0;
    juce::Image preview;
};

//==============================================================================
// The link to one node: a thread that keeps a TCP connection up, plus the
// node's last known state, which only the message thread touches.
class NodeLink : private juce::Thread
{
public:
    NodeLink (juce::String nodeAddress, int controlPort, std::function<void()> changeCallback)
        : Thread ("CircleScope node link"), address (std::move (nodeAddress)), port (controlPort),
          onChange (std::move (changeCallback))
    {
        startThread();
    }

    ~NodeLink() override
    {
        masterReference.clear();
        signalThreadShouldExit();
        notify();
        stopThread (4000);
    }

    void send (const juce::var& command)
    {
        const juce::ScopedLock lock (outgoingLock);
        outgoing.add (command);
    }

    // Message-thread state.
    const juce::String address;
    const int port;
    juce::String name, renderer;
    bool connected = false, testTone = false;
    juce::uint32 lastBeaconMs = 0;
    std::vector<OutputState> outputs;

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            juce::StreamingSocket socket;
            if (socket.connect (address, port, 2000))
            {
                setConnected (true);
                serve (socket);
                setConnected (false);
            }
            wait (1000);
        }
    }

    void serve (juce::StreamingSocket& socket)
    {
        while (! threadShouldExit())
        {
            const int ready = socket.waitUntilReady (true, 20);
            if (ready < 0)
                return;

            if (ready > 0)
            {
                char type = 0;
                juce::MemoryBlock payload;
                if (! net::readFrame (socket, type, payload))
                    return;

                if (type == 'J')
                    handleStatus (net::parseJson (payload.getData(), payload.getSize()));
                else if (type == 'P' && payload.getSize() > 1)
                    handlePreview ((int) (juce::uint8) payload[0],
                                   juce::ImageFileFormat::loadFrom (juce::addBytesToPointer (payload.getData(), 1), payload.getSize() - 1));
            }

            juce::Array<juce::var> toSend;
            {
                const juce::ScopedLock lock (outgoingLock);
                toSend.swapWith (outgoing);
            }
            for (const auto& command : toSend)
                if (! net::writeJson (socket, command))
                    return;
        }
    }

    // Everything below hops to the message thread before touching state.
    void setConnected (bool isConnected)
    {
        juce::MessageManager::callAsync ([link = juce::WeakReference<NodeLink> (this), isConnected]
        {
            if (link != nullptr)
            {
                link->connected = isConnected;
                link->onChange();
            }
        });
    }

    void handleStatus (juce::var status)
    {
        juce::MessageManager::callAsync ([link = juce::WeakReference<NodeLink> (this), status]
        {
            if (link == nullptr)
                return;

            link->name = status["name"].toString();
            link->renderer = status["renderer"].toString();
            link->testTone = (bool) status["test"];

            if (const auto* list = status["outputs"].getArray())
            {
                link->outputs.resize ((size_t) list->size());
                for (int i = 0; i < list->size(); ++i)
                {
                    const auto& o = list->getReference (i);
                    auto& state = link->outputs[(size_t) i];
                    state.stream = (int) o["stream"];
                    state.cols = (int) o["cols"];
                    state.rows = (int) o["rows"];
                    state.col = (int) o["col"];
                    state.row = (int) o["row"];
                    state.spanW = (int) o["spanw"];
                    state.spanH = (int) o["spanh"];
                    state.width = (int) o["width"];
                    state.height = (int) o["height"];
                    state.fps = (int) o["fps"];
                    state.rxAgeMs = (int) o["rxAgeMs"];
                    state.lost = (int) o["lost"];
                }
            }
            link->onChange();
        });
    }

    void handlePreview (int output, juce::Image image)
    {
        if (! image.isValid())
            return;

        juce::MessageManager::callAsync ([link = juce::WeakReference<NodeLink> (this), output, image]
        {
            if (link != nullptr && output < (int) link->outputs.size())
            {
                link->outputs[(size_t) output].preview = image;
                link->onChange();
            }
        });
    }

    std::function<void()> onChange;
    juce::CriticalSection outgoingLock;
    juce::Array<juce::var> outgoing;

    JUCE_DECLARE_WEAK_REFERENCEABLE (NodeLink)
};

//==============================================================================
// Hears the nodes' beacons and reports each one on the message thread.
class BeaconListener : private juce::Thread
{
public:
    explicit BeaconListener (std::function<void (juce::String address, int controlPort, juce::String name)> callback)
        : Thread ("CircleScope beacons"), onBeacon (std::move (callback))
    {
        startThread();
    }

    ~BeaconListener() override
    {
        signalThreadShouldExit();
        stopThread (2000);
    }

    std::atomic<bool> listening { false };

private:
    void run() override
    {
        net::BeaconReceiver receiver;
        juce::String address;
        juce::var beacon;

        while (! threadShouldExit())
        {
            while (receiver.read (address, beacon))
            {
                const int controlPort = (int) beacon["control"];
                if (controlPort > 0 && controlPort <= 65535)
                    juce::MessageManager::callAsync ([callback = onBeacon, address, controlPort, name = beacon["name"].toString()]
                                                     { callback (address, controlPort, name); });
            }

            listening.store (receiver.isListening());
            wait (100);
        }
    }

    std::function<void (juce::String, int, juce::String)> onBeacon;
};

//==============================================================================
// One output of one node: its live preview and a caption.
class Tile : public juce::Component
{
public:
    Tile (NodeLink& nodeLink, int outputIndex) : link (nodeLink), output (outputIndex) {}

    std::function<void()> onClick;
    bool selected = false;

    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().reduced (4);
        auto caption = area.removeFromBottom (40);
        const bool haveState = output < (int) link.outputs.size();
        const bool live = link.connected && haveState;

        g.setColour (juce::Colours::black);
        g.fillRect (area);

        if (haveState && link.outputs[(size_t) output].preview.isValid())
        {
            g.setOpacity (live ? 1.0f : 0.3f);
            g.drawImage (link.outputs[(size_t) output].preview, area.toFloat(), juce::RectanglePlacement::centred);
            g.setOpacity (1.0f);
        }

        if (! live)
        {
            g.setColour (warning);
            g.setFont (juce::FontOptions (15.0f));
            g.drawText ("offline", area, juce::Justification::centred);
        }

        g.setColour (selected ? accent : juce::Colours::white.withAlpha (0.15f));
        g.drawRect (area, selected ? 2 : 1);

        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText ((link.name.isNotEmpty() ? link.name : link.address) + "  /  output " + juce::String (output + 1),
                    caption.removeFromTop (20), juce::Justification::centredLeft);

        if (haveState)
        {
            const auto& s = link.outputs[(size_t) output];
            const bool receiving = s.rxAgeMs >= 0 && s.rxAgeMs < 1000;
            auto text = "stream " + juce::String (s.stream) + (link.testTone ? " (test tone)" : receiving ? "" : " (no signal)");
            if (s.cols * s.rows > 1)
                text << "   wall " << s.cols << "x" << s.rows << " @ " << (s.col + 1) << "," << (s.row + 1);
            text << "   " << s.width << "x" << s.height << "   " << s.fps << " fps";

            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.setFont (juce::FontOptions (12.5f));
            g.drawText (text, caption, juce::Justification::centredLeft);
        }
    }

    NodeLink& link;
    const int output;
};

//==============================================================================
class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    MainComponent()
    {
        setOpaque (true);

        for (int i = 1; i <= net::maxStreams; ++i)
            streamBox.addItem ("Stream " + juce::String (i), i);
        streamBox.onChange = [this] { sendSetting ("stream", streamBox.getSelectedId()); };

        auto setUpNumber = [this] (Number& n, const char* text, const char* key, bool oneBased)
        {
            n.label.setText (text, juce::dontSendNotification);
            n.label.setJustificationType (juce::Justification::centredRight);
            n.slider.setSliderStyle (juce::Slider::IncDecButtons);
            n.slider.setRange (1.0, 64.0, 1.0);
            n.slider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 34, 22);
            n.slider.onValueChange = [this, &n, key, oneBased] { sendSetting (key, (int) n.slider.getValue() - (oneBased ? 1 : 0)); };
            addAndMakeVisible (n.label);
            addAndMakeVisible (n.slider);
        };
        setUpNumber (cols, "Wall cols", "cols", false);
        setUpNumber (rows, "rows", "rows", false);
        setUpNumber (col, "Column", "col", true);
        setUpNumber (row, "row", "row", true);
        setUpNumber (spanW, "Span cols", "spanw", false);
        setUpNumber (spanH, "rows", "spanh", false);

        identifyButton.onClick = [this] { if (auto* link = selectedLink()) link->send (command ("identify")); };
        testButton.onClick = [this]
        {
            if (auto* link = selectedLink())
            {
                auto c = command ("test");
                c.getDynamicObject()->setProperty ("on", testButton.getToggleState());
                link->send (c);
            }
        };

        selectionLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        rendererLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.5f));
        rendererLabel.setJustificationType (juce::Justification::centredRight);
        for (juce::Component* c : std::initializer_list<juce::Component*> { &selectionLabel, &rendererLabel, &streamBox, &identifyButton, &testButton })
            addAndMakeVisible (c);

        beacons = std::make_unique<BeaconListener> ([this] (juce::String address, int port, juce::String name)
                                                    { nodeSeen (address, port, name); });
        setSize (1360, 820);
        startTimer (100);
        refreshInspector();
    }

    ~MainComponent() override
    {
        stopTimer();
        beacons.reset();
        tiles.clear();
        links.clear();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (background);

        auto header = getLocalBounds().removeFromTop (headerHeight).reduced (16, 0);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        g.drawText ("CircleScope Control", header, juce::Justification::centredLeft);

        int online = 0;
        for (auto* link : links)
            online += link->connected ? 1 : 0;
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (juce::FontOptions (13.5f));
        g.drawText (juce::String (online) + " of " + juce::String (links.size()) + " nodes online, "
                        + juce::String (tiles.size()) + " outputs",
                    header, juce::Justification::centredRight);

        g.setColour (panelColour);
        g.fillRect (getLocalBounds().removeFromBottom (inspectorHeight));

        if (tiles.isEmpty())
        {
            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.setFont (juce::FontOptions (16.0f));
            g.drawFittedText (beacons->listening.load()
                                  ? "Waiting for nodes.\nStart CircleScopeNode on a machine on this network; it appears here by itself."
                                  : "Cannot listen on UDP port " + juce::String (net::beaconPort) + ". Is another program using it?",
                              gridArea(), juce::Justification::centred, 3);
        }
    }

    void resized() override
    {
        layoutTiles();

        auto inspector = getLocalBounds().removeFromBottom (inspectorHeight).reduced (16, 10);
        auto top = inspector.removeFromTop (26);
        selectionLabel.setBounds (top.removeFromLeft (360));
        rendererLabel.setBounds (top);

        inspector.removeFromTop (8);
        auto rowArea = inspector.removeFromTop (26);
        streamBox.setBounds (rowArea.removeFromLeft (120));
        rowArea.removeFromLeft (8);

        auto place = [&rowArea] (Number& n, int labelWidth)
        {
            n.label.setBounds (rowArea.removeFromLeft (labelWidth));
            n.slider.setBounds (rowArea.removeFromLeft (98).reduced (4, 0));
        };
        place (cols, 74);
        place (rows, 42);
        place (col, 66);
        place (row, 36);
        place (spanW, 78);
        place (spanH, 42);

        testButton.setBounds (rowArea.removeFromRight (96));
        rowArea.removeFromRight (8);
        identifyButton.setBounds (rowArea.removeFromRight (90));
    }

private:
    static constexpr int headerHeight = 48, inspectorHeight = 90;

    struct Number
    {
        juce::Label label;
        juce::Slider slider;
    };

    juce::Rectangle<int> gridArea() const
    {
        return getLocalBounds().withTrimmedTop (headerHeight).withTrimmedBottom (inspectorHeight).reduced (12, 4);
    }

    static juce::var command (const char* name)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("cmd", name);
        return juce::var (o.get());
    }

    NodeLink* selectedLink() const { return selectedTile != nullptr ? &selectedTile->link : nullptr; }

    void sendSetting (const char* key, int value)
    {
        if (refreshing || selectedTile == nullptr)
            return;

        auto c = command ("set");
        c.getDynamicObject()->setProperty ("output", selectedTile->output);
        c.getDynamicObject()->setProperty (key, value);
        selectedTile->link.send (c);
    }

    void nodeSeen (const juce::String& address, int port, const juce::String& name)
    {
        for (auto* link : links)
        {
            if (link->address == address && link->port == port)
            {
                link->lastBeaconMs = juce::Time::getMillisecondCounter();
                return;
            }
        }

        auto* link = links.add (new NodeLink (address, port, [this] { dirty = true; }));
        link->name = name;
        link->lastBeaconMs = juce::Time::getMillisecondCounter();
        dirty = true;
    }

    // State changes arrive many times a second; fold them into one refresh.
    void timerCallback() override
    {
        if (! std::exchange (dirty, false))
            return;

        int wanted = 0;
        for (auto* link : links)
            wanted += juce::jmax (1, (int) link->outputs.size());

        if (wanted != tiles.size())
            rebuildTiles();
        else
            layoutTiles();   // wall settings may have changed

        refreshInspector();
        repaint();
    }

    void rebuildTiles()
    {
        // Remember the selection across the rebuild.
        NodeLink* keptLink = selectedLink();
        const int keptOutput = selectedTile != nullptr ? selectedTile->output : 0;
        selectedTile = nullptr;
        tiles.clear();

        for (auto* link : links)
        {
            for (int i = 0; i < juce::jmax (1, (int) link->outputs.size()); ++i)
            {
                auto* tile = tiles.add (new Tile (*link, i));
                tile->onClick = [this, tile] { select (tile); };
                addAndMakeVisible (tile);
                if (link == keptLink && i == keptOutput)
                    selectedTile = tile;
            }
        }

        if (selectedTile == nullptr && ! tiles.isEmpty())
            selectedTile = tiles.getFirst();
        for (auto* tile : tiles)
            tile->selected = tile == selectedTile;

        layoutTiles();
    }

    void select (Tile* tile)
    {
        selectedTile = tile;
        for (auto* t : tiles)
            t->selected = t == tile;
        refreshInspector();
        repaint();
    }

    // A group of tiles laid out together: either the outputs that make up
    // one wall (same stream and wall size), placed at their cells so the
    // grid looks like the wall, or a single output on its own.
    struct Block
    {
        int cols = 1, rows = 1;
        juce::Array<Tile*> members;
    };

    static const OutputState* stateOf (const Tile& tile)
    {
        return tile.output < (int) tile.link.outputs.size() ? &tile.link.outputs[(size_t) tile.output] : nullptr;
    }

    std::vector<Block> makeBlocks() const
    {
        std::vector<Block> blocks;
        std::map<std::tuple<int, int, int>, size_t> walls;   // stream, cols, rows -> block

        for (auto* tile : tiles)
        {
            const auto* s = stateOf (*tile);
            if (s == nullptr || s->cols * s->rows <= 1)
            {
                blocks.push_back ({ 1, 1, { tile } });
                continue;
            }

            const auto key = std::make_tuple (s->stream, s->cols, s->rows);
            if (walls.find (key) == walls.end())
            {
                walls[key] = blocks.size();
                blocks.push_back ({ s->cols, s->rows, {} });
            }
            blocks[walls[key]].members.add (tile);
        }
        return blocks;
    }

    // Flows the blocks left to right, top to bottom, for a given tile width.
    // Returns the height used; positions the tiles when apply is set.
    int flowBlocks (const std::vector<Block>& blocks, juce::Rectangle<int> area, int tileW, bool apply) const
    {
        constexpr int gap = 14;
        const int tileH = tileW * 9 / 16 + 48;
        int x = 0, y = 0, rowHeight = 0;

        for (const auto& block : blocks)
        {
            const int w = block.cols * tileW, h = block.rows * tileH;
            if (x > 0 && x + w > area.getWidth())
            {
                x = 0;
                y += rowHeight + gap;
                rowHeight = 0;
            }

            if (apply)
            {
                for (auto* tile : block.members)
                {
                    const auto* s = stateOf (*tile);
                    const bool inWall = s != nullptr && block.cols * block.rows > 1;
                    tile->setBounds (area.getX() + x + (inWall ? s->col * tileW : 0),
                                     area.getY() + y + (inWall ? s->row * tileH : 0),
                                     (inWall ? s->spanW : 1) * tileW,
                                     (inWall ? s->spanH : 1) * tileH);
                }
            }

            x += w + gap;
            rowHeight = juce::jmax (rowHeight, h);
        }
        return y + rowHeight;
    }

    // Uses the largest tile size at which everything fits.
    void layoutTiles()
    {
        const auto area = gridArea();
        if (tiles.isEmpty() || area.isEmpty())
            return;

        const auto blocks = makeBlocks();
        int widest = 1;
        for (const auto& block : blocks)
            widest = juce::jmax (widest, block.cols);

        int tileW = juce::jmax (60, area.getWidth() / widest);
        while (tileW > 60 && flowBlocks (blocks, area, tileW, false) > area.getHeight())
            tileW -= 4;

        flowBlocks (blocks, area, tileW, true);
    }

    void refreshInspector()
    {
        const juce::ScopedValueSetter<bool> guard (refreshing, true);
        const auto* link = selectedLink();
        const bool haveState = link != nullptr && selectedTile->output < (int) link->outputs.size();

        for (juce::Component* c : std::initializer_list<juce::Component*> { &streamBox, &identifyButton, &testButton, &cols.slider, &rows.slider,
                                                                            &col.slider, &row.slider, &spanW.slider, &spanH.slider })
            c->setEnabled (haveState && link->connected);

        if (link == nullptr)
        {
            selectionLabel.setText ("No output selected", juce::dontSendNotification);
            rendererLabel.setText ({}, juce::dontSendNotification);
            return;
        }

        selectionLabel.setText ((link->name.isNotEmpty() ? link->name : link->address) + "  /  output "
                                    + juce::String (selectedTile->output + 1) + "   (" + link->address + ")",
                                juce::dontSendNotification);
        rendererLabel.setText (link->renderer, juce::dontSendNotification);
        testButton.setToggleState (link->testTone, juce::dontSendNotification);

        if (! haveState)
            return;

        // Do not overwrite a value the user is in the middle of typing.
        const auto& s = link->outputs[(size_t) selectedTile->output];
        auto show = [] (Number& n, int value)
        {
            if (! n.slider.hasKeyboardFocus (true))
                n.slider.setValue (value, juce::dontSendNotification);
        };
        streamBox.setSelectedId (s.stream, juce::dontSendNotification);
        show (cols, s.cols);
        show (rows, s.rows);
        show (col, s.col + 1);
        show (row, s.row + 1);
        show (spanW, s.spanW);
        show (spanH, s.spanH);
    }

    juce::OwnedArray<NodeLink> links;
    juce::OwnedArray<Tile> tiles;
    Tile* selectedTile = nullptr;
    std::unique_ptr<BeaconListener> beacons;
    bool dirty = false, refreshing = false;

    juce::Label selectionLabel, rendererLabel;
    juce::ComboBox streamBox;
    Number cols, rows, col, row, spanW, spanH;
    juce::TextButton identifyButton { "Identify" };
    juce::ToggleButton testButton { "Test tone" };
};

//==============================================================================
class ControlApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "CircleScope Control"; }
    const juce::String getApplicationVersion() override { return "0.2.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
       #if ! JUCE_WINDOWS
        std::signal (SIGPIPE, SIG_IGN);
       #endif
        juce::LookAndFeel::getDefaultLookAndFeel().setColour (juce::ResizableWindow::backgroundColourId, background);
        window = std::make_unique<MainWindow>();
    }

    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    struct MainWindow : public juce::DocumentWindow
    {
        MainWindow() : DocumentWindow ("CircleScope Control", background, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (1280, 480, 8000, 8000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { juce::JUCEApplicationBase::quit(); }
    };

    std::unique_ptr<MainWindow> window;
};
}

START_JUCE_APPLICATION (ControlApplication)
