/*
  ==============================================================================
    ArtifactDetectorTests.cpp
    Pink Noise Audio Monitor (pnam) — offline fault-injection rig.

    Runs the real PinkNoiseGenerator through a simulated signal path into the
    real ArtifactDetector, far faster than real time, injects known faults at
    known moments and checks that each is reported — once, on the right
    channel, at the right time — and that hours of clean signal report nothing.

    Build and run:  cmake --build <dir> --target pnam_tests && <dir>/pnam_tests
    Exit code is the number of failed checks.
  ==============================================================================
*/

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "../Source/Core/ArtifactDetector.h"
#include "../Source/Core/PinkNoiseGenerator.h"

using namespace pnam;

namespace
{
int failures = 0;

void check (bool ok, const std::string& what)
{
    std::printf ("    %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());

    if (! ok)
        ++failures;
}

//==============================================================================
struct OnePole
{
    double a = 0.0, x1 = 0.0, y1 = 0.0;

    void setHighPass (double fs, double fc)   { a = 1.0 / (1.0 + juce::MathConstants<double>::twoPi * fc / fs); }

    float process (float x)
    {
        const double y = a * (y1 + (double) x - x1);
        x1 = x;
        y1 = y;
        return (float) y;
    }
};

struct LowPassBiquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void set (double fs, double fc, double q)
    {
        const double w0 = juce::MathConstants<double>::twoPi * fc / fs;
        const double alpha = std::sin (w0) / (2.0 * q), cosw = std::cos (w0), a0 = 1.0 + alpha;
        b0 = (1.0 - cosw) * 0.5 / a0;
        b1 = (1.0 - cosw) / a0;
        b2 = b0;
        a1 = -2.0 * cosw / a0;
        a2 = (1.0 - alpha) / a0;
    }

    float process (float x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float) y;
    }
};

/** One channel's simulated round trip: gain, delay (integer + fractional, and
    optionally drifting), and — when `realistic` — the things real converters
    do: AC coupling at both ends, a steep anti-alias low-pass, a noise floor. */
struct Path
{
    double gain = 0.5;
    double delaySamples = 1000.0;
    double driftPpm = 0.0;
    bool   realistic = false;
    double noiseFloorDb = -96.0;

    std::vector<float> line;
    int64_t written = 0;
    OnePole hpA, hpB;
    LowPassBiquad lp[3];
    std::mt19937 rng { 12345 };
    std::normal_distribution<float> gauss { 0.0f, 1.0f };

    void prepare (double fs, uint32_t seed)
    {
        line.assign (1 << 19, 0.0f);
        written = 0;
        hpA.setHighPass (fs, 5.0);
        hpB.setHighPass (fs, 5.0);
        const double q[3] = { 0.5176, 0.7071, 1.9319 };

        for (int i = 0; i < 3; ++i)
            lp[i].set (fs, 0.45 * fs, q[i]);

        rng.seed (seed);
    }

    float readDelayed (double delay) const
    {
        // 16-tap Hann-windowed sinc: a fractional delay good to the top of the band.
        const double pos = (double) (written - 1) - delay;
        const int64_t base = (int64_t) std::floor (pos);
        const double frac = pos - (double) base;
        const int mask = (int) line.size() - 1;

        if (frac < 1.0e-9)
            return base >= 0 ? line[(size_t) (base & mask)] : 0.0f;

        double sum = 0.0;

        for (int k = -7; k <= 8; ++k)
        {
            const double t = (double) k - frac;
            const double sinc = std::sin (juce::MathConstants<double>::pi * t) / (juce::MathConstants<double>::pi * t);
            const double window = 0.5 * (1.0 + std::cos (juce::MathConstants<double>::pi * t / 8.5));
            const int64_t idx = base + k;

            if (idx >= 0)
                sum += sinc * window * (double) line[(size_t) (idx & mask)];
        }

        return (float) sum;
    }

    void process (const float* in, float* out, int n)
    {
        const int mask = (int) line.size() - 1;

        for (int i = 0; i < n; ++i)
        {
            line[(size_t) (written & mask)] = in[i];
            ++written;

            delaySamples += driftPpm * 1.0e-6;
            float v = (float) gain * readDelayed (delaySamples);

            if (realistic)
            {
                v = hpB.process (hpA.process (v));

                for (auto& f : lp)
                    v = f.process (v);

                v += gauss (rng) * (float) std::pow (10.0, noiseFloorDb / 20.0);
            }

            out[i] = v;
        }
    }
};

//==============================================================================
struct Sim
{
    double fs = 48000.0;
    int    block = 512;
    int    numCh = 1;

    PinkNoiseGenerator generator;
    EventQueue queue { 4096 };
    ArtifactDetector detector { queue };

    std::vector<Path> paths;
    std::vector<std::vector<float>> refData, inData;
    std::vector<float*> refPtrs, inPtrs;
    std::vector<Event> events;

    int64_t sample = 0;
    bool muted = false;
    std::vector<double> callbackMs;     // filled when timing is on
    bool timing = false;

    /** Called for every block after the path, before the detector: this is
        where faults are injected. (channel buffers, first sample index, n) */
    std::function<void (std::vector<std::vector<float>>&, int64_t, int)> inject;

    void prepare (double sampleRate, int blockSize, int channels, bool realistic)
    {
        fs = sampleRate;
        block = blockSize;
        numCh = channels;

        generator.setSeed (0x1234abcdULL);
        generator.prepare (fs, block);
        generator.setStereoMode (PinkNoiseGenerator::StereoMode::IndependentStereo);
        generator.startFade (true);

        detector.prepare (fs, block, numCh, numCh);
        detector.resetSessionState();

        paths.assign ((size_t) numCh, {});
        refData.assign ((size_t) numCh, std::vector<float> ((size_t) block));
        inData.assign ((size_t) numCh, std::vector<float> ((size_t) block));
        refPtrs.resize ((size_t) numCh);
        inPtrs.resize ((size_t) numCh);

        for (int ch = 0; ch < numCh; ++ch)
        {
            paths[(size_t) ch].realistic = realistic;
            paths[(size_t) ch].delaySamples = 5000.0 + 913.0 * ch + (realistic ? 0.37 : 0.0);
            paths[(size_t) ch].prepare (fs, (uint32_t) (77 + ch));
            refPtrs[(size_t) ch] = refData[(size_t) ch].data();
            inPtrs[(size_t) ch] = inData[(size_t) ch].data();
            detector.setReferenceOutputForInput (ch, ch);
            detector.setInputMonitored (ch, true);
        }

        events.clear();
        sample = 0;
    }

