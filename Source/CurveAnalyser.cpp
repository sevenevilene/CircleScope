#include "CurveAnalyser.h"

CurveAnalyser::CurveAnalyser (const ScopeSource& own, scope::CurveFeed& feedToFill, Partners partnersToAdd)
    : Thread ("CircleScope analysis"), source (own), feed (feedToFill), partners (std::move (partnersToAdd))
{
    signals.push_back (std::make_unique<Signal> (source, nullptr));
    timeScratch.resize ((size_t) blockSize);
    freqScratch.resize ((size_t) blockSize);
    bandFreq.resize ((size_t) blockSize);
    bandTime.resize ((size_t) blockSize);
    wave.resize ((size_t) blockSize);
    startThread();
}

CurveAnalyser::~CurveAnalyser()
{
    signalThreadShouldExit();
    feed.request();
    stopThread (2000);
}

// The colour of a pitch on the scene's scale.
juce::Colour CurveAnalyser::colourForFrequency (double hz) const
{
    float rgb[3];
    colours.colourAt (scope::ColourScale::placeOf (hz), rgb);
    return juce::Colour::fromFloatRGBA (rgb[0], rgb[1], rgb[2], 1.0f);
}

CurveAnalyser::Settings CurveAnalyser::readSettings (const ScopeSource& src)
{
    using namespace scope;
    auto get = [&src] (int index) { return src.get (index); };

    // The axis of the Swing mode makes one swing per window length, or
    // swings at its own rate.
    const float swingHz = get (pSwingSync) > 0.5f ? 1000.0f / juce::jmax (1.0f, get (pWindow)) : get (pSwingRate);
    const auto plane = (Plane) juce::jlimit (0, 2, (int) get (pPlane));

    return { (Mode) juce::jlimit (0, 4, (int) get (pMode)),
             get (pColour) > 0.5f,
             get (pGuides) > 0.5f,
             src.mono.load() ? Layout::mono : (Layout) juce::jlimit (0, 3, (int) get (pLayout)),
             get (pSwingMotion) > 0.5f,
             swingHz,
             get (pWindow),
             get (pDelay),
             get (pLowCut),
             get (pHighCut),
             juce::jmax (1000.0, src.sampleRate.load()),
             planeOrientation (plane, get (pPlaneRotX), get (pPlaneRotY), get (pPlaneRotZ)),
             plane != Plane::xy,
             { get (pPosX), get (pPosY), get (pPosZ) },
             get (pFieldFade) * 0.01f,
             get (pFieldLock) > 0.5f,
             get (pFieldSpeed) * 0.01f,
             get (pFieldShape) > 0.5f,
             get (pFieldEq) > 0.5f,
             get (pFieldEqWidth),
             juce::jlimit (-1.0f, 1.0f, get (pFieldPolarity)) };
}

//==============================================================================
CurveAnalyser::Range CurveAnalyser::computeRange (const Settings& s) const
{
    Range r {};
    r.end = blockSize - edgeMargin;
    r.length = juce::jlimit (16, blockSize - 2 * edgeMargin, (int) (s.windowMs * 0.001 * s.sampleRate));
    r.start = r.end - r.length;
    r.stride = juce::jmax (1, r.length / scope::maxCurvePoints);
    r.freqLag = juce::jmax (r.stride, 8);
    return r;
}

// Makes signals the instance's own followed by its partners', keeping the
// ones already there so their auto-gain carries on.
void CurveAnalyser::updateSignals()
{
    std::vector<std::shared_ptr<const ScopeSource>> wanted;
    if (partners != nullptr)
        wanted = partners();

    wanted.erase (std::remove_if (wanted.begin(), wanted.end(), [this] (const auto& p) { return p == nullptr || p.get() == &source; }),
                  wanted.end());
    if ((int) wanted.size() > maxSignals - 1)
        wanted.resize ((size_t) maxSignals - 1);

    for (size_t i = 0; i < wanted.size(); ++i)
    {
        const auto slot = signals.begin() + (std::ptrdiff_t) i + 1;
        auto found = std::find_if (slot, signals.end(), [&] (const auto& sig) { return &sig->source == wanted[i].get(); });
        if (found == signals.end())
            signals.insert (slot, std::make_unique<Signal> (*wanted[i], wanted[i]));
        else
            std::iter_swap (slot, found);
    }
    signals.resize (wanted.size() + 1);
}

