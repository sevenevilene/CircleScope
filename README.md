# CircleScope

Stereo "circle" oscilloscope plugin (AU / VST3 / Standalone, JUCE + OpenGL),
with a networked mode that spreads it over a cluster of screens.
Each channel is drawn as a 2D trajectory in its own square:

- **Quadrature**: x = signal, y = the same signal shifted 90° (via an FFT
  analytic signal). A pure sine is a perfect circle.
- **Delay embed**: x = s(t), y = s(t − delay). The default delay of 12.5 ms
  is a quarter period of 20 Hz, so a 20 Hz sine is a circle.

Colour shows instantaneous frequency (20 Hz red to 20 kHz blue), and brightness
shows the trail's age. **Test tone** shows 20 Hz + 60 Hz on L and
20 Hz + 60.5 Hz on R, so the two channels slowly drift apart.

## Build

```bash
git clone --depth 1 --branch 8.0.9 https://github.com/juce-framework/JUCE.git external/JUCE
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

This builds the plugin, `CircleScopeNode` and `CircleScope Control`. The build copies the AU to `~/Library/Audio/Plug-Ins/Components` and the VST3
to `~/Library/Audio/Plug-Ins/VST3`. To validate the AU: `auval -v aufx Crc1 Cscp`.

## Controlling it

Every look parameter is a host parameter, so a DAW can automate it. Each one
also answers to a MIDI controller (CC 102 to 114, in the order of
`Source/ScopeParams.h`; 0 to 127 covers the full range) in the standalone app
and in hosts that send MIDI to audio effects. Logic does not send MIDI to an
audio effect, so use track automation there.

**Low cut / High cut** band-limit what is drawn, so several instances can each
show one band of the same audio.

Full screen is clean: no controls, labels or cursor. Keys: `Esc`/`F` exit,
`M` mode, `L` layout, `C` colour, `G` guides, `T` test tone.

## Cluster

Three programs work together on one network:

- **CircleScope** (the plugin): set **Network** to a stream number (1 to 16)
  and it sends what it displays, plus its parameters, to every node showing
  that stream. One instance per track gives one stream per track.
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
- `Source/ScopeComponent.*`: analytic signal, band limits, auto-gain, OpenGL rendering, wall regions, previews
- `Source/PluginProcessor.*`, `PluginEditor.*`, `ControlPanel.h`, `FullScreenWindow.h`: the plugin
- `Source/NetProtocol.h`, `NetSender.h`: wire format and the plugin's sender
- `Source/Node/NodeMain.cpp`: the cluster node
- `Source/Control/ControlMain.cpp`: the control center

## Ideas next

Frame-locked tiles (all nodes drawing the same sample window, so fast shapes
meet exactly at screen seams); 3D delay embedding with rotation; a cymatics
(Chladni plate) mode.

## License

The CircleScope source code is MIT licensed (see `LICENSE`). JUCE is not
included in this repository and has its own license (AGPLv3 or a commercial
JUCE license), which applies to the plugin binaries you build with it.