    double now() const   { return (double) sample / fs; }

    void run (double seconds)
    {
        const int64_t end = sample + (int64_t) (seconds * fs);

        while (sample < end)
        {
            generator.processBlock (refPtrs.data(), numCh, block);

            for (int ch = 0; ch < numCh; ++ch)
                paths[(size_t) ch].process (refPtrs[(size_t) ch], inPtrs[(size_t) ch], block);

            if (inject)
                inject (inData, sample, block);

            // Same suppression the engine applies: a settling window after
            // start, and the mute / fade bits.
            int32_t status = StatusBits::ok;

            if (now() < 3.0)                 status |= StatusBits::settling;
            if (generator.isMuted() || muted) status |= StatusBits::outputMuted;
            if (generator.isFadeActive())     status |= StatusBits::fadingIn;

            const auto t0 = timing ? juce::Time::getHighResolutionTicks() : 0;
            detector.processBlock (inPtrs.data(), numCh, refPtrs.data(), numCh, block, status, now());

            if (timing)
                callbackMs.push_back (1000.0 * juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0));

            detector.serviceAcquisitions();

            queue.popAll ([this] (const Event& e) { events.push_back (e); });
            sample += block;
        }
    }

    int count (EventType type, int ch = -1, int32_t code = -1, double from = 0.0, double to = 1.0e12) const
    {
        int n = 0;

        for (const auto& e : events)
            if (e.type == type && (ch < 0 || e.channelIndex == ch) && (code < 0 || e.code == code)
                && e.elapsedSeconds >= from && e.elapsedSeconds <= to)
                ++n;

        return n;
    }

    int countAll (double from = 0.0) const
    {
        int n = 0;

        for (const auto& e : events)
            if (e.elapsedSeconds >= from)
                ++n;

        return n;
    }

    const Event* first (EventType type, int ch = -1, int32_t code = -1) const
    {
        for (const auto& e : events)
            if (e.type == type && (ch < 0 || e.channelIndex == ch) && (code < 0 || e.code == code))
                return &e;

        return nullptr;
    }

    void dump (const char* indent = "      ") const
    {
        for (const auto& e : events)
            std::printf ("%s%9.3fs  ch%d  %-14s code=%d  measured=%.2f expected=%.2f diff=%.3f\n", indent,
                         e.elapsedSeconds, e.channelIndex, toString (e.type), e.code,
                         e.measuredValue, e.expectedValue, e.difference);
    }
};

/** Signal RMS of the returned channel over a block, for scaling faults. */
double rmsOf (const std::vector<float>& v)
{
    double s = 0.0;
    for (float x : v) s += (double) x * (double) x;
    return std::sqrt (s / (double) v.size());
}

inline bool inBlock (int64_t at, int64_t start, int n)   { return at >= start && at < start + n; }

//==============================================================================
void testCleanRuns()
{
    struct Case { double fs; int block; bool realistic; double minutes; const char* name; };

    const Case cases[] = {
        { 48000.0,  512,  false, 20.0, "ideal path, 48 kHz / 512" },
        { 48000.0,  512,  true,  30.0, "realistic path, 48 kHz / 512" },
        { 48000.0,  64,   true,  10.0, "realistic path, 48 kHz / 64" },
        { 96000.0,  1024, true,  10.0, "realistic path, 96 kHz / 1024" },
        { 192000.0, 512,  true,  10.0, "realistic path, 192 kHz / 512" },
    };

    for (const auto& c : cases)
    {
        std::printf ("\n  Clean run: %s, %.0f min\n", c.name, c.minutes);

        Sim sim;
        sim.prepare (c.fs, c.block, 2, c.realistic);
        sim.run (8.0);

        const bool locked = sim.detector.isLocked (0) && sim.detector.isLocked (1);
        check (locked, "both channels aligned within 8 s");

        const double trueMs0 = sim.paths[0].delaySamples * 1000.0 / c.fs;
        const double gotMs0 = sim.detector.getLatencyMs (0);
        std::printf ("      latency ch0: true %.4f ms, measured %.4f ms   residual ch0 %.1f dB, ch1 %.1f dB\n",
                     trueMs0, gotMs0, sim.detector.getResidualDb (0), sim.detector.getResidualDb (1));
        check (std::abs (gotMs0 - trueMs0) < (c.realistic ? 0.15 : 0.03), "measured latency matches the path");

        sim.run (c.minutes * 60.0);
        check (sim.events.empty(), "no events at all on clean signal (" + std::to_string (sim.events.size()) + " reported)");

        if (! sim.events.empty())
            sim.dump();
    }
}

