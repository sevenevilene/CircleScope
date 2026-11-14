# CircleScope

Stereo "circle" oscilloscope plugin (AU / VST3 / Standalone, JUCE + OpenGL),
with two ways to show many instances at once: CircleScope Wall, a grid of
scopes on one computer, and a networked cluster of screens.
Each channel is drawn as a 2D trajectory in its own square:

- **Quadrature**: x = signal, y = the same signal shifted 90° (via an FFT
  analytic signal). A pure sine is a perfect circle.
- **Delay embed**: x = s(t), y = s(t − delay). The default delay of 12.5 ms
  is a quarter period of 20 Hz, so a 20 Hz sine is a circle.
- **Swing**: the signal moves a point along an axis through the centre
  (-1 to 1), and the axis swings round in the plane, so the picture is a
  polar plot of the sound. **Swing motion** is Back and forth (a pendulum,
  180° each way) or Round (turning on and on). The axis makes one swing per
  Window length (**Once per window**), or swings at the **Rate** you set. A
  rate tied to the pitch gives a still figure: a 110 Hz tone with a round
  swing of 55 Hz is a four-petal rose; slightly off, it slowly turns. Swing
  applies to the Side by side and Overlay layouts.

Colour shows instantaneous frequency (20 Hz red to 20 kHz blue), and brightness
shows the trail's age. **Test tone** shows 20 Hz + 60 Hz on L and
20 Hz + 60.5 Hz on R, so the two channels slowly drift apart.

## Build