// Low cut / high cut as per-bin gains, with a half-octave raised-cosine
// slope at each edge so the band limits do not ring.
void CurveAnalyser::updateBandWeights (Signal& sig)
{
    const auto& s = sig.settings;
    if (juce::exactlyEqual (s.lowCutHz, sig.weightsLowCut) && juce::exactlyEqual (s.highCutHz, sig.weightsHighCut)
        && juce::exactlyEqual (s.sampleRate, sig.weightsSampleRate))
        return;

    sig.weightsLowCut = s.lowCutHz;
    sig.weightsHighCut = s.highCutHz;
    sig.weightsSampleRate = s.sampleRate;

    const bool lowActive = s.lowCutHz > 20.5f, highActive = s.highCutHz < 19999.5f;
    sig.bandLimited = lowActive || highActive;
    if (! sig.bandLimited)
        return;

    // 0 well below the cut, 1 well above it.
    auto edge = [] (double hz, double cutHz)
    {
        const double t = juce::jlimit (0.0, 1.0, std::log2 (juce::jmax (hz, 1.0e-3) / cutHz) * 2.0 + 0.5);
        return 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * t);
    };

    sig.bandWeights.resize ((size_t) blockSize / 2 + 1);
    for (int k = 0; k <= blockSize / 2; ++k)
    {
        const double hz = (double) k * s.sampleRate / (double) blockSize;
        double w = 1.0;
        if (lowActive)  w *= edge (hz, (double) s.lowCutHz);
        if (highActive) w *= 1.0 - edge (hz, (double) s.highCutHz);
        sig.bandWeights[(size_t) k] = (float) w;
    }
}

void CurveAnalyser::analyse (Signal& sig, int channel)
{
    auto& samples = sig.block[channel];

    // Analytic signal: zero the negative frequencies, double the positive.
    for (int i = 0; i < blockSize; ++i)
        timeScratch[(size_t) i] = { samples[(size_t) i], 0.0f };
    fft.perform (timeScratch.data(), freqScratch.data(), false);

    if (sig.bandLimited)
        for (int k = 0; k <= blockSize / 2; ++k)
            freqScratch[(size_t) k] *= sig.bandWeights[(size_t) k];

    for (int k = 1; k < blockSize / 2; ++k)
        freqScratch[(size_t) k] *= 2.0f;
    for (int k = blockSize / 2 + 1; k < blockSize; ++k)
        freqScratch[(size_t) k] = 0.0f;
    fft.perform (freqScratch.data(), sig.analytic[channel].data(), true);

    // Everything that reads the plain samples (delay embed, goniometer,
    // correlation) should see the band-limited signal too.
    if (sig.bandLimited)
        for (int i = 0; i < blockSize; ++i)
            samples[(size_t) i] = sig.analytic[channel][(size_t) i].real();
}

// Of the instance's own signal.
void CurveAnalyser::updateCorrelation (const Range& r)
{
    const auto& block = signals[0]->block;
    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (int i = r.start; i < r.end; ++i)
    {
        const double l = block[0][(size_t) i], rt = block[1][(size_t) i];
        lr += l * rt;
        ll += l * l;
        rr += rt * rt;
    }

    const double energy = std::sqrt (ll * rr);
    const float target = energy > 1.0e-12 ? (float) (lr / energy) : 1.0f;
    correlation += (target - correlation) * 0.2f;
}

// Fills every signal's block. One signal gets its newest samples. Several
// are to be added up, so they should be from the same moment: if they all
// say where they are on the same clock, each gets the samples up to the
// latest moment all of them have reached. Failing that (no host clock, or
// one of them is too far behind), each gets its newest, which is right to
// within the few milliseconds their packets arrive apart.
void CurveAnalyser::readSamples()
{
    constexpr juce::int64 furthestBack = (juce::int64) ScopeSource::ringSize - blockSize - 4096;
    const auto count = signals.size();
    juce::int64 offset[maxSignals] {}, written[maxSignals] {};
    juce::int64 common = std::numeric_limits<juce::int64>::max();
    bool locked = count > 1;

    for (size_t k = 0; k < count; ++k)
    {
        const auto& src = signals[k]->source;
        const int domain = src.clockDomain.load();
        locked = locked && domain != 0 && domain == signals[0]->source.clockDomain.load()
                 && juce::exactlyEqual (src.sampleRate.load(), signals[0]->source.sampleRate.load());

        offset[k] = src.clockOffset.load();
        written[k] = (juce::int64) src.rings[1].getWritePosition();   // pushed after ring 0: the safe one
        common = juce::jmin (common, written[k] + offset[k]);
    }

    for (size_t k = 0; k < count && locked; ++k)
        locked = written[k] + offset[k] - common <= furthestBack;

    for (size_t k = 0; k < count; ++k)
    {
        auto& sig = *signals[k];
        for (int ch = 0; ch < 2; ++ch)
        {
            if (locked)
            {
                sig.blockStart[ch] = (size_t) (common - offset[k]) - (size_t) blockSize;
                sig.source.rings[ch].read (sig.blockStart[ch], sig.block[ch].data(), blockSize);
            }
            else
            {
                sig.blockStart[ch] = sig.source.rings[ch].readLatest (sig.block[ch].data(), blockSize);
            }
        }
    }
}

