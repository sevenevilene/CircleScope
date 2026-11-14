// CircleScope Wall: one window divided into a grid of scopes.
//
// Every cell of the grid shows one network stream. A CircleScope plugin
// instance picks the streams, and so the cells, it appears in; instances that
// share a cell are layers of it, combined by their blend modes. An instance
// set to Divide is drawn as one picture across all its cells instead of once
// in each. Every layer is analysed on a thread of its own. It is the way to work on one computer, with no
// cluster: run it in a window next to the DAW, or full screen on a second
// monitor or TV. Plugins find it by the same broadcast beacon as the cluster
// nodes (see Node/NodeMain.cpp) and send to walls and nodes alike.
//
// Keys:  F        full screen on the display the window is on (Esc leaves it)
//        Tab / S  show or hide the setup bar (grid size, stream of each cell)
//        arrows   select a cell          + / -  change its stream
//        T        test tone              Q      quit

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>
#include <array>
#include "../NetReceiver.h"
#include "../ScopeComponent.h"

namespace
{
//==============================================================================
// Everything that is remembered between runs.
struct WallConfig
{
    static constexpr int maxSide = 6;

    int cols = 2, rows = 2;
    std::vector<int> streams { 1, 2, 3, 4 };   // one per cell, row by row; 0 = off
    juce::Rectangle<int> windowBounds { 80, 80, 1280, 720 };
    bool fullScreen = false;

    int numCells() const { return cols * rows; }

    // A new grid starts with stream 1, 2, 3... in reading order.
    void setGrid (int newCols, int newRows)
    {
        cols = juce::jlimit (1, maxSide, newCols);
        rows = juce::jlimit (1, maxSide, newRows);
        streams.clear();
        for (int i = 0; i < numCells(); ++i)
            streams.push_back (i % net::maxStreams + 1);
    }

    void load (const juce::File& file)
    {
        const auto saved = juce::JSON::parse (file);
        if (! saved.isObject())
            return;

        setGrid ((int) saved.getProperty ("cols", cols), (int) saved.getProperty ("rows", rows));
        if (const auto* list = saved["streams"].getArray(); list != nullptr && list->size() == numCells())
            for (int i = 0; i < numCells(); ++i)
                streams[(size_t) i] = juce::jlimit (0, net::maxStreams, (int) list->getReference (i));

        if (const auto* b = saved["window"].getArray(); b != nullptr && b->size() == 4)
            windowBounds = { (int) (*b)[0], (int) (*b)[1], juce::jmax (320, (int) (*b)[2]), juce::jmax (200, (int) (*b)[3]) };
        fullScreen = (bool) saved.getProperty ("fullscreen", false);
    }

    void save (const juce::File& file) const
    {
        juce::Array<juce::var> list;
        for (int stream : streams)
            list.add (stream);

        juce::DynamicObject::Ptr root = new juce::DynamicObject();
        root->setProperty ("cols", cols);
        root->setProperty ("rows", rows);
        root->setProperty ("streams", list);
        root->setProperty ("window", juce::Array<juce::var> { windowBounds.getX(), windowBounds.getY(),
                                                              windowBounds.getWidth(), windowBounds.getHeight() });
        root->setProperty ("fullscreen", fullScreen);
        file.getParentDirectory().createDirectory();
        file.replaceWithText (juce::JSON::toString (juce::var (root.get())));
    }
};

//==============================================================================
// The window: the grid of scopes and, while setting up, a bar of controls
// under it and a caption over each cell. With the bar hidden the cells tile
// the window edge to edge and nothing but the scopes is drawn.
class WallWindow : public juce::Component,
                   private juce::Timer
{
public:
    struct Actions
    {
        std::function<void()> configChanged;      // save it and tell the plugins
        std::function<void()> toggleFullScreen;   // replaces this window: returns before it happens
        std::function<void()> quit;
    };

    WallWindow (WallConfig& configToUse, StreamReceiver& receiverToUse, bool isFullScreen,
                juce::Rectangle<int> bounds, Actions actionsToUse)
        : config (configToUse), receiver (receiverToUse), fullScreen (isFullScreen),
          setupVisible (! isFullScreen), actions (std::move (actionsToUse))
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);
        setName ("CircleScope Wall");