void testClicks()
{
    std::printf ("\n  Clicks of decreasing size, realistic path, 48 kHz\n");

    // Single-sample clicks, sized relative to the return's own RMS.
    const double sizesDb[] = { 20.0, 6.0, 0.0, -6.0, -12.0, -20.0, -30.0 };

    for (double sizeDb : sizesDb)
    {
        Sim sim;
        sim.prepare (48000.0, 512, 2, true);
        sim.run (10.0);

        const double rms = rmsOf (sim.inData[0]);
        const int64_t at = sim.sample + 48000 * 2 + 137;
        const float amp = (float) (rms * std::pow (10.0, sizeDb / 20.0));

        sim.inject = [at, amp] (std::vector<std::vector<float>>& in, int64_t start, int n)
        {
            if (inBlock (at, start, n))
                in[0][(size_t) (at - start)] += amp;
        };

        sim.run (6.0);

        const int clicks = sim.count (EventType::Click, 0);
        const auto* e = sim.first (EventType::Click, 0);
        const bool timed = e != nullptr && std::abs (e->elapsedSeconds - (double) at / 48000.0) < 0.002;
        const bool others = (int) sim.events.size() != clicks;

        std::printf ("      click %+5.0f dB re signal RMS: %d Click event(s)%s%s\n", sizeDb, clicks,
                     timed ? ", timed to within 2 ms" : "", others ? ", PLUS OTHER EVENTS" : "");

        if (sizeDb >= -20.0)
        {
            check (clicks == 1 && timed, "reported exactly once at the right time");
            check (e != nullptr && e->severity == (sizeDb >= -12.0 ? Severity::Warning : Severity::Info),
                   sizeDb >= -12.0 ? "as a Click event (Warning)" : "as a small click (Info summary), not a Warning");
            check (sim.count (EventType::Click, 1) == 0, "nothing on the other channel");
            check (! others, "no other event types");
        }

        if (others)
            sim.dump();
    }
}

void testTimingSlips()
{
    std::printf ("\n  Dropped and repeated samples, realistic path, 48 kHz\n");

    const int slips[] = { -64, 64, -512, 1024, -1, 1, -4096 };

    for (int slip : slips)
    {
        Sim sim;
        sim.prepare (48000.0, 512, 2, true);
        sim.run (10.0);

        const int64_t at = sim.sample + 48000 * 2;
        bool done = false;

        // Dropping samples shortens the path's delay; repeating lengthens it.
        sim.inject = [&sim, &done, at, slip] (std::vector<std::vector<float>>&, int64_t start, int n)
        {
            if (! done && start + n > at)
            {
                sim.paths[0].delaySamples += (double) slip;
                done = true;
            }
        };

        sim.run (8.0);

        const auto* e = sim.first (EventType::TimingSlip, 0);
        const int slipEvents = sim.count (EventType::TimingSlip, 0);

        std::printf ("      slip %+5d samples: %d TimingSlip event(s)", slip, slipEvents);
        if (e != nullptr)
            std::printf (", reported %+.2f samples at %.3f s (true %.3f s)", e->measuredValue, e->elapsedSeconds, (double) at / 48000.0);
        std::printf (", %d Click, %d other\n", sim.count (EventType::Click, 0),
                     (int) sim.events.size() - slipEvents - sim.count (EventType::Click, 0));

        check (slipEvents == 1, "reported exactly once");
        check (e != nullptr && std::abs (e->measuredValue - (double) slip) < 0.5, "size right to within half a sample");
        check (e != nullptr && std::abs (e->elapsedSeconds - (double) at / 48000.0) < 0.15, "timed to within 150 ms");
        check (sim.count (EventType::Noise) + sim.count (EventType::Distortion) + sim.count (EventType::SignalMismatch) == 0,
               "not also reported as noise, distortion or mismatch");
        check (sim.countAll() == sim.count (EventType::TimingSlip, 0) + sim.count (EventType::Click, 0),
               "nothing on the other channel");

        if (slipEvents != 1)
            sim.dump();
    }
}

void testNoiseAndDistortion()
{
    std::printf ("\n  Sustained noise and distortion, realistic path, 48 kHz\n");

    struct Case { const char* name; int kind; double levelDb; EventType expect; bool mustDetect; };

    const Case cases[] = {
        { "static: white noise 10 dB under the signal, 2 s",   0, -10.0, EventType::Noise,      true },
        { "static: white noise 20 dB under the signal, 2 s",   0, -20.0, EventType::Noise,      true },
        { "hiss: white noise 30 dB under the signal, 2 s",     0, -30.0, EventType::Noise,      false },
        { "crackle: 200 random ticks per second, 2 s",         1,   0.0, EventType::Noise,      true },
        { "hard clipping at 2.0x RMS, 2 s",                    2,   2.0, EventType::Distortion, true },
        { "hard clipping at 3.0x RMS, 2 s",                    2,   3.0, EventType::Distortion, false },
        { "soft saturation (tanh) driven to 1.5x RMS, 2 s",    3,   1.5, EventType::Distortion, true },
        { "buzz: 120 Hz sawtooth 15 dB under the signal, 2 s", 4, -15.0, EventType::Noise,      true },
        { "hum: 60 Hz sine 15 dB under the signal, 2 s",       5, -15.0, EventType::Noise,      false },
    };

    for (const auto& c : cases)
    {
        Sim sim;
        sim.prepare (48000.0, 512, 2, true);
        sim.run (10.0);

        const double rms = rmsOf (sim.inData[0]);
        const int64_t from = sim.sample + 48000 * 2, to = from + 48000 * 2;
        std::mt19937 rng (99);
        std::normal_distribution<float> gauss (0.0f, 1.0f);
        std::uniform_real_distribution<float> uni (0.0f, 1.0f);
        const float level = (float) (rms * std::pow (10.0, c.levelDb / 20.0));
        const int kind = c.kind;
        const double param = c.levelDb;

        sim.inject = [=, &rng, &gauss, &uni] (std::vector<std::vector<float>>& in, int64_t start, int n) mutable
        {
            for (int i = 0; i < n; ++i)
            {
                const int64_t t = start + i;

                if (t < from || t >= to)
                    continue;

                float& v = in[0][(size_t) i];

                switch (kind)
                {
                    case 0: v += gauss (rng) * level; break;
                    case 1: if (uni (rng) < 200.0f / 48000.0f) v += (float) rms * (uni (rng) > 0.5f ? 1.0f : -1.0f); break;
                    case 2: v = juce::jlimit ((float) (-param * rms), (float) (param * rms), v); break;
                    case 3: v = (float) (param * rms * std::tanh ((double) v / (param * rms))); break;
                    case 4: v += level * 1.7f * (float) (2.0 * std::fmod ((double) t * 120.0 / 48000.0, 1.0) - 1.0); break;
                    case 5: v += level * 1.414f * (float) std::sin (juce::MathConstants<double>::twoPi * 60.0 * (double) t / 48000.0); break;
                    default: break;
                }
            }
        };

        sim.run (8.0);

        const double t0 = (double) from / 48000.0, t1 = (double) to / 48000.0;
        const int starts = sim.count (EventType::Noise, 0, eventCodeStart) + sim.count (EventType::Distortion, 0, eventCodeStart);
        const int ends   = sim.count (EventType::Noise, 0, eventCodeEnd) + sim.count (EventType::Distortion, 0, eventCodeEnd);
        const Event* start = sim.first (EventType::Noise, 0, eventCodeStart);
        if (start == nullptr) start = sim.first (EventType::Distortion, 0, eventCodeStart);
        const Event* end = sim.first (EventType::Noise, 0, eventCodeEnd);
        if (end == nullptr) end = sim.first (EventType::Distortion, 0, eventCodeEnd);

        std::printf ("      %-50s -> ", c.name);

        if (start != nullptr)
            std::printf ("%s %.2f..%.2f s (true %.2f..%.2f), residual %.1f dB vs baseline %.1f dB, %d clicks\n",
                         toString (start->type), start->elapsedSeconds, end != nullptr ? end->elapsedSeconds : -1.0,
                         t0, t1, start->measuredValue, start->expectedValue, sim.count (EventType::Click, 0));
        else
            std::printf ("not reported as sustained (%d clicks, residual now %.1f dB)\n",
                         sim.count (EventType::Click, 0), sim.detector.getResidualDb (0));

        if (c.mustDetect)
        {
            check (starts == 1 && ends == 1, "exactly one start/end pair");
            check (start != nullptr && start->type == c.expect, std::string ("classified as ") + toString (c.expect));
            check (start != nullptr && std::abs (start->elapsedSeconds - t0) < 0.25, "start within 250 ms");
            check (end != nullptr && std::abs (end->elapsedSeconds - t1) < 0.4, "end within 400 ms");
        }

        check (sim.count (EventType::TimingSlip) + sim.count (EventType::SignalMismatch) == 0, "not reported as a slip or mismatch");
        check (sim.countAll() == sim.count (EventType::Noise, 0) + sim.count (EventType::Distortion, 0) + sim.count (EventType::Click, 0),
               "nothing on the other channel");

        if (c.mustDetect && (starts != 1 || ends != 1))
            sim.dump();
    }
}

