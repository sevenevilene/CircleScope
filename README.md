# CircleScope

Stereo "circle" oscilloscope plugin (AU / VST3 / Standalone, JUCE + OpenGL).
Each channel is drawn as a 2D trajectory in its own square:

- **Quadrature**: x = signal, y = the same signal shifted 90° (via an FFT
  analytic signal). A pure sine is a perfect circle.
- **Delay embed**: x = s(t), y = s(t − delay). The default delay of 12.5 ms
  is a quarter period of 20 Hz, so a 20 Hz sine is a circle.

Colour shows instantaneous frequency (20 Hz blue to 20 kHz red), and brightness
shows the trail's age. **Test tone** shows 20 Hz + 60 Hz on L and
20 Hz + 60.5 Hz on R, so the two channels slowly drift apart.

## Build

```bash
git clone --depth 1 --branch 8.0.9 https://github.com/juce-framework/JUCE.git external/JUCE
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

The build copies the AU to `~/Library/Audio/Plug-Ins/Components` and the VST3
to `~/Library/Audio/Plug-Ins/VST3`. To validate the AU: `auval -v aufx Crc1 Cscp`.

## Layout

- `Source/RingBuffer.h`: lock-free buffer that carries samples from the audio thread to the render thread
- `Source/PluginProcessor.*`: passes audio through untouched, copies it into the rings, generates the test tone
- `Source/ScopeComponent.*`: analytic signal, auto-gain, thick-line OpenGL rendering
- `Source/PluginEditor.*`: controls

## Ideas next

3D delay embedding (s(t), s(t−τ), s(t−2τ)) with rotation; mid/side view;
port the renderer to Metal; a cymatics (Chladni plate) mode.