        for (int i = 1; i <= WallConfig::maxSide; ++i)
        {
            colsBox.addItem (juce::String (i), i);
            rowsBox.addItem (juce::String (i), i);
        }
        streamBox.addItem ("Off", 1);
        for (int i = 1; i <= net::maxStreams; ++i)
            streamBox.addItem ("Stream " + juce::String (i), i + 1);

        colsLabel.setText ("Columns", juce::dontSendNotification);
        rowsLabel.setText ("Rows", juce::dontSendNotification);
        fullScreenButton.setButtonText (fullScreen ? "Leave full screen" : "Full screen");
        hint.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.55f));
        hint.setJustificationType (juce::Justification::centredRight);
        hint.setMinimumHorizontalScale (0.7f);

        colsBox.onChange = rowsBox.onChange = [this] { setGrid (colsBox.getSelectedId(), rowsBox.getSelectedId()); };
        streamBox.onChange = [this] { setCellStream (selected, streamBox.getSelectedId() - 1); };
        testButton.onClick = [this] { receiver.testTone.store (testButton.getToggleState()); };
        fullScreenButton.onClick = [this] { actions.toggleFullScreen(); };

        for (juce::Component* c : std::initializer_list<juce::Component*> { &colsLabel, &colsBox, &rowsLabel, &rowsBox, &cellLabel,
                                                                            &streamBox, &testButton, &fullScreenButton, &hint })
        {
            c->setWantsKeyboardFocus (false);   // the keys stay with the window
            bar.addAndMakeVisible (c);
        }
        addChildComponent (bar);

        if (fullScreen)
            addToDesktop (0);
        else
        {
            using Peer = juce::ComponentPeer;
            // The maximise button is what makes the green button on macOS the
            // usual "full screen in a Space of its own" one instead of a zoom.
            addToDesktop (Peer::windowHasTitleBar | Peer::windowIsResizable | Peer::windowHasCloseButton
                          | Peer::windowHasMinimiseButton | Peer::windowHasMaximiseButton | Peer::windowAppearsOnTaskbar);
        }

        setBounds (bounds);
        rebuildCells();
        setVisible (true);
        toFront (true);
        grabKeyboardFocus();
        startTimer (250);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        if (! setupVisible)
            return;

        g.setColour (juce::Colour (0xff17171c));
        g.fillRect (bar.getBounds());

        g.setFont (juce::FontOptions (13.0f));
        for (int i = 0; i < (int) cells.size(); ++i)
        {
            const int stream = cells[(size_t) i].stream;
            const auto outer = cellBounds (i);
            auto caption = outer.reduced (cellGap).removeFromTop (captionHeight).reduced (4, 0);

            if (i == selected)
            {
                g.setColour (juce::Colour (0xff7f77dd));
                g.drawRect (outer.reduced (1), 2);
            }

            juce::String status = "off";
            auto statusColour = juce::Colours::white.withAlpha (0.35f);
            if (stream > 0)
            {
                const int layers = receiver.getNumLayers (stream);
                status = receiver.testTone.load() ? "test tone" : layers > 1 ? juce::String (layers) + " layers"
                                                                : layers == 1 ? "live" : "no signal";
                if (layers > 0)
                    statusColour = juce::Colour (0xff97c459);
            }

            g.setColour (juce::Colours::white.withAlpha (i == selected ? 0.95f : 0.6f));
            g.drawText (stream > 0 ? "Stream " + juce::String (stream) : "Empty", caption, juce::Justification::centredLeft);
            g.setColour (statusColour);
            g.drawText (status, caption, juce::Justification::centredRight);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        bar.setVisible (setupVisible);
        if (setupVisible)
        {
            bar.setBounds (area.removeFromBottom (barHeight));
            auto row = bar.getLocalBounds().reduced (10, 8);
            auto place = [&row] (juce::Component& c, int width) { c.setBounds (row.removeFromLeft (width)); row.removeFromLeft (8); };
            place (colsLabel, 62);
            place (colsBox, 58);
            place (rowsLabel, 40);
            place (rowsBox, 58);
            row.removeFromLeft (12);
            place (cellLabel, 50);
            place (streamBox, 110);
            row.removeFromLeft (12);
            place (testButton, 96);
            place (fullScreenButton, 130);
            hint.setBounds (row);
        }
        gridArea = area;

        for (int i = 0; i < (int) cells.size(); ++i)
            if (auto& scope = cells[(size_t) i].scope)
                scope->setBounds (setupVisible ? cellBounds (i).reduced (cellGap).withTrimmedTop (captionHeight)
                                               : cellBounds (i));
        boundsChanged();
    }

    void moved() override { boundsChanged(); }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            if (fullScreen)
                actions.toggleFullScreen();
            return true;
        }
        if (key == juce::KeyPress::tabKey)
        {
            setSetupVisible (! setupVisible);
            return true;
        }

        if (setupVisible)
        {
            const int cols = config.cols, count = (int) cells.size();
            if (key == juce::KeyPress::leftKey)  { select ((selected + count - 1) % count); return true; }
            if (key == juce::KeyPress::rightKey) { select ((selected + 1) % count); return true; }
            if (key == juce::KeyPress::upKey)    { select ((selected + count - cols) % count); return true; }
            if (key == juce::KeyPress::downKey)  { select ((selected + cols) % count); return true; }
        }

        const int stream = cells[(size_t) selected].stream;
        switch (juce::CharacterFunctions::toLowerCase (key.getTextCharacter()))
        {
            case 'f': actions.toggleFullScreen(); return true;
            case 'q': actions.quit(); return true;
            case 's': setSetupVisible (! setupVisible); return true;
            case 't': receiver.testTone.store (! receiver.testTone.load()); return true;
            case '+':
            case '=': if (setupVisible) setCellStream (selected, juce::jmin (net::maxStreams, stream + 1)); return true;
            case '-': if (setupVisible) setCellStream (selected, juce::jmax (0, stream - 1)); return true;
            default:  return false;
        }
    }

    // Also hears the scopes' mouse events (see rebuildCell).
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! setupVisible)
            return;

        const auto position = e.getEventRelativeTo (this).getPosition();
        for (int i = 0; i < (int) cells.size(); ++i)
            if (cellBounds (i).contains (position))
                select (i);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! bar.isParentOf (e.eventComponent) && e.eventComponent != &bar)
            actions.toggleFullScreen();
    }

    void userTriedToCloseWindow() override { actions.quit(); }

    bool isFullScreen() const { return fullScreen; }