void testThingsThatAreNotArtifacts()
{
    std::printf ("\n  Things that must NOT be reported as artifacts, realistic path, 48 kHz\n");

    struct Case { const char* name; int kind; };
    const Case cases[] = {
        { "level drops 10 dB and stays there",          0 },
        { "level rises 6 dB and stays there",           1 },
        { "complete dropout for 2 s, then back",        2 },
        { "output muted for 3 s, then unmuted",         3 },
        { "20 ppm clock drift for 5 minutes",           4 },
        { "polarity flips (a path was re-patched)",     5 },
    };

    for (const auto& c : cases)
    {
        Sim sim;
        sim.prepare (48000.0, 512, 2, true);

        // Drift is a property of the rig (two clocks), so it is there from the start.
        if (c.kind == 4)
            sim.paths[0].driftPpm = 20.0;

        sim.run (10.0);

        const int64_t from = sim.sample + 48000 * 2, to = from + 48000 * 2;
        const int kind = c.kind;
        const double runFor = kind == 4 ? 300.0 : 10.0;

        sim.inject = [&sim, kind, from, to] (std::vector<std::vector<float>>& in, int64_t start, int n)
        {
            if (kind == 3)
                sim.muted = start >= from && start < from + 48000 * 3;

            for (int i = 0; i < n; ++i)
            {
                const int64_t t = start + i;
                float& v = in[0][(size_t) i];

                if (kind == 0 && t >= from)             v *= 0.316f;
                if (kind == 1 && t >= from)             v *= 2.0f;
                if (kind == 2 && t >= from && t < to)   v = 0.0f;
                if (kind == 3 && sim.muted)             v = 0.0f;
                if (kind == 5 && t >= from)             v = -v;
            }
        };

        const double startDelay = sim.paths[0].delaySamples;
        sim.run (runFor);

        std::printf ("      %-44s -> %d event(s)", c.name, (int) sim.events.size());

        if (kind == 4)
            std::printf (", latency followed the drift: path moved %+.2f samples, measured %.4f ms vs true %.4f ms",
                         sim.paths[0].delaySamples - startDelay, sim.detector.getLatencyMs (0),
                         sim.paths[0].delaySamples * 1000.0 / 48000.0);

        std::printf ("\n");

        // A polarity flip is a real change to the path and one Click at the
        // instant it happens is fair; nothing else is.
        const int allowed = kind == 5 ? sim.count (EventType::Click, 0) : 0;
        check ((int) sim.events.size() == allowed, "no artifact events");
        check (sim.detector.isLocked (0), "still aligned afterwards");

        if ((int) sim.events.size() != allowed)
            sim.dump();
    }
}