// A frame each time a scope asks for one, and about sixty a second while
// the feed is streamed, whether a scope is looking or not.
void CurveAnalyser::run()
{
    while (! threadShouldExit())
    {
        const bool streamed = feed.streamed.load();
        const bool asked = feed.waitForRequest (streamed ? 4 : 100);
        const double sinceLast = juce::Time::getMillisecondCounterHiRes() - lastFrameMs;

        if (asked ? sinceLast < 4.0 : ! (streamed && sinceLast >= 15.0))
            continue;   // two scopes asking at once; or nobody wants one

        lastFrameMs = juce::Time::getMillisecondCounterHiRes();
        makeFrame();
    }
}

void CurveAnalyser::makeFrame()
{
    updateSignals();
    colours = scope::SceneColours::get().read();
    for (auto& sig : signals)
        sig->settings = readSettings (sig->source);
    readSamples();

    // What the instance's own settings say goes for the whole picture:
    // its layout, window and colours.
    const auto& s = signals[0]->settings;
    const auto range = computeRange (s);

    for (auto& sig : signals)
    {
        updateBandWeights (*sig);
        analyse (*sig, 0);
        analyse (*sig, 1);
    }
    updateCorrelation (range);

    auto frame = pool.acquire();
    frame->layout = s.layout;
    frame->correlation = correlation;

    // The planes in the picture, and those of them that show their guides.
    for (const auto& sig : signals)
    {
        const auto& plane = sig->settings.plane;
        const auto& at = sig->settings.at;
        frame->flat = frame->flat && plane.isIdentity() && at.isZero();

        bool shown = false;
        for (int k = 0; k < frame->numGuides; ++k)
            shown = shown || (frame->guides[k].isNear (plane) && frame->guidesAt[k].isNear (at));

        if (sig->settings.guides && ! shown && frame->numGuides < scope::maxGuidePlanes)
        {
            frame->guides[frame->numGuides] = plane;
            frame->guidesAt[frame->numGuides++] = at;
        }
    }

    if (s.mode == Mode::field)
    {
        addField (*frame, range);
        feed.publish (std::move (frame));
        return;
    }

    switch (s.layout)
    {
        case Layout::sideBySide:
            addCurve (frame->panels[0], leftCurve, range, std::nullopt);
            addCurve (frame->panels[1], rightCurve, range, std::nullopt);
            break;
        case Layout::overlay:
            addCurve (frame->panels[0], leftCurve, range, scope::leftColour);
            addCurve (frame->panels[0], rightCurve, range, scope::rightColour);
            break;
        case Layout::midSide:
            addCurve (frame->panels[0], goniometerCurve, range, std::nullopt);
            break;
        case Layout::mono:
            addCurve (frame->panels[0], monoCurve, range, std::nullopt);
            break;
    }

    frame->buildTriangles();
    feed.publish (std::move (frame));
}

