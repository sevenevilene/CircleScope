#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include "ScopeParams.h"

// The colours of the scene.
//
// There is one colour scale for everything drawn: a gradient over pitch,
// from 20 Hz to 20 kHz in octaves, set by stops the user places and
// colours. Every mode takes its colours from it.
//
//   Curves  each point has the colour of the pitch the sound has there.
//   Field   the scale is one edge of a map with pitch upward and amplitude
//           across. At amplitude +1 a pitch has its colour of the scale, at
//           -1 the opposite hue (as bright and as strong), and in between
//           the two are blended. So a tone's colour swings with its phase.
//           The field is split into bands of pitch, each coloured from its
//           row of the map, and where bands overlap their colours are
//           averaged by strength.
//
// The scale belongs to the scene, not to an instance: all the instances in
// a host share the one here (as InstanceRegistry shares the instances). An
// instance saves a copy with its state, with the time of the last change,
// and the newest one loaded wins. A wall or node needs none: curves arrive
// coloured, and a field's frame carries its bands' colours.
namespace scope
{
    constexpr int maxColourStops = 12;
    constexpr float colourScaleOctaves = 9.9658f;   // 20 Hz to 20 kHz

    struct ColourStop
    {
        float at = 0.0f;                         // 0 is 20 Hz, 1 is 20 kHz
        float r = 1.0f, g = 1.0f, b = 1.0f;
    };

    struct ColourScale
    {
        int count = 0;
        ColourStop stops[maxColourStops];        // in order of "at"

        // Red at 20 Hz through yellow, green and cyan to blue at 20 kHz.
        static ColourScale standard()
        {
            ColourScale s;
            s.count = 5;
            s.stops[0] = { 0.0f,    1.0f, 0.2f,   0.2f };
            s.stops[1] = { 0.2525f, 1.0f, 1.0f,   0.2f };
            s.stops[2] = { 0.5051f, 0.2f, 1.0f,   0.2f };
            s.stops[3] = { 0.7576f, 0.2f, 1.0f,   1.0f };
            s.stops[4] = { 1.0f,    0.2f, 0.232f, 1.0f };
            return s;
        }

        void sort()
        {
            std::stable_sort (stops, stops + count, [] (const ColourStop& a, const ColourStop& b) { return a.at < b.at; });
        }

        void colourAt (float at, float rgb[3]) const noexcept
        {
            if (count <= 0)
            {
                rgb[0] = rgb[1] = rgb[2] = 1.0f;
                return;
            }

            int next = 0;
            while (next < count && stops[next].at < at)
                ++next;

            const auto& a = stops[juce::jmax (0, next - 1)];
            const auto& b = stops[juce::jmin (count - 1, next)];
            const float t = b.at - a.at > 1.0e-6f ? juce::jlimit (0.0f, 1.0f, (at - a.at) / (b.at - a.at)) : 0.0f;
            rgb[0] = a.r + (b.r - a.r) * t;
            rgb[1] = a.g + (b.g - a.g) * t;
            rgb[2] = a.b + (b.b - a.b) * t;
        }

        // Where a pitch is on the scale.
        static float placeOf (double hz) noexcept
        {
            return (float) juce::jlimit (0.0, 1.0, std::log2 (juce::jmax (hz, 1.0) / 20.0) / (double) colourScaleOctaves);
        }

        // Where the middle of a band of the field is (see ScopeParams.h).
        static float placeOfBand (int band) noexcept
        {
            return juce::jlimit (0.0f, 1.0f, ((float) band + 0.5f) * fieldBandOctaves / colourScaleOctaves);
        }

        juce::String toString() const
        {
            juce::StringArray parts;
            for (int i = 0; i < count; ++i)
                for (float v : { stops[i].at, stops[i].r, stops[i].g, stops[i].b })
                    parts.add (juce::String (v, 4));
            return parts.joinIntoString (",");
        }

        // False if the text is not a scale.
        bool fromString (const juce::String& text)
        {
            const auto parts = juce::StringArray::fromTokens (text, ",", "");
            if (parts.size() < 8 || parts.size() % 4 != 0 || parts.size() > maxColourStops * 4)
                return false;

            count = parts.size() / 4;
            for (int i = 0; i < count; ++i)
            {
                auto value = [&parts, i] (int k) { return juce::jlimit (0.0f, 1.0f, parts[i * 4 + k].getFloatValue()); };
                stops[i] = { value (0), value (1), value (2), value (3) };
            }
            sort();
            return true;
        }
    };

    // The scene's scale, for all the instances in this process.
    class SceneColours
    {
    public:
        static SceneColours& get()
        {
            static SceneColours colours;
            return colours;
        }

        ColourScale read() const
        {
            const juce::SpinLock::ScopedLockType lock (mutex);
            return scale;
        }

        // Changes with every write, so whoever shows or uses the scale can tell.
        juce::uint32 getVersion() const noexcept { return version.load(); }

        // When it was last changed by hand (ms since 1970), 0 if never.
        juce::int64 getStamp() const
        {
            const juce::SpinLock::ScopedLockType lock (mutex);
            return stamp;
        }

        void write (const ColourScale& newScale, juce::int64 changedAt)
        {
            {
                const juce::SpinLock::ScopedLockType lock (mutex);
                scale = newScale;
                scale.sort();
                stamp = changedAt;
            }
            ++version;
        }

        // From a saved state: taken only if it is newer than what is here.
        void offer (const ColourScale& saved, juce::int64 changedAt)
        {
            if (changedAt > getStamp())
                write (saved, changedAt);
        }

    private:
        mutable juce::SpinLock mutex;
        ColourScale scale = ColourScale::standard();
        juce::int64 stamp = 0;
        std::atomic<juce::uint32> version { 1 };
    };
}
