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

- **Axis**: the signal moves a point along one axis, from -1 to 1, and
  nothing else: up and down (y) on the XY plane, in depth (z) on YZ, left and
  right (x) on XZ. One instance alone draws a line. Three of them summed
  (Sum with, see Planes), one on each plane, put three tracks on the three axes of
  space: a Lissajous figure in 3D.

- **Field**: not a curve but a field of standing waves, worked out on the
  graphics card. The picture is filled with dots that stay where they are
  and swing between a colour and its complement, through dark. Every tone
  in the sound makes a grid of them, each dot the opposite of its
  neighbours, and as large as the tone is low: a bass note is a few big
  dots, the highest notes are dots of a pixel, like fine noise, and a
  sound is the grids of all its tones added. **Window** is the scale: the
  shorter it is, the larger everything gets. Positive has the colour set
  by **Field hue**, negative the complementary colour. **Field fade** dims
  the field with distance from the instance's position.

  **Field shape** can be Rings instead of Dots. Then the instance is a
  point that its sound spreads out from in all directions: at a distance
  from it you see the sound of a moment ago, so a tone makes rings round
  it, shells in space. Two instances apart make interference patterns
  where their rings cross. As a volume the shells are thin and
  see-through, like bubbles inside each other, since a solid shell would
  hide everything in it.

  The real swing is far too fast for a screen, so **Lock to wave** works
  like a stroboscope: it takes the picture at the same point of the wave
  every frame, moved on a little each time, and the dots pulse in slow
  motion (rings move outward slowly). **Field speed** is how fast, in
  percent of real time (0 stands still). Unlocked, the picture is of the moment and flickers. The field
  fills the whole picture and is of the two channels' average, whatever
  the layout, and it runs about a tenth of a second behind the sound.

  The field has no edge: it goes on for ever, repeating like a reflection
  where the sound it is made from runs out. Field fade is what bounds it,
  to a ball round the instance's position. With the fade at 0 the volume
  is endless, the camera stands inside it, and what is far away sinks into
  the dark; a low Density lets you see further in.

  **Aperture** gives the volume a depth of field, like opening the lens
  of a camera: what is at the depth of **Focus** stays sharp (0 is the
  centre of the scene, positive nearer to you), and everything else blurs
  the more the further it is in front of or behind that. At 0 all is
  sharp. Sweep Focus to move through the block.

  **EQ by distance** gives every distance from the instance's position its
  own range of pitch. Which pitch is a curve you draw under the controls:
  distance runs to the right (the centre at the left, two plane units and
  beyond at the right), pitch upward (20 Hz to 20 kHz). **EQ width** is how
  wide the range is, in octaves. So a rising curve keeps the low notes'
  large balls in the centre and puts finer and finer ones round them, a
  falling one does the opposite, and a narrow width tunes in to the
  pattern of a single pitch. The curve is saved with the session; it is
  drawn, so the DAW cannot automate it (the width it can). With Sum with,
  the curve of the instance that does the summing goes for all of them,
  each from its own position. A
  short Afterglow keeps it crisp.

  The grid runs along all three axes of the instance's plane, so in space
  the dots are balls. With **Volume** on (the default) you see them all: a
  block of coloured balls in space, the near ones hiding the far ones, so
  turn the view (say yaw 35, pitch 25) to look into it. **Density** is how
  solid they are: low, you see deep into the block; high, only its outside.
  How solid a ball is at each point follows the amplitude, so the wave's
  shape is the ball's: a smooth tone makes soft clouds, a square wave or a
  sharp attack hard-edged blocks. Field fade carves the block down to a ball round the
  instance's position. With Volume off, only the instance's plane is drawn,
  as a cut through the balls: lighter work for the graphics card (a
  Raspberry Pi should use this). With **Sum
  with** every instance added has a field of its own, with its own
  position, turn, hue and fade, and every instance's plane shows all of
  them, added as light: an XY and a YZ plane show the same spheres cut two
  ways.

The **Mono** layout draws one picture, of the two channels' average. On a
mono track the plugin always draws that way, whatever the layout says: there
is no second channel to put beside it.

Colour shows instantaneous frequency (20 Hz red to 20 kHz blue), and brightness
shows the trail's age. **Test tone** shows 20 Hz + 60 Hz on L and
20 Hz + 60.5 Hz on R, so the two channels slowly drift apart.

## Planes

Everything above is drawn on a plane, and the plane sits in a 3D scene. So
with more tracks than one stereo pair, each track's instance can have a plane
of its own in the same space:

- **Plane** is XY (facing you, the flat picture), YZ (the side wall) or XZ
  (the floor). **Turn X / Y / Z** turn the plane further about the scene's
  axes, for any plane in between.
- **Position X / Y / Z** move the plane's centre: x to the right, y up, z
  towards you, with 1 at the edge of the picture's square. So an instance
  can be placed in space as well as turned, like a microphone on a stand.
  In a sum of curves the positions add up, like the points do.
- **View yaw / pitch / roll** are the camera, and turn the whole drawing,
  summed planes and all: yaw walks round it, pitch looks at it from above or
  below, roll turns the picture itself. At rest you look straight at XY, and
  YZ and XZ are edge on, so turn the view (say yaw 35, pitch 20) to see them.