void testMismatchAndGarbage()
{
    std::printf ("\n  Wrong signal and corrupted bursts, realistic path, 48 kHz\n");

    {
        // The return is replaced by a different, unrelated pink noise for 10 s.
        Sim sim;
        sim.prepare (48000.0, 512, 2, true);
        sim.run (10.0);

        PinkNoiseGenerator other;
        other.setSeed (0x9999ULL);
        other.prepare (48000.0, 512);
        other.startFade (true);
        std::vector<float> junk (512);
        float* junkPtr = junk.data();

        const int64_t from = sim.sample + 48000 * 2, to = from + 48000 * 10;

        sim.inject = [&] (std::vector<std::vector<float>>& in, int64_t start, int n)
        {
            other.processBlock (&junkPtr, 1, n);

            if (start >= from && start < to)
                for (int i = 0; i < n; ++i)
                    in[0][(size_t) i] = junk[(size_t) i] * 0.5f;
        };

        sim.run (22.0);

        const auto* s = sim.first (EventType::SignalMismatch, 0, eventCodeStart);
        const auto* e = sim.first (EventType::SignalMismatch, 0, eventCodeEnd);
        std::printf ("      unrelated signal for 10 s -> ");
        if (s != nullptr)
            std::printf ("SignalMismatch %.2f..%.2f s (true %.2f..%.2f)\n", s->elapsedSeconds, e != nullptr ? e->elapsedSeconds : -1.0,
                         (double) from / 48000.0, (double) to / 48000.0);
        else
            std::printf ("not reported\n");

        check (sim.count (EventType::SignalMismatch, 0, eventCodeStart) == 1 && sim.count (EventType::SignalMismatch, 0, eventCodeEnd) == 1,
               "exactly one mismatch start/end pair");
        check (s != nullptr && std::abs (s->elapsedSeconds - (double) from / 48000.0) < 0.3, "start within 300 ms of the swap");
        check (e != nullptr && e->elapsedSeconds - (double) to / 48000.0 < 2.5, "ended within 2.5 s of the signal returning");
        check (sim.count (EventType::TimingSlip) == 0, "no timing slip reported");
        check (sim.detector.isLocked (0), "aligned again afterwards");
        check (sim.count (EventType::SignalMismatch, 1) + sim.count (EventType::Noise, 1) + sim.count (EventType::Click, 1) == 0,
               "nothing on the other channel");
        sim.dump();
    }

    for (double burstMs : { 20.0, 150.0, 800.0 })
    {
        // A burst of unrelated noise replaces the signal, then it comes back
        // exactly where it was.
        Sim sim;
        sim.prepare (48000.0, 512, 2, true);
        sim.run (10.0);

        const double rms = rmsOf (sim.inData[0]);
        const int64_t from = sim.sample + 48000 * 2, to = from + (int64_t) (burstMs * 48.0);
        std::mt19937 rng (5);
        std::normal_distribution<float> gauss (0.0f, 1.0f);

        sim.inject = [&, from, to] (std::vector<std::vector<float>>& in, int64_t start, int n)
        {
            for (int i = 0; i < n; ++i)
                if (start + i >= from && start + i < to)
                    in[0][(size_t) i] = gauss (rng) * (float) rms;
        };

        sim.run (8.0);

        const int reported = sim.count (EventType::Noise, 0) + sim.count (EventType::Click, 0);
        std::printf ("      %4.0f ms of corrupted audio -> %d Click, %d Noise, %d TimingSlip, %d SignalMismatch\n", burstMs,
                     sim.count (EventType::Click, 0), sim.count (EventType::Noise, 0),
                     sim.count (EventType::TimingSlip, 0), sim.count (EventType::SignalMismatch, 0));

        check (reported >= 1, "reported");
        check (sim.count (EventType::TimingSlip) == 0, "not mistaken for a timing slip");
        check (sim.count (EventType::SignalMismatch) == 0, "not escalated to a mismatch");
        check (sim.detector.isLocked (0), "aligned again afterwards");
        check (sim.count (EventType::Noise, 0, eventCodeStart) == sim.count (EventType::Noise, 0, eventCodeEnd),
               "every start has its end");
    }
}

void testManyChannels()
{
    std::printf ("\n  16 channels, a click on channel 11 only, 96 kHz / 256\n");

    Sim sim;
    sim.prepare (96000.0, 256, 16, true);
    sim.run (10.0);

    int lockedCount = 0;
    for (int ch = 0; ch < 16; ++ch)
        lockedCount += sim.detector.isLocked (ch) ? 1 : 0;

    check (lockedCount == 16, "all 16 aligned within 10 s (" + std::to_string (lockedCount) + ")");

    const double rms = rmsOf (sim.inData[11]);
    const int64_t at = sim.sample + 96000 * 2 + 5;

    sim.inject = [at, rms] (std::vector<std::vector<float>>& in, int64_t start, int n)
    {
        if (inBlock (at, start, n))
            in[11][(size_t) (at - start)] += (float) rms * 2.0f;
    };

    sim.run (60.0);
    check (sim.count (EventType::Click, 11) == 1 && sim.events.size() == 1, "one Click, on channel 11, nothing else in a minute");

    if (sim.events.size() != 1)
        sim.dump();
}

void reportFloors()
{
    std::printf ("\n  Residual floor on a clean realistic path (lower = more sensitive)\n");

    struct Case { double fs; int block; };

    for (const auto& c : { Case { 44100.0, 512 }, Case { 48000.0, 512 }, Case { 96000.0, 512 }, Case { 192000.0, 512 } })
    {
        Sim sim;
        sim.prepare (c.fs, c.block, 1, true);
        sim.run (9.0);
        std::printf ("      %6.1f kHz: residual %.1f dB, latency error %.4f ms, locked=%d\n", c.fs / 1000.0,
                     sim.detector.getResidualDb (0),
                     sim.detector.getLatencyMs (0) - sim.paths[0].delaySamples * 1000.0 / c.fs, (int) sim.detector.isLocked (0));
    }
}

/** The figure the live delay monitor shows, and the arithmetic its
    calibration relies on: (device in the path) - (interface alone). */