// The Field mode: every signal is a source of the field. For dots these are
// standing waves, and a standing wave is the sound running out both ways
// from a moment, added up, so each source gives its sound mirrored about
// that moment: sample j is the average of the sound j steps after it and j
// steps before. For rings the sound just spreads, so each gives its sound
// going back from the moment. Where the
// sources are and how they look comes from their settings. The Window is
// how much sound fits in one plane unit.
void CurveAnalyser::addField (scope::Frame& frame, const Range& r)
{
    const auto& lead = *signals[0];
    const auto& s = lead.settings;
    auto mono = [] (const Signal& sig, int i) { return 0.5f * (sig.block[0][(size_t) i] + sig.block[1][(size_t) i]); };

    // The moment needs sound before it, as far as the field reaches (for
    // dots, on both sides of it), and room for the lock to choose in.
    const bool rings = s.fieldRings;
    const int first = edgeMargin, last = blockSize - edgeMargin - 1;
    const int lockRoom = juce::jmin ((int) (0.1 * s.sampleRate), (last - first) / 4);
    const float perUnit = (float) r.length;
    const int span = juce::jmin ((last - first - lockRoom) / (rings ? 1 : 2), (int) (scope::fieldMaxReach * perUnit));
    int moment = rings ? last : last - span;

    // Locked, it works like a stroboscope. The moment is taken from the same
    // point of the instance's own wave every frame: where it last came up
    // through zero (after a proper dip, so ripples on a low note do not
    // count). From there it moves on a little every frame, and starts
    // again after one period, so a steady tone pulses in slow motion.
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double sinceLast = juce::jlimit (0.0, 100.0, nowMs - lastFieldMs);
    lastFieldMs = nowMs;

    if (s.fieldLock)
    {
        fieldCreep += (double) s.fieldSpeed * sinceLast * 0.001 * s.sampleRate;

        float peak = 0.0f;
        for (int i = moment - lockRoom; i <= moment; ++i)
            peak = juce::jmax (peak, std::abs (mono (lead, i)));

        int crossing = -1, before = -1;
        bool dipped = false;
        for (int i = moment - lockRoom; i <= moment && peak > 1.0e-4f; ++i)
        {
            const float v = mono (lead, i);
            if (v < -0.3f * peak)
                dipped = true;
            else if (dipped && v >= 0.0f)
            {
                before = crossing;
                crossing = i;
                dipped = false;
            }
        }

        if (before >= 0)
            moment = before + (int) std::fmod (fieldCreep, (double) (crossing - before));
        else if (crossing >= 0)
            moment = crossing;
    }

    const float step = (float) span / (float) (scope::fieldSamples - 1);

    frame.layout = Layout::mono;
    frame.flat = false;      // it fills the picture, not the panel's square
    frame.reach = (float) span / perUnit;
    frame.rings = rings;
    frame.eq = s.fieldEq;
    frame.eqWidth = s.fieldEqWidth;
    for (int i = 0; i < scope::fieldCurvePoints; ++i)
    {
        frame.curve[i] = juce::jlimit (0.0f, 1.0f, lead.source.fieldCurve[i].load());
        frame.show[i] = juce::jlimit (0.0f, 1.0f, lead.source.showCurve[i].load());
    }

    for (int band = 0; band < scope::fieldBands; ++band)
        colours.colourAt (scope::ColourScale::placeOfBand (band), frame.bandColour[band]);

    frame.numSources = (int) juce::jmin (signals.size(), (size_t) scope::maxFieldSources);

    for (int k = 0; k < frame.numSources; ++k)
    {
        auto& sig = *signals[(size_t) k];
        const auto& set = sig.settings;
        auto& out = frame.sources[k];

        const int around = juce::jmin (span, r.length);
        float peak = 0.0f;
        for (int i = moment - around; i <= moment + (rings ? 0 : around); ++i)
            peak = juce::jmax (peak, std::abs (mono (sig, i)));
        const float target = 1.0f / juce::jmax (peak, 0.01f);
        sig.fieldGain = target < sig.fieldGain ? target : sig.fieldGain + (target - sig.fieldGain) * 0.05f;

        out.plane = set.plane;
        out.at = set.at;
        out.fade = set.fieldFade;
        for (int i = 0; i < scope::fieldCurvePoints; ++i)
            out.fadeCurve[i] = juce::jlimit (0.0f, 1.0f, sig.source.fadeCurve[i].load());

        // The sound once per band of pitch: its spectrum, and of that
        // each band in turn, back as sound. The bands overlap so that
        // together they are the whole sound again.
        for (int i = 0; i < blockSize; ++i)
            timeScratch[(size_t) i] = { mono (sig, i), 0.0f };
        fft.perform (timeScratch.data(), freqScratch.data(), false);

        out.samples.resize ((size_t) (scope::fieldSamples * scope::fieldBands));
        for (int band = 0; band < scope::fieldBands; ++band)
        {
            {
                for (int bin = 0; bin <= blockSize / 2; ++bin)
                {
                    // Where the bin's pitch is among the bands, and how much of it is this band's.
                    const double hz = (double) bin * s.sampleRate / (double) blockSize;
                    const double place = juce::jlimit (0.5, (double) scope::fieldBands - 0.5,
                                                       std::log2 (juce::jmax (hz, 1.0) / 20.0) / (double) scope::fieldBandOctaves);
                    const double off = juce::jmin (1.0, std::abs (place - ((double) band + 0.5)));
                    const double c = std::cos (juce::MathConstants<double>::halfPi * off);
                    const float weight = (float) (c * c);

                    bandFreq[(size_t) bin] = freqScratch[(size_t) bin] * weight;
                    if (bin > 0 && bin < blockSize / 2)
                        bandFreq[(size_t) (blockSize - bin)] = freqScratch[(size_t) (blockSize - bin)] * weight;
                }
                fft.perform (bandFreq.data(), bandTime.data(), true);
                for (int i = 0; i < blockSize; ++i)
                    wave[(size_t) i] = bandTime[(size_t) i].real();
            }

            // Each band is handed on filling -1..1, with how strong it
            // really is beside it: where it is on the colour map goes by
            // its own swing, not by how loud it is next to the others.
            auto* to = out.samples.data() + (size_t) (band * scope::fieldSamples);
            float bandPeak = 0.0f;
            for (int j = 0; j < scope::fieldSamples; ++j)
            {
                const int away = juce::roundToInt ((float) j * step);
                const float v = rings ? wave[(size_t) (moment - away)]
                                      : 0.5f * (wave[(size_t) (moment + away)] + wave[(size_t) (moment - away)]);
                to[j] = v * sig.fieldGain;
                bandPeak = juce::jmax (bandPeak, std::abs (to[j]));
            }

            const bool heard = bandPeak > 1.0e-4f;
            // The instance's polarity: its size goes with the level, and
            // a negative one turns the wave over.
            const float turned = set.fieldPolarity < 0.0f ? -1.0f : 1.0f;
            out.level[band] = heard ? juce::jmin (bandPeak, 4.0f) * std::abs (set.fieldPolarity) : 0.0f;
            for (int j = 0; j < scope::fieldSamples; ++j)
                to[j] = heard ? turned * to[j] / bandPeak : 0.0f;
        }
    }
}