- **The camera pad** shows in the lower left corner of the plugin's scope
  while the mouse is over it. Each control works for as long as it is held.
  **Orbit** takes the camera round the point it looks at (View yaw and
  pitch). **Look** turns it where it stands, like turning your head.
  **Move** has six arrows: right, left, up, down, and forward and back along
  the line of sight. Hold forward and steer with Look to fly through the
  scene. The camera's place is **Camera X / Y / Z**, the point it looks at
  from four units away; like the view angles they are host parameters, so a
  flight can be recorded as automation. A double-click on the pad puts the
  camera back.

**Sum with...** adds other CircleScope instances in the same session to
this instance's drawing: the button lists them by track name, tick the ones
to add. At every moment each instance gives a point on its own plane; the
points are added as vectors, so together they give one point in space and
draw a single curve in 3D. With each track's picture as (u, v) on its
plane, three tracks on XY, YZ and XZ make the point
(u1 + u3, v1 + v2, u2 - v3).

The summing happens in the plugin, so the instance you did this on shows the
whole 3D drawing in its own window, and it is the only one that needs its
Streams switched on to put it on a wall or a node. The drawing has that
instance's look: its layout, window, colours, afterglow, brightness, blend
and view. Every instance added keeps its own mode, delay, band limits, plane
and auto-gain, and the sum is scaled to fill the picture. The choice is saved
with the session.

A sum is of samples from the same moment: the instances in a host share its
clock (the song position while it plays, its audio clock while stopped) and
are lined up by it. A host that runs every plugin in a process of its own
hides the instances from each other, and then the list stays empty.

Instances that are streamed separately to the same stream of a wall or a
node are layers there (see below) and share the scene: their planes cross at
the centre, like the three coordinate planes. There is one camera for all of
them: set the view on any one instance (if several have, the lowest layer's
counts). Every plane in use shows its guides once.

All of these are host parameters, so the DAW can automate a turning plane or
an orbiting camera. Lines keep their thickness whatever the angle. The drawing
modes know nothing of this (`Source/Plane.h`), so a new mode works on every
plane.

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
also answers to a MIDI controller (CC 102 to 119, then 85 to 90 and 20 to 31, then 14, 15, 3 and 9, in the order of
`Source/ScopeParams.h`; 0 to 127 covers the full range; Camera X / Y / Z have none) in the standalone app
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

- **Layer** (1 to 8) is the stacking order, 1 at the bottom.
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

A wall does no analysis: the plugin sends the finished drawing, so every
cell shows exactly what the plugin's own window shows.

To the plugin a wall is one more node: it is found the same way and gets the
same streams, so a wall and a cluster can run side by side. The control app
does not list walls; a wall is set up in its own window.

## Cluster

For an installation with many screens.

Three programs work together on one network:

- **CircleScope** (the plugin): switch on one or more **Streams** (1 to 16)
  and it sends its drawing, plus its parameters, to every node showing
  one of them. It is the finished drawing that is sent, the points of the
  curves in 3D (in Field mode, the sources of the field and their sound) about sixty times a second, not audio: a node only draws, so
  all the screens show the same thing and a Raspberry Pi has little to do.
  It is more data than audio would be (15 to 20 Mbit/s for a full curve, and
  about as much per source for a field with the EQ on), so use wired Ethernet. Instances sharing a stream are layers there too (see Layers
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
address typed in. The drawing and parameters go by UDP from 47801, control and
previews by TCP from 47901. There is no authentication: keep it on a network
you trust. `Source/NetProtocol.h` describes the wire format.

On a Mac with the firewall on, allow incoming connections for CircleScope
Control and the plugin's host when macOS asks, or the beacons never arrive.

`cluster/setup-node.sh` prepares a Raspberry Pi (build, auto-start at boot).
A Pi with no monitor attached needs a forced video mode to have something to
render to: add `video=HDMI-A-1:1920x1080@60D` to `/boot/firmware/cmdline.txt`.

## Layout

- `Source/ScopeParams.h`: the one table of parameters (ranges, defaults, CCs)
- `Source/Plane.h`: the 3D scene: plane orientations, the camera
- `Source/CameraPad.h`: the camera's sticks and arrows in the plugin's scope
- `Source/ScopeSource.h`, `RingBuffer.h`: an instance's sample rings and parameter values
- `Source/CurveAnalyser.*`: audio to drawing: analytic signal, band limits, auto-gain, the modes, summing instances
- `Source/ScopeCurve.h`: the drawing itself: frames of curve points, and the feed that hands them on
- `Source/ScopeComponent.*`: draws frames: OpenGL rendering (lines, the field shaders), layers, wall regions, previews
- `Source/InstanceRegistry.h`: the instances in one host, for Sum with
- `Source/PluginProcessor.*`, `PluginEditor.*`, `ControlPanel.h`, `FullScreenWindow.h`: the plugin
- `Source/NetProtocol.h`, `NetSender.h`, `NetReceiver.h`: wire format, the plugin's sender, the receiver of walls and nodes
- `Source/Wall/WallMain.cpp`: the wall
- `Source/Node/NodeMain.cpp`: the cluster node
- `Source/Control/ControlMain.cpp`: the control center

## To do

- **Frame-locked tiles**, once it can be tested on a real cluster. Every
  node now gets the same frames, so the tiles of a split picture show the
  same curve with the same gain. What is left is when: each node draws a
  frame as it arrives, so two screens can be a frame apart for an instant.
  They could hold each frame until a time stamped on it.

## Ideas next

- Field mode: the volume is drawn at half resolution with 64 steps along
  each line of sight; a quality setting could trade speed for sharpness.

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