private:
    struct Cell
    {
        int stream = 0;
        std::unique_ptr<ScopeComponent> scope;   // none while the cell is off
    };

    // The grid as the scopes see it. They read it on the GL thread, so it is
    // never changed: a change of configuration gives every scope a new one.
    struct Grid
    {
        int cols = 1, rows = 1;
        std::vector<int> streams;

        // The part of an instance's picture that belongs in a cell. A divided
        // instance spreads one picture over the smallest block of cells that
        // holds all the cells showing its streams.
        ScopeComponent::View viewFor (int cell, juce::uint32 streamMask, bool divide) const
        {
            if (! divide)
                return {};

            int minCol = cols, maxCol = -1, minRow = rows, maxRow = -1;
            for (int i = 0; i < (int) streams.size(); ++i)
            {
                const int stream = streams[(size_t) i];
                if (stream > 0 && (streamMask & (1u << (stream - 1))) != 0)
                {
                    minCol = juce::jmin (minCol, i % cols);
                    maxCol = juce::jmax (maxCol, i % cols);
                    minRow = juce::jmin (minRow, i / cols);
                    maxRow = juce::jmax (maxRow, i / cols);
                }
            }

            const float w = (float) (maxCol - minCol + 1), h = (float) (maxRow - minRow + 1);
            return { (float) (cell % cols - minCol) / w, (float) (cell / cols - minRow) / h, 1.0f / w, 1.0f / h };
        }
    };

    // Called on the GL thread: the layers of one cell.
    static int layersFor (const StreamReceiver& receiver, const Grid& grid, int cell, ScopeComponent::LayerList& list)
    {
        StreamReceiver::LayerList heard;
        const int count = receiver.getLayers (grid.streams[(size_t) cell], heard);
        for (int i = 0; i < count; ++i)
        {
            const auto& layer = heard[(size_t) i];
            list[(size_t) i] = { layer.source, grid.viewFor (cell, layer.streamMask, layer.source->get (scope::pDivide) > 0.5f) };
        }
        return count;
    }

    // Hands every scope the current configuration.
    void updateScopes()
    {
        auto newGrid = std::make_shared<Grid>();
        newGrid->cols = config.cols;
        newGrid->rows = config.rows;
        newGrid->streams = config.streams;
        grid = newGrid;

        for (int i = 0; i < (int) cells.size(); ++i)
            if (auto& scope = cells[(size_t) i].scope)
                scope->setLayerProvider ([&r = receiver, g = grid, i] (ScopeComponent::LayerList& list) { return layersFor (r, *g, i, list); });
    }

    juce::Rectangle<int> cellBounds (int index) const
    {
        const int col = index % config.cols, row = index / config.cols;
        const int x0 = gridArea.getX() + gridArea.getWidth() * col / config.cols;
        const int x1 = gridArea.getX() + gridArea.getWidth() * (col + 1) / config.cols;
        const int y0 = gridArea.getY() + gridArea.getHeight() * row / config.rows;
        const int y1 = gridArea.getY() + gridArea.getHeight() * (row + 1) / config.rows;
        return { x0, y0, x1 - x0, y1 - y0 };
    }

    void rebuildCells()
    {
        cells.clear();
        cells.resize ((size_t) config.numCells());
        for (int i = 0; i < config.numCells(); ++i)
            rebuildCell (i);
        updateScopes();

        colsBox.setSelectedId (config.cols, juce::dontSendNotification);
        rowsBox.setSelectedId (config.rows, juce::dontSendNotification);
        select (juce::jmin (selected, config.numCells() - 1));
        resized();
    }

    // Brings one cell in line with the configuration. updateScopes must follow.
    void rebuildCell (int index)
    {
        auto& cell = cells[(size_t) index];
        cell.stream = config.streams[(size_t) index];

        if (cell.stream == 0)
        {
            cell.scope.reset();
        }
        else if (cell.scope == nullptr)
        {
            cell.scope = std::make_unique<ScopeComponent> (ScopeComponent::LayerProvider(), false);   // see updateScopes
            cell.scope->addMouseListener (this, false);
            addAndMakeVisible (*cell.scope);
            updateCursor();
        }
    }

    void setGrid (int cols, int rows)
    {
        if (cols == config.cols && rows == config.rows)
            return;

        config.setGrid (cols, rows);
        rebuildCells();
        actions.configChanged();
    }

    void setCellStream (int index, int stream)
    {
        if (config.streams[(size_t) index] == stream)
            return;

        config.streams[(size_t) index] = stream;
        rebuildCell (index);
        updateScopes();
        select (index);
        resized();
        actions.configChanged();
    }

    void select (int index)
    {
        selected = index;
        cellLabel.setText ("Cell " + juce::String (index + 1), juce::dontSendNotification);
        streamBox.setSelectedId (cells[(size_t) index].stream + 1, juce::dontSendNotification);
        repaint();
    }

    void setSetupVisible (bool shouldBeVisible)
    {
        setupVisible = shouldBeVisible;
        updateCursor();
        resized();
        repaint();
    }

    // A clean full-screen picture has no mouse pointer over it.
    void updateCursor()
    {
        const auto cursor = fullScreen && ! setupVisible ? juce::MouseCursor::NoCursor : juce::MouseCursor::NormalCursor;
        setMouseCursor (cursor);
        for (auto& cell : cells)
            if (cell.scope != nullptr)
                cell.scope->setMouseCursor (cursor);
    }

    void boundsChanged()
    {
        if (fullScreen || ! isOnDesktop() || config.windowBounds == getBounds())
            return;

        // Not while the system has the window full screen (the green button),
        // or on the way there: the size to remember is the window's own.
        const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getBounds());
        if (getPeer()->isFullScreen() || (display != nullptr && getHeight() >= display->totalArea.getHeight()))
            return;

        config.windowBounds = getBounds();
        actions.configChanged();
    }

    void timerCallback() override
    {
        if (! setupVisible)
            return;

        float slowest = 0.0f;
        for (auto& cell : cells)
            if (cell.scope != nullptr)
                slowest = slowest > 0.0f ? juce::jmin (slowest, cell.scope->getFramesPerSecond()) : cell.scope->getFramesPerSecond();

        testButton.setToggleState (receiver.testTone.load(), juce::dontSendNotification);
        hint.setText (juce::String (juce::roundToInt (slowest)) + " fps    Tab hides this bar, F full screen", juce::dontSendNotification);
        repaint (gridArea);   // the captions' signal status
    }

    static constexpr int barHeight = 44, captionHeight = 20, cellGap = 4;

    WallConfig& config;
    StreamReceiver& receiver;
    const bool fullScreen;
    bool setupVisible;
    Actions actions;

    std::vector<Cell> cells;
    std::shared_ptr<const Grid> grid;   // what the cells' scopes are drawing from
    int selected = 0;
    juce::Rectangle<int> gridArea;

    juce::Component bar;
    juce::Label colsLabel, rowsLabel, cellLabel, hint;
    juce::ComboBox colsBox, rowsBox, streamBox;
    juce::ToggleButton testButton { "Test tone" };
    juce::TextButton fullScreenButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WallWindow)
};