// Auto-gain of one signal's picture on its plane: snap down on loud input,
// ease back up when it gets quieter. Returns what to multiply samples by.
float CurveAnalyser::updateGain (Signal& sig, Curve curve, const Range& r)
{
    constexpr float k = 0.70710678f;
    float peak = 0.0f;

    if (curve == goniometerCurve)
    {
        for (int i = r.start; i < r.end; ++i)
        {
            const float l = sig.block[0][(size_t) i], rt = sig.block[1][(size_t) i];
            peak = juce::jmax (peak, std::abs (l + rt) * k, std::abs (rt - l) * k);
        }
    }
    else if (curve == monoCurve)
    {
        for (int i = r.start; i < r.end; ++i)
            peak = juce::jmax (peak, 0.5f * std::abs (sig.analytic[0][(size_t) i] + sig.analytic[1][(size_t) i]));
    }
    else
    {
        for (int i = r.start; i < r.end; ++i)
            peak = juce::jmax (peak, std::abs (sig.analytic[curve][(size_t) i]));
    }

    auto& gain = sig.gain[curve];
    const float target = scope::plotRadius / juce::jmax (peak, 0.01f);
    gain = target < gain ? target : gain + (target - gain) * 0.05f;
    return curve == goniometerCurve ? gain * k : gain;
}