void testDelayMeasurement()
{
    std::printf ("\n  Delay measurement (what the delay monitor shows), realistic path\n");

    struct Case { double fs; double delayMs; };

    for (const auto& c : { Case { 48000.0, 2.0 },    Case { 48000.0, 22.93 },  Case { 48000.0, 300.0 },
                           Case { 48000.0, 600.0 },  Case { 44100.0, 300.0 },  Case { 96000.0, 300.0 },
                           Case { 192000.0, 300.0 } })
    {
        Sim sim;
        sim.prepare (c.fs, 512, 1, true);
        sim.paths[0].delaySamples = c.delayMs * c.fs / 1000.0 + 0.37;
        sim.run (10.0);

        const double trueMs = sim.paths[0].delaySamples * 1000.0 / c.fs;
        const double gotMs  = sim.detector.getLatencyMs (0);

        std::printf ("      %5.1f kHz, path delay %8.3f ms -> measured %8.3f ms (error %+.3f ms), aligned=%d\n",
                     c.fs / 1000.0, trueMs, gotMs, gotMs - trueMs, (int) sim.detector.isAligned (0));
        check (sim.detector.isAligned (0), "aligned within 10 s");
        check (std::abs (gotMs - trueMs) < 0.05, "measured delay within 0.05 ms of the path");
        check (sim.events.empty(), "no events");
    }

    {
        // Calibrate on the interface alone, then put a device with exactly
        // 300 ms of delay in the path (no gap in the signal), then take it out.
        Sim sim;
        sim.prepare (48000.0, 512, 1, true);
        sim.paths[0].delaySamples = 1100.37;
        sim.run (10.0);

        const double calibrationMs = sim.detector.getLatencyMs (0);

        const auto settle = [&sim] (double expectedMs)
        {
            // Seconds until the reading is live again and within 1 ms of where it should end up.
            for (int half = 1; half <= 40; ++half)
            {
                sim.run (0.5);

                if (sim.detector.isAligned (0) && std::abs ((double) sim.detector.getLatencyMs (0) - expectedMs) < 1.0)
                    return 0.5 * half;
            }

            return -1.0;
        };

        sim.paths[0].delaySamples += 14400.0;
        const double inSeconds = settle (calibrationMs + 300.0);
        sim.run (3.0);
        const double withDevice = (double) sim.detector.getLatencyMs (0) - calibrationMs;

        sim.paths[0].delaySamples -= 14400.0;
        const double outSeconds = settle (calibrationMs);
        sim.run (3.0);
        const double without = (double) sim.detector.getLatencyMs (0) - calibrationMs;

        std::printf ("      calibrated at %.3f ms; +300 ms device in: reads %.3f ms after %.1f s; device out: reads %.3f ms after %.1f s\n",
                     calibrationMs, withDevice, inSeconds, without, outSeconds);
        check (inSeconds > 0.0 && inSeconds <= 8.0, "follows the device going in within 8 s");
        check (std::abs (withDevice - 300.0) < 0.02, "reads 300 ms to within 0.02 ms");
        check (outSeconds > 0.0 && outSeconds <= 8.0, "follows it coming out within 8 s");
        check (std::abs (without) < 0.02, "and returns to 0 to within 0.02 ms");

        // What the swap leaves in the log: one timing slip each way, nothing else.
        std::printf ("      events from the two swaps:\n");
        sim.dump ("        ");
        check (sim.count (EventType::TimingSlip, 0) == 2 && sim.events.size() == 2,
               "each swap is logged as one timing slip and nothing else");
    }

    {
        // Beyond what can be found: must not show a wrong number.
        Sim sim;
        sim.prepare (48000.0, 512, 1, true);
        sim.paths[0].delaySamples = 0.9 * 48000.0;
        sim.run (12.0);
        std::printf ("      path delay 900 ms (beyond the ~0.68 s search range) -> aligned=%d\n", (int) sim.detector.isAligned (0));
        check (! sim.detector.isAligned (0), "not reported as aligned");
        sim.dump ("        ");
        check (sim.count (EventType::SignalMismatch, 0, eventCodeStart) == 1 && sim.events.size() == 1,
               "logged as one signal mismatch");
    }
}

void benchmark()
{
    std::printf ("\n  Cost on this machine (audio-thread work only, every channel aligned and running)\n");

    struct Case { double fs; int block; int channels; };

    for (const auto& c : { Case { 48000.0, 512, 8 }, Case { 96000.0, 512, 16 }, Case { 96000.0, 512, 32 }, Case { 192000.0, 512, 64 } })
    {
        Sim sim;
        sim.prepare (c.fs, c.block, c.channels, true);

        // Keep every path inside the delay the detector can find at this rate.
        for (int ch = 0; ch < c.channels; ++ch)
            sim.paths[(size_t) ch].delaySamples = 900.0 + 37.0 * ch + 0.37;

        sim.run (8.0);

        int aligned = 0;
        for (int ch = 0; ch < c.channels; ++ch)
            aligned += sim.detector.isLocked (ch) ? 1 : 0;

        sim.timing = true;
        sim.run (4.0);

        std::sort (sim.callbackMs.begin(), sim.callbackMs.end());
        const double median = sim.callbackMs[sim.callbackMs.size() / 2];
        const double worst  = sim.callbackMs.back();
        const double budgetMs = c.block * 1000.0 / c.fs;

        std::printf ("      %2d channels (%2d aligned) at %3.0f kHz / %d: median %.3f ms = %.0f%% of the %.2f ms deadline, worst %.3f ms = %.0f%%\n",
                     c.channels, aligned, c.fs / 1000.0, c.block, median, 100.0 * median / budgetMs, budgetMs,
                     worst, 100.0 * worst / budgetMs);
        check (aligned == c.channels, "all channels aligned");
    }
}

//==============================================================================
struct HighPassBiquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;

    void set (double fs, double fc, double q)
    {
        const double w0 = juce::MathConstants<double>::twoPi * fc / fs;
        const double alpha = std::sin (w0) / (2.0 * q), cosw = std::cos (w0), a0 = 1.0 + alpha;
        b0 = (1.0 + cosw) * 0.5 / a0;
        b1 = -(1.0 + cosw) / a0;
        b2 = b0;
        a1 = -2.0 * cosw / a0;
        a2 = (1.0 - alpha) / a0;
    }

    float process (float x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float) y;
    }
};

/** Cases an independent review found the first version got wrong. Each is here
    so it cannot quietly come back. */