```bash
git clone --depth 1 --branch 8.0.9 https://github.com/juce-framework/JUCE.git external/JUCE
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

This builds the plugin, `CircleScope Wall`, `CircleScopeNode` and `CircleScope Control`. The build copies the AU to `~/Library/Audio/Plug-Ins/Components` and the VST3
to `~/Library/Audio/Plug-Ins/VST3`. To validate the AU: `auval -v aufx Crc1 Cscp`.

## Controlling it

Every look parameter is a host parameter, so a DAW can automate it. Each one
also answers to a MIDI controller (CC 102 to 119 and then 85, in the order of
`Source/ScopeParams.h`; 0 to 127 covers the full range) in the standalone app
and in hosts that send MIDI to audio effects. Logic does not send MIDI to an
audio effect, so use track automation there.

**Low cut / High cut** band-limit what is drawn, so several instances can each
show one band of the same audio.

Full screen is clean: no controls, labels or cursor. Keys: `Esc`/`F` exit,
`M` mode, `L` layout, `C` colour, `G` guides, `T` test tone.

## CircleScope Wall

For composing on one computer, with no cluster: one app that divides its
window into a grid, one scope per cell.

- In **CircleScope** (the plugin), switch on one or more of the **Streams**
  1 to 16. On the wall a stream is a cell, so this picks the cells the
  instance appears in. With **Divide** off its picture is repeated in each
  of them; with Divide on, one picture is split across them (across the
  smallest block of cells that holds them all).
- **CircleScope Wall** shows one stream in each cell. Press `Tab` for the
  setup bar: grid size (up to 6 x 6) and, for the selected cell, its stream
  or Off. A new grid starts with streams 1, 2, 3... in reading order. The
  settings are remembered.

### Layers

Instances that share a stream are layers of that cell, up to 8, each drawn
with its own settings. So one instance can fill the whole grid while another
sits on a single cell on top of it. Per instance:

- **Layer** (1 to 8) is the stacking order, 1 at the bottom. The bottom
  layer's guides are the ones shown.
- **Blend** is how the instance is combined with the layers under it: Add
  (the default), Screen, Normal, Lighten, Multiply, Darken, Overlay,
  Difference, Exclusion or Subtract. A layer is light on black, so in Normal,
  Multiply, Darken and Overlay its brightness serves as its opacity: where it
  is dark, what is underneath shows through unchanged.

The streams, Layer, Blend and Divide are host parameters like the rest, so
the DAW can automate where an instance appears and how it blends.

Run it in a window next to the DAW, or drag it to a second monitor or TV and
press `F` for full screen there. Full screen is clean: no bar, captions or
cursor. It can also run on another computer on the same network.

Keys: `F` full screen (`Esc` leaves it, so does a double-click), `Tab`/`S`
setup bar, arrows select a cell, `+`/`-` change its stream, `T` test tone,
`Q` quit. `--help` lists the command line options (`--grid 3x2`,
`--fullscreen`, `--display N`, ...).

Each layer analyses its audio on its own thread, so a big grid uses all the
CPU cores; the drawing itself shares one OpenGL thread.

To the plugin a wall is one more node: it is found the same way and gets the
same streams, so a wall and a cluster can run side by side. The control app
does not list walls; a wall is set up in its own window.

## Cluster

For an installation with many screens.

Three programs work together on one network:

- **CircleScope** (the plugin): switch on one or more **Streams** (1 to 16)
  and it sends what it displays, plus its parameters, to every node showing
  one of them. Instances sharing a stream are layers there too (see Layers
  above); Divide only applies to a wall, since a node has its own place in
  the video wall set from the control app.
- **CircleScopeNode**: runs on each display machine (Raspberry Pi 4, or any
  Mac/Linux box). Clean full screen on every display, drawing its stream.
  `--span` uses one window across all displays, `--windowed` is for testing.
  Keys: `Esc`/`Q` quit, `T` test tone, `I` identify.
- **CircleScope Control**: shows a live preview of every output of every node
  in a grid. Select a tile to set its stream and its place in a video wall
  (wall size in screens, this screen's column and row, how many it spans).
  A wall of 4 x 3 screens on one stream draws one picture across all twelve.

Nodes announce themselves by UDP broadcast (port 47800), so nothing needs an
address typed in. Audio and parameters go by UDP from 47801, control and
previews by TCP from 47901. There is no authentication: keep it on a network
you trust. `Source/NetProtocol.h` describes the wire format.

On a Mac with the firewall on, allow incoming connections for CircleScope
Control and the plugin's host when macOS asks, or the beacons never arrive.

`cluster/setup-node.sh` prepares a Raspberry Pi (build, auto-start at boot).
A Pi with no monitor attached needs a forced video mode to have something to
render to: add `video=HDMI-A-1:1920x1080@60D` to `/boot/firmware/cmdline.txt`.

## Layout

- `Source/ScopeParams.h`: the one table of parameters (ranges, defaults, CCs)
- `Source/ScopeSource.h`, `RingBuffer.h`: sample rings and parameter values a scope draws from
- `Source/ScopeComponent.*`: analytic signal, band limits, auto-gain (on a thread per scope), OpenGL rendering, wall regions, previews
- `Source/PluginProcessor.*`, `PluginEditor.*`, `ControlPanel.h`, `FullScreenWindow.h`: the plugin
- `Source/NetProtocol.h`, `NetSender.h`, `NetReceiver.h`: wire format, the plugin's sender, the receiver of walls and nodes
- `Source/Wall/WallMain.cpp`: the wall
- `Source/Node/NodeMain.cpp`: the cluster node
- `Source/Control/ControlMain.cpp`: the control center

## To do

- **Frame-locked tiles**, once it can be tested on a real cluster. Every
  node analyses the samples it has when it draws, and runs its own
  auto-gain, so on a picture split over several screens fast shapes may not
  meet exactly at the seams. The nodes should all draw the same sample
  window (the packets already carry a sample position to lock to) with the
  same gain. The same applies to a divided picture across the cells of a
  wall.

## Ideas next

- Swing mode: lock the swing rate to the detected pitch, with a ratio, so
  figures stand still while the notes change.
- Swing mode: let one channel move the point and the other swing the axis.
- Swing mode in 3D: more planes (XY, XZ, YZ), each with its own swing, so
  the axis moves over a sphere; drawn through a rotating 3D view.
- 3D delay embedding with rotation.
- A cymatics (Chladni plate) mode.

## License

The CircleScope source code is MIT licensed (see `LICENSE`). JUCE is not
included in this repository and has its own license (AGPLv3 or a commercial
JUCE license), which applies to the plugin binaries you build with it.