//==============================================================================
class WallApplication : public juce::JUCEApplication,
                        private juce::Timer,
                        private juce::AsyncUpdater
{
public:
    const juce::String getApplicationName() override { return "CircleScope Wall"; }
    const juce::String getApplicationVersion() override { return "0.3.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        const auto args = getCommandLineParameterArray();
        auto option = [&args] (const char* name) { const int i = args.indexOf (name); return i >= 0 ? args[i + 1] : juce::String(); };

        if (args.contains ("--help") || args.contains ("-h"))
        {
            std::cout << "CircleScope Wall [options]\n"
                         "  --grid CxR       grid size, for example 3x2 (default: as last time, at first 2x2)\n"
                         "  --fullscreen     start full screen (default: as last time)\n"
                         "  --windowed       start in a window\n"
                         "  --display N      which display to fill in full screen (1 = main; default: the one the window is on)\n"
                         "  --name NAME      name announced on the network (default: host name)\n"
                         "  --config FILE    where the settings are kept\n"
                         "  --test           start with the local test tone on\n"
                         "Keys: F full screen, Esc leave it, Tab/S setup bar, arrows select a cell, +/- its stream, T test tone, Q quit\n";
            quit();
            return;
        }

        wallName = option ("--name").isNotEmpty() ? option ("--name") : juce::SystemStats::getComputerName();
        configFile = option ("--config").isNotEmpty()
                         ? juce::File::getCurrentWorkingDirectory().getChildFile (option ("--config"))
                         : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                               .getChildFile ("CircleScope").getChildFile ("wall.json");
        config.load (configFile);

        if (const auto grid = juce::StringArray::fromTokens (option ("--grid").toLowerCase(), "x", ""); grid.size() == 2)
            if (grid[0].getIntValue() != config.cols || grid[1].getIntValue() != config.rows)
                config.setGrid (grid[0].getIntValue(), grid[1].getIntValue());

        if (args.contains ("--fullscreen")) config.fullScreen = true;
        if (args.contains ("--windowed"))   config.fullScreen = false;
        displayNumber = option ("--display").getIntValue();
        receiver.testTone.store (args.contains ("--test"));

        if (! receiver.start())
            juce::Logger::writeToLog ("CircleScope Wall: no free UDP port for audio from " + juce::String (net::audioPortBase));

        openWindow();
        juce::Logger::writeToLog ("CircleScope Wall \"" + wallName + "\": " + juce::String (config.cols) + " x " + juce::String (config.rows)
                                  + " grid, audio UDP " + juce::String (receiver.getAudioPort())
                                  + ", config " + configFile.getFullPathName());

        sendBeacon();
        startTimer (250);
    }

    void shutdown() override
    {
        stopTimer();
        cancelPendingUpdate();
        closeWindow();
        if (saveDue)
            config.save (configFile);
    }

    void systemRequestedQuit() override { quit(); }

private:
    void openWindow()
    {
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        juce::Rectangle<int> bounds;

        if (config.fullScreen)
        {
            const auto* display = displayNumber >= 1 && displayNumber <= displays.displays.size()
                                      ? &displays.displays.getReference (displayNumber - 1)
                                      : displays.getDisplayForRect (config.windowBounds);
            bounds = display->totalArea;
        }
        else
        {
            // A window saved on a display that has since gone comes back to the main one.
            if (! displays.getRectangleList (true).intersectsRectangle (config.windowBounds.reduced (40)))
                config.windowBounds = WallConfig().windowBounds;
            bounds = config.windowBounds;
        }

        WallWindow::Actions actions;
        actions.configChanged = [this] { saveDue = beaconDue = true; };
        actions.toggleFullScreen = [this] { triggerAsyncUpdate(); };
        actions.quit = [] { juce::JUCEApplicationBase::quit(); };
        window = std::make_unique<WallWindow> (config, receiver, config.fullScreen, bounds, std::move (actions));

       #if JUCE_MAC
        if (config.fullScreen)   // hides the menu bar and the Dock
            juce::Desktop::getInstance().setKioskModeComponent (window.get(), false);
       #endif
    }

    void closeWindow()
    {
        juce::Desktop::getInstance().setKioskModeComponent (nullptr, false);
        window.reset();
    }

    // Switching between window and full screen builds a fresh window, once
    // the old one's key or mouse callback has returned.
    void handleAsyncUpdate() override
    {
        closeWindow();
        config.fullScreen = ! config.fullScreen;
        saveDue = true;
        openWindow();
    }

    // Tells the plugins on the network where this wall is and which streams it shows.
    void sendBeacon()
    {
        juce::Array<juce::var> streams;
        for (int stream : config.streams)
            if (stream > 0)
                streams.addIfNotAlreadyThere (stream);

        juce::DynamicObject::Ptr beacon = new juce::DynamicObject();
        beacon->setProperty ("app", net::nodeAppId);   // to a plugin, a wall is one more node
        beacon->setProperty ("name", wallName);
        beacon->setProperty ("audio", receiver.getAudioPort());
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

        // Plugins on this machine each have their own loopback port.
        for (int i = 0; i < net::localBeaconPorts; ++i)
            beaconSocket.write ("127.0.0.1", net::localBeaconPortBase + i, text.toRawUTF8(), (int) text.getNumBytesAsUTF8());
    }

    void timerCallback() override
    {
        if (std::exchange (beaconDue, false) || ++tick % 4 == 0)
            sendBeacon();

        // Not on every change: dragging the window makes a stream of them.
        if (saveDue && ++ticksSinceSaveDue >= 4)
        {
            config.save (configFile);
            saveDue = false;
            ticksSinceSaveDue = 0;
        }
    }

    juce::String wallName;
    juce::File configFile;
    WallConfig config;
    int displayNumber = 0;
    StreamReceiver receiver;
    std::unique_ptr<WallWindow> window;
    juce::DatagramSocket beaconSocket { true };
    bool saveDue = false, beaconDue = false;
    int tick = 0, ticksSinceSaveDue = 0;
};
}

START_JUCE_APPLICATION (WallApplication)