void testReviewRegressions()
{
    std::printf ("\n  Regressions from review, realistic path, 48 kHz\n");

    {
        // Noise as loud as the signal, for 30 s: the return still correlates
        // but cannot be modelled. It must be reported WHILE it is happening.
        Sim sim;
        sim.prepare (48000.0, 512, 2, true);
        sim.run (10.0);

        const double rms = rmsOf (sim.inData[0]);
        const int64_t from = sim.sample + 48000 * 2, to = from + 48000 * 30;
        std::mt19937 rng (3);
        std::normal_distribution<float> gauss (0.0f, 1.0f);

        sim.inject = [&, from, to] (std::vector<std::vector<float>>& in, int64_t start, int n)
        {
            for (int i = 0; i < n; ++i)
                if (start + i >= from && start + i < to)
                    in[0][(size_t) i] += gauss (rng) * (float) rms;
        };

        sim.run (12.0);     // 10 s into the fault
        const int duringFault = (int) sim.events.size();
        const auto* s = sim.first (EventType::SignalMismatch, 0, eventCodeStart);
        const auto* n = sim.first (EventType::Noise, 0, eventCodeStart);
        std::printf ("      noise as loud as the signal: %d event(s) in the first 10 s of it\n", duringFault);
        check (duringFault >= 1 && (s != nullptr || n != nullptr), "reported while it is still happening");
        check ((s != nullptr ? s->elapsedSeconds : n != nullptr ? n->elapsedSeconds : 1.0e9) < (double) from / 48000.0 + 4.0,
               "start stamped within 4 s of the onset");

        sim.run (30.0);     // past the end, and long enough to settle
        check (sim.count (EventType::SignalMismatch, 0, eventCodeStart) == sim.count (EventType::SignalMismatch, 0, eventCodeEnd)
                 && sim.count (EventType::Noise, 0, eventCodeStart) == sim.count (EventType::Noise, 0, eventCodeEnd),
               "every start has its end once it stops");
        check (sim.detector.isLocked (0), "aligned again afterwards");
        check (sim.count (EventType::SignalMismatch, 1) + sim.count (EventType::Noise, 1) + sim.count (EventType::Click, 1) == 0,
               "nothing on the other channel");
    }

    struct PathCase { const char* name; int kind; };

    for (const auto& pc : { PathCase { "steep high-pass in the path (24 dB/oct at 150 Hz)", 0 },
                            PathCase { "resonant EQ in the path (+6 dB at 250 Hz, Q 4)", 1 },
                            PathCase { "mains hum already present (60/180/300 Hz, 10 dB under the signal)", 2 },
                            PathCase { "inverted polarity", 3 } })
    {
        // Ordinary things a healthy path can contain, present from the start:
        // five minutes must report nothing.
        Sim sim;
        sim.prepare (48000.0, 512, 2, true);

        HighPassBiquad hpA, hpB;
        hpA.set (48000.0, 150.0, 0.5412);
        hpB.set (48000.0, 150.0, 1.3066);

        // peaking EQ, +6 dB at 250 Hz, Q 4 (RBJ)
        double pb0, pb1, pb2, pa1, pa2, pz1 = 0.0, pz2 = 0.0;
        {
            const double A = std::pow (10.0, 6.0 / 40.0), w0 = juce::MathConstants<double>::twoPi * 250.0 / 48000.0;
            const double alpha = std::sin (w0) / (2.0 * 4.0), a0 = 1.0 + alpha / A;
            pb0 = (1.0 + alpha * A) / a0;  pb1 = -2.0 * std::cos (w0) / a0;  pb2 = (1.0 - alpha * A) / a0;
            pa1 = pb1;                     pa2 = (1.0 - alpha / A) / a0;
        }

        const int kind = pc.kind;
        double humLevel = 0.0;

        sim.inject = [&] (std::vector<std::vector<float>>& in, int64_t start, int n)
        {
            if (kind == 2 && humLevel == 0.0 && start > 48000)
                humLevel = rmsOf (in[0]) * std::pow (10.0, -10.0 / 20.0);

            for (int i = 0; i < n; ++i)
            {
                float& v = in[0][(size_t) i];
                const double t = (double) (start + i) / 48000.0;

                if (kind == 0)   v = hpB.process (hpA.process (v));
                if (kind == 3)   v = -v;

                if (kind == 1)
                {
                    const double y = pb0 * v + pz1;
                    pz1 = pb1 * v - pa1 * y + pz2;
                    pz2 = pb2 * v - pa2 * y;
                    v = (float) y;
                }

                if (kind == 2)
                    for (double f : { 60.0, 180.0, 300.0 })
                        v += (float) (humLevel * 0.816 * std::sin (juce::MathConstants<double>::twoPi * f * t));
            }
        };

        sim.run (10.0);
        const bool aligned = sim.detector.isLocked (0);
        const double latencyError = sim.detector.getLatencyMs (0) - sim.paths[0].delaySamples * 1000.0 / 48000.0;
        sim.run (300.0);

        std::printf ("      %-66s -> %d event(s) in 5 min, residual %.1f dB, latency error %+.3f ms\n", pc.name,
                     (int) sim.events.size(), sim.detector.getResidualDb (0), latencyError);
        check (aligned, "aligned within 10 s");
        check (sim.events.empty(), "no events");

        if (kind == 3)
            check (std::abs (latencyError) < 0.05, "an inverted path is aligned on the true delay, not a sidelobe");

        if (! sim.events.empty())
            sim.dump();
    }

    {
        // Seen on real hardware (a UMC404HD over USB): a faint tick, about
        // 24 to 28 dB under the signal, every couple of minutes. Real, so it must be
        // counted; trivial, so it must not fill the log.
        Sim sim;
        sim.prepare (48000.0, 512, 2, true);
        sim.run (10.0);
        const double rms = rmsOf (sim.inData[0]);
        const int64_t first = sim.sample + 48000;

        sim.inject = [first, rms] (std::vector<std::vector<float>>& in, int64_t start, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                const int64_t t = start + i - first;

                if (t >= 0 && t % (48000 * 2) < 3)      // a 3-sample tick every 2 s
                    in[0][(size_t) i] += (float) (rms * 0.04);    // 28 dB under the signal
            }
        };

        int counted = 0;
        for (int second = 0; second < 120; ++second)
        {
            sim.run (1.0);
            counted += sim.detector.takeIntervalFigures (0).clickCount;
        }

        const int warnings = (int) std::count_if (sim.events.begin(), sim.events.end(),
                                                  [] (const Event& e) { return e.severity != Severity::Info; });
        std::printf ("      a faint tick every 2 s for two minutes (60 ticks)                    -> %d counted, %d event(s), %d above Info\n",
                     counted, (int) sim.events.size(), warnings);
        check (counted >= 57 && counted <= 60, "every tick is counted in the interval figures");
        check (sim.events.size() == 1 && warnings == 0, "one Info summary row, not sixty warnings");

        if (sim.events.size() != 1)
            sim.dump();
    }

    {
        // The operator turns the output down 20 dB mid-run. The path's noise
        // floor is now 20 dB closer to the signal; that is not a fault.
        Sim sim;
        sim.prepare (48000.0, 512, 2, true);
        sim.run (12.0);
        sim.generator.setLevelDb (-40.0);
        sim.run (60.0);
        std::printf ("      output level lowered by 20 dB mid-run                               -> %d event(s) in the next minute\n",
                     (int) sim.events.size());
        check (sim.events.empty(), "no events, and no start left without an end");

        if (! sim.events.empty())
            sim.dump();

        // ...and a click afterwards is still caught at the new level.
        const double rms = rmsOf (sim.inData[0]);
        const int64_t at = sim.sample + 48000 + 11;
        sim.inject = [at, rms] (std::vector<std::vector<float>>& in, int64_t start, int n)
        {
            if (inBlock (at, start, n))
                in[0][(size_t) (at - start)] += (float) rms * 2.0f;
        };
        sim.run (4.0);
        check (sim.count (EventType::Click, 0) == 1, "a click is still detected at the new level");
    }

    {
        // Monitoring switched off for 5 s and back on.
        Sim sim;
        sim.prepare (48000.0, 512, 2, true);
        sim.run (12.0);
        sim.detector.setInputMonitored (0, false);
        sim.run (5.0);
        const bool forgotten = ! sim.detector.isLocked (0);
        sim.detector.setInputMonitored (0, true);
        sim.run (20.0);
        std::printf ("      monitoring switched off for 5 s and back on                         -> %d event(s)\n", (int) sim.events.size());
        check (forgotten, "not reported as aligned while unmonitored");
        check (sim.events.empty(), "no events");
        check (sim.detector.isLocked (0), "aligned again");
    }

    {
        // Every channel slips at once (a clock event): each must be on record.
        Sim sim;
        sim.prepare (48000.0, 512, 24, true);
        sim.run (12.0);
        const int64_t at = sim.sample + 48000;
        bool done = false;

        sim.inject = [&] (std::vector<std::vector<float>>&, int64_t start, int n)
        {
            if (! done && start + n > at)
            {
                for (auto& p : sim.paths)
                    p.delaySamples += 64.0;

                done = true;
            }
        };

        sim.run (12.0);
        int reported = 0;
        for (int ch = 0; ch < 24; ++ch)
            reported += sim.count (EventType::TimingSlip, ch) == 1 ? 1 : 0;

        std::printf ("      24 channels slip 64 samples at the same instant                     -> %d of 24 reported once, %d other event(s)\n",
                     reported, (int) sim.events.size() - sim.count (EventType::TimingSlip));
        check (reported == 24, "every channel's slip is reported exactly once");
        check ((int) sim.events.size() == sim.count (EventType::TimingSlip), "and nothing else");
    }

    {
        // One NaN sample from a misbehaving driver must not blind the channel.
        Sim sim;
        sim.prepare (48000.0, 512, 2, true);
        sim.run (12.0);
        const int64_t at = sim.sample + 48000;
        sim.inject = [at] (std::vector<std::vector<float>>& in, int64_t start, int n)
        {
            if (inBlock (at, start, n))
                in[0][(size_t) (at - start)] = std::numeric_limits<float>::quiet_NaN();
        };
        sim.run (20.0);
        std::printf ("      one NaN sample on the input                                         -> aligned=%d, residual %.1f dB, %d event(s)\n",
                     (int) sim.detector.isLocked (0), sim.detector.getResidualDb (0), (int) sim.events.size());
        check (sim.detector.isLocked (0) && sim.detector.getResidualDb (0) < -30.0f, "still aligned and measuring afterwards");
        check ((int) sim.events.size() <= 1 && (int) sim.events.size() == sim.count (EventType::Click, 0), "at most one Click for the bad sample");
    }
}
} // namespace

//==============================================================================
int main (int argc, char** argv)
{
    const juce::String only = argc > 1 ? juce::String (argv[1]) : juce::String();
    const auto wanted = [&only] (const char* name) { return only.isEmpty() || only == name; };

    std::printf ("Pink Noise Audio Monitor - artifact detector fault-injection tests\n");

    if (wanted ("clean"))     testCleanRuns();
    if (wanted ("clicks"))    testClicks();
    if (wanted ("slips"))     testTimingSlips();
    if (wanted ("noise"))     testNoiseAndDistortion();
    if (wanted ("benign"))    testThingsThatAreNotArtifacts();
    if (wanted ("mismatch"))  testMismatchAndGarbage();
    if (wanted ("channels"))  testManyChannels();
    if (wanted ("delay"))     testDelayMeasurement();
    if (wanted ("review"))    testReviewRegressions();
    if (wanted ("floor"))     reportFloors();
    if (wanted ("bench"))     benchmark();

    std::printf ("\n%s (%d failed check%s)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures, failures == 1 ? "" : "s");
    return failures;
}