// One curve of the picture: a channel's trajectory, the trajectory of the
// two channels' average (mono), or the goniometer's.
// Each signal gives a point on its own plane, by its own mode; the planes'
// points are added up in the scene, so several planes make one point in
// space, and one plane alone is just its picture, wherever the plane is.
void CurveAnalyser::addCurve (scope::Frame::Panel& out, Curve curve, const Range& r,
                              std::optional<juce::Colour> colourOverride)
{
    const auto& lead = *signals[0];
    const auto& s = lead.settings;

    points.assign ((size_t) ((r.length + r.stride - 1) / r.stride), Point3());
    Point3 centre;     // of the summed picture: the planes' centres, added up

    for (auto& sig : signals)
    {
        const auto& set = sig->settings;
        const auto& m = set.plane.m;
        const float g = updateGain (*sig, curve, r);
        centre.x += set.at.x;
        centre.y += set.at.y;
        centre.z += set.at.z;
        const int channel = curve == rightCurve ? 1 : 0;
        const int delay = juce::jlimit (1, r.start, (int) (set.delayMs * 0.001 * s.sampleRate));

        // Swing mode: the axis angle follows the samples' absolute position, so
        // a point keeps its place on screen for as long as it is shown.
        const double swingsPerSample = (double) set.swingHz / s.sampleRate;
        auto swingAngle = [&] (int i)
        {
            const double swings = (double) (sig->blockStart[channel] + (size_t) i) * swingsPerSample;
            const double phase = juce::MathConstants<double>::twoPi * (swings - std::floor (swings));
            return set.swingRound ? phase : juce::MathConstants<double>::pi * std::sin (phase);
        };

        size_t n = 0;
        for (int i = r.start; i < r.end; i += r.stride, ++n)
        {
            float u = 0.0f, v = 0.0f;

            if (curve == goniometerCurve)
            {
                const float l = sig->block[0][(size_t) i], rt = sig->block[1][(size_t) i];
                u = (rt - l) * g;
                v = (l + rt) * g;
            }
            else
            {
                const bool mono = curve == monoCurve;
                const auto zi = mono ? 0.5f * (sig->analytic[0][(size_t) i] + sig->analytic[1][(size_t) i])
                                     : sig->analytic[channel][(size_t) i];
                u = zi.real() * g;

                switch (set.mode)
                {
                    case Mode::field:       // as a partner of an instance that draws curves
                    case Mode::quadrature: v = zi.imag() * g; break;
                    case Mode::delayEmbed:
                    {
                        const auto past = (size_t) (i - delay);
                        v = (mono ? 0.5f * (sig->block[0][past] + sig->block[1][past]) : sig->block[channel][past]) * g;
                        break;
                    }
                    case Mode::swing:
                    {
                        const double angle = swingAngle (i);
                        v = u * (float) std::sin (angle);
                        u *= (float) std::cos (angle);
                        break;
                    }
                    case Mode::axis:
                        // One axis each: y on XY, z on YZ, x on XZ.
                        if (! set.axisAlongU)
                            std::swap (u, v);
                        break;
                }
            }

            auto& p = points[n];
            p.x += m[0] * u + m[3] * v;
            p.y += m[1] * u + m[4] * v;
            p.z += m[2] * u + m[5] * v;
        }
    }

    // A sum of several full-size pictures is brought back to full size.
    auto& sum = sumGain[curve];
    if (signals.size() > 1)
    {
        float peak = 0.0f;
        for (const auto& p : points)
            peak = juce::jmax (peak, std::sqrt (p.x * p.x + p.y * p.y + p.z * p.z));
        const float target = scope::plotRadius / juce::jmax (peak, 0.01f);
        sum = target < sum ? target : sum + (target - sum) * 0.05f;
        for (auto& p : points)
        {
            p.x *= sum;
            p.y *= sum;
            p.z *= sum;
        }
    }
    else
    {
        sum = 1.0f;
    }

    // Colour: fixed, or the hue of the lead signal's instantaneous
    // frequency, from its phase advance over freqLag samples.
    const juce::Colour fixed = colourOverride.value_or (curve == leftCurve ? scope::leftColour
                                                        : curve == rightCurve ? scope::rightColour : scope::goniometerColour);
    const bool useHue = ! s.fixedColour && ! colourOverride.has_value();
    auto phasor = [&] (int i)
    {
        return curve == leftCurve || curve == rightCurve ? lead.analytic[curve][(size_t) i]
                                                         : lead.analytic[0][(size_t) i] + lead.analytic[1][(size_t) i];
    };

    if (! out.points.empty())
        out.secondStart = (int) out.points.size();

    size_t n = 0;
    for (int i = r.start; i < r.end; i += r.stride, ++n)
    {
        juce::Colour col = fixed;
        if (useHue)
            col = colourForFrequency (std::abs (std::arg (phasor (i) * std::conj (phasor (i - r.freqLag)))) * s.sampleRate
                                      / (juce::MathConstants<double>::twoPi * r.freqLag));

        const Point3 p { points[n].x + centre.x, points[n].y + centre.y, points[n].z + centre.z };
        const float age = (float) (i - r.start) / (float) r.length;
        out.points.push_back ({ p.x, p.y, p.z, col.getFloatRed(), col.getFloatGreen(), col.getFloatBlue(), 0.04f + 0.86f * age * age });
    }
}
