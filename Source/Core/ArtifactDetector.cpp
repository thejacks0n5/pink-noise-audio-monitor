/*
  ==============================================================================
    ArtifactDetector.cpp
    Pink Noise Audio Monitor (pnam)
    See ArtifactDetector.h for what this does and the real-time rules.
  ==============================================================================
*/

#include "ArtifactDetector.h"
#include <cmath>

namespace pnam
{

namespace
{
    // The comparison band (see the header for why the bottom is cut). The top
    // is cut too: converters' anti-alias filters reshape the last third of the
    // band in ways a short model cannot follow, and leaving it in costs ~20 dB
    // of residual floor for no detection benefit.
    constexpr double analysisHpHz = 200.0;
    constexpr double analysisLpFractionOfRate = 0.30;
    constexpr double analysisLpMaxHz = 16000.0;

    // The path model's main tap sits here in the window, leaving room either
    // side for fractional delay and for the delay to wander before recentring.
    constexpr int    centreTap = ArtifactDetector::numTaps / 2;            // index from the OLDEST tap
    constexpr int    centreDelayOffset = ArtifactDetector::numTaps - 1 - centreTap;

    constexpr float  adaptStep = 0.2f;     // with the regularised denominator, ~0.1 effective

    // Capture lengths for the cross-correlation, in samples at ~48 kHz (higher
    // rates are decimated into this). The search covers lags up to half of it.
    constexpr int    fullCaptureSamples  = 65536;   // finds latencies up to ~0.68 s
    constexpr int    shortCaptureSamples = 16384;   // re-finds one that moved by up to ~0.17 s
    constexpr float  acquisitionMinPeakToMean = 12.0f;

    constexpr double convergeSeconds        = 0.3;    // after lock: adapt only
    constexpr double baselineSegmentSeconds = 0.5;    // three of these make the baseline
    constexpr double lockLossHoldSeconds    = 0.06;
    constexpr float  lockLossRatioDb        = -3.0f;
    constexpr double verifyLockSeconds      = 0.5;
    constexpr float  verifyLockRatioDb      = -6.0f;
    constexpr double mismatchAfterSeconds   = 3.0;
    constexpr double clickMergeSeconds      = 0.005;  // outliers this close are one click
    constexpr double clickMaxSeconds        = 0.01;   // an outlier run longer than this is not a click
    constexpr int    maxClickEventsPerSecondPerChannel = 4;
    constexpr int    maxEventsPerSecond     = 20;
    constexpr float  noiseEndHysteresisDb   = 4.0f;
    constexpr float  slipMinSamples         = 0.6f;
    constexpr double clickResidualFactor    = 6.0;
    constexpr double clickHoldSeconds       = 0.1;
    constexpr double smallClickReportSeconds = 3600.0;    // a click waits this long in case it was the start of something larger
    constexpr double crackleRatePerSecond   = 20.0;   // this many clicks a second is no longer "clicks"
    constexpr double crackleQuietSeconds    = 0.3;    // ...and it has stopped after this long without one
    constexpr double clickRateSeconds       = 0.5;
    constexpr float  stableResidualMarginDb = 6.0f;

    constexpr double presentPowerFloor      = 1.0e-8; // -80 dBFS, high-passed
    constexpr double tinyPower              = 1.0e-20;

    constexpr int32_t suppressingBits = StatusBits::outputMuted | StatusBits::fadingIn | StatusBits::fadingOut
                                      | StatusBits::settling | StatusBits::inputInactive;

    inline float powerRatioDb (double numerator, double denominator) noexcept
    {
        if (numerator <= tinyPower || denominator <= tinyPower)
            return numerator <= tinyPower ? silenceFloorDb : 0.0f;

        return (float) (10.0 * std::log10 (numerator / denominator));
    }

    inline float amplitudeDb (double amplitude) noexcept
    {
        return amplitude <= 1.0e-10 ? silenceFloorDb : (float) (20.0 * std::log10 (amplitude));
    }

    inline double smoothing (double blockSeconds, double timeConstantSeconds) noexcept
    {
        return 1.0 - std::exp (-blockSeconds / timeConstantSeconds);
    }

    /** Dot product over the model's taps, written as four independent sums so
        the compiler can vectorise it (a single float accumulator cannot be
        reordered without -ffast-math). This is the audio thread's hot loop. */
    inline float dotTaps (const float* a, const float* b) noexcept
    {
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;

        for (int j = 0; j < ArtifactDetector::numTaps; j += 4)
        {
            s0 += a[j]     * b[j];
            s1 += a[j + 1] * b[j + 1];
            s2 += a[j + 2] * b[j + 2];
            s3 += a[j + 3] * b[j + 3];
        }

        return (s0 + s1) + (s2 + s3);
    }

    int nextPowerOfTwo (int n) noexcept
    {
        int p = 1;
        while (p < n)
            p <<= 1;
        return p;
    }
}

//==============================================================================
void ArtifactDetector::Biquad::setHighPass (double rate, double frequency) noexcept
{
    const double w0 = juce::MathConstants<double>::twoPi * frequency / rate;
    const double cosw = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * 0.7071067811865476);
    const double a0 = 1.0 + alpha;

    b0 = (1.0 + cosw) * 0.5 / a0;
    b1 = -(1.0 + cosw) / a0;
    b2 = b0;
    a1 = -2.0 * cosw / a0;
    a2 = (1.0 - alpha) / a0;
}

void ArtifactDetector::Biquad::setLowPass (double rate, double frequency, double q) noexcept
{
    const double w0 = juce::MathConstants<double>::twoPi * frequency / rate;
    const double cosw = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;

    b0 = (1.0 - cosw) * 0.5 / a0;
    b1 = (1.0 - cosw) / a0;
    b2 = b0;
    a1 = -2.0 * cosw / a0;
    a2 = (1.0 - alpha) / a0;
}

void ArtifactDetector::AnalysisFilter::prepare (double rate) noexcept
{
    const double lowPassHz = juce::jmin (analysisLpMaxHz, analysisLpFractionOfRate * rate);

    highPass.setHighPass (rate, analysisHpHz);
    lowPassA.setLowPass (rate, lowPassHz, 0.5411961);   // 4th-order Butterworth, as two sections
    lowPassB.setLowPass (rate, lowPassHz, 1.3065630);
    highPass.reset();
    lowPassA.reset();
    lowPassB.reset();
}

//==============================================================================
ArtifactDetector::ArtifactDetector (EventQueue& queueToFill)
    : eventQueue (queueToFill),
      channels (new Channel[maxChannels])
{
    setSettings ({});
}

ArtifactDetector::~ArtifactDetector() = default;

void ArtifactDetector::setSettings (const Settings& s) noexcept
{
    sEnabled.store (s.enabled, std::memory_order_relaxed);
    sClickSigma.store (s.clickSigma, std::memory_order_relaxed);
    sClickMinRelDb.store (s.clickMinRelDb, std::memory_order_relaxed);
    sClickEventRelDb.store (s.clickEventRelDb, std::memory_order_relaxed);
    sNoiseRiseDb.store (s.noiseRiseDb, std::memory_order_relaxed);
    sNoiseMinRatioDb.store (s.noiseMinRatioDb, std::memory_order_relaxed);
    sDistortionPeakDb.store (s.distortionPeakDb, std::memory_order_relaxed);
    sNoiseHoldMs.store (s.noiseHoldMs, std::memory_order_relaxed);
    sNoiseEndHoldMs.store (s.noiseEndHoldMs, std::memory_order_relaxed);
}

ArtifactDetector::Settings ArtifactDetector::loadSettings() const noexcept
{
    Settings s;
    s.enabled          = sEnabled.load (std::memory_order_relaxed);
    s.clickSigma       = sClickSigma.load (std::memory_order_relaxed);
    s.clickMinRelDb    = sClickMinRelDb.load (std::memory_order_relaxed);
    s.clickEventRelDb  = sClickEventRelDb.load (std::memory_order_relaxed);
    s.noiseRiseDb      = sNoiseRiseDb.load (std::memory_order_relaxed);
    s.noiseMinRatioDb  = sNoiseMinRatioDb.load (std::memory_order_relaxed);
    s.distortionPeakDb = sDistortionPeakDb.load (std::memory_order_relaxed);
    s.noiseHoldMs      = sNoiseHoldMs.load (std::memory_order_relaxed);
    s.noiseEndHoldMs   = sNoiseEndHoldMs.load (std::memory_order_relaxed);
    return s;
}

ArtifactDetector::Settings ArtifactDetector::getSettings() const noexcept   { return loadSettings(); }

void ArtifactDetector::setReferenceOutputForInput (int inputChannel, int outputChannel) noexcept
{
    if (juce::isPositiveAndBelow (inputChannel, maxChannels))
        channels[(size_t) inputChannel].referenceOutput.store (outputChannel, std::memory_order_relaxed);
}

void ArtifactDetector::setInputMonitored (int inputChannel, bool monitored) noexcept
{
    if (juce::isPositiveAndBelow (inputChannel, maxChannels))
        channels[(size_t) inputChannel].monitored.store (monitored, std::memory_order_relaxed);
}

void ArtifactDetector::recaptureBaseline() noexcept
{
    for (int ch = 0; ch < maxChannels; ++ch)
        channels[(size_t) ch].baselineRecaptureRequested.store (true, std::memory_order_relaxed);
}

//==============================================================================
void ArtifactDetector::prepare (double newSampleRate, int maximumBlockSize, int numInputChannels, int numOutputChannels)
{
    const juce::ScopedLock lock (serviceLock);

    sampleRate = newSampleRate;
    maxBlock   = juce::jmax (1, maximumBlockSize);
    numIn      = juce::jlimit (0, maxChannels, numInputChannels);
    numOut     = juce::jlimit (0, maxChannels, numOutputChannels);

    decimation = juce::jmax (1, juce::roundToInt (sampleRate / 48000.0));
    fullCaptureLength  = fullCaptureSamples;
    shortCaptureLength = shortCaptureSamples;

    // The ring must hold the longest findable delay plus one block and the taps.
    const int ringSize = nextPowerOfTwo ((fullCaptureLength / 2) * decimation + maxBlock + numTaps + 64);
    ringMask = ringSize - 1;
    ringWritten = 0;

    referenceRing.resize ((size_t) numOut);

    for (auto& ring : referenceRing)
        ring.assign ((size_t) ringSize, 0.0f);

    for (int o = 0; o < maxChannels; ++o)
    {
        referenceFilter[o].prepare (sampleRate);
    }

    scratchInput.assign ((size_t) maxBlock, 0.0f);
    scratchReference.assign ((size_t) (maxBlock + numTaps), 0.0f);

    for (int ch = 0; ch < maxChannels; ++ch)
    {
        auto& c = channels[(size_t) ch];

        c.inputFilter.prepare (sampleRate);

        // The path may be different now: alignment and model start over.
        // Conditions that are active stay active (one fault, one pair).
        resetModel (c);

        if (ch < numIn)
        {
            c.captureReference.assign ((size_t) fullCaptureLength, 0.0f);
            c.captureInput.assign ((size_t) fullCaptureLength, 0.0f);
        }
        else
        {
            c.captureReference.clear();
            c.captureInput.clear();
        }

    }

    fullOrder  = juce::roundToInt (std::log2 ((double) (2 * fullCaptureLength)));
    shortOrder = juce::roundToInt (std::log2 ((double) (2 * shortCaptureLength)));
    fftFull  = std::make_unique<juce::dsp::FFT> (fullOrder);
    fftShort = std::make_unique<juce::dsp::FFT> (shortOrder);
    fftA.assign ((size_t) (4 * fullCaptureLength), 0.0f);
    fftB.assign ((size_t) (4 * fullCaptureLength), 0.0f);
}

void ArtifactDetector::resetModel (Channel& c) noexcept
{
    c.locked = c.lockVerified = c.everLocked = false;
    c.delay = 0;
    std::fill (std::begin (c.weights), std::end (c.weights), 0.0f);
    c.integral = c.integralPower = 0.0;
    c.integralWeight = 0.0f;
    c.lockedAtSeconds = 0.0;
    c.stableDelay = 0.0;
    c.delayFrozen = c.slipCheckPending = c.smallSlipPending = false;
    c.unlockedSinceSeconds = c.highResidualSinceSeconds = -1.0;
    c.fastResidualPower = c.fastPredictedPower = c.fastInputPower = 0.0;
    c.midResidualPower = c.midPredictedPower = 0.0;
    c.sampleInputPower = c.rawInputPower = c.referencePower = 0.0;
    c.residualSpread = c.previousError = 0.0;
    c.peakResidualPower = c.offPeakResidualPower = 0.0;
    c.runActive = c.runWaitForQuiet = c.burstPending = c.pendingClickValid = false;
    c.absentCooldownSamples = c.linearHoldSamples = 0;
    c.clickRate = 0.0;
    c.lastClickSeconds = -1.0e9;
    c.ratioClearSinceSeconds = -1.0;
    c.clickWindowStartSeconds = 0.0;
    c.clickEventsThisWindow = 0;
    // A fault being tracked keeps the baseline it is judged against; learning
    // a new one with the fault present would "end" it while it is still there.
    if (c.noise.phase != Condition::Phase::active)
    {
        c.baselineValid = false;
        c.baselineSegment = 0;
        c.baselineSum = 0.0;
        c.baselineBlocks = 0;
    }

    c.intervalResidual = c.intervalPredicted = 0.0;
    c.intervalClicks = 0;
    c.lastAcquisitionWasShort = false;

    // A capture the worker is correlating right now is its to finish; its
    // result is thrown away when it arrives. Anything else is simply dropped.
    if (c.acqState.load (std::memory_order_acquire) == (int) AcqState::captured)
        c.discardAcquisition = true;
    else
        c.acqState.store ((int) AcqState::idle, std::memory_order_release);

    c.lockedAtomic.store (false, std::memory_order_relaxed);
    c.alignedAtomic.store (false, std::memory_order_relaxed);

    if (c.noise.phase == Condition::Phase::pending)
        c.noise = {};
}

void ArtifactDetector::resetSessionState() noexcept
{
    const juce::ScopedLock lock (serviceLock);

    // The session clock is about to restart at zero, so everything that
    // remembers a time has to go, not only the conditions.
    for (int ch = 0; ch < maxChannels; ++ch)
    {
        auto& c = channels[(size_t) ch];
        c.noise = {};
        c.mismatch = {};
        resetModel (c);
        c.smallClicksSinceReport = 0;
        c.smallClickLargestPeak = c.smallClickLargestDuration = 0.0;
        c.lastSmallClickReportSeconds = -1.0e9;
    }

    rateWindowStartSeconds = 0.0;
    eventsThisWindow = 0;
}

//==============================================================================
Event ArtifactDetector::makeEvent (EventType type, Severity severity, int ch, double seconds, int32_t code) const noexcept
{
    Event e;
    e.type           = type;
    e.severity       = severity;
    e.channel        = ChannelId::Both;
    e.channelIndex   = ch;
    e.channelIsInput = true;
    e.elapsedSeconds = seconds;
    e.code           = code;
    return e;
}

bool ArtifactDetector::pushEvent (const Event& e) noexcept
{
    // End events always go out: a start with no end is worse than a busy second.
    // So do timing slips: there is at most one per loss per channel, and when
    // every channel slips at once (a clock event) each one has to be on record.
    if (e.code != eventCodeEnd && e.type != EventType::TimingSlip && eventsThisWindow >= maxEventsPerSecond)
        return false;

    if (! eventQueue.push (e))
        return false;

    ++eventsThisWindow;
    return true;
}

double ArtifactDetector::effectiveDelay (const Channel& c) const noexcept
{
    // Energy centroid of the model's main lobe. (Over the whole window it would
    // shift with how the band-limited response's tails happen to be cut off.)
    int peak = centreTap;
    float peakMagnitude = 0.0f;

    for (int j = 0; j < numTaps; ++j)
    {
        if (std::abs (c.weights[j]) > peakMagnitude)
        {
            peakMagnitude = std::abs (c.weights[j]);
            peak = j;
        }
    }

    double energy = 0.0, moment = 0.0;

    for (int j = juce::jmax (0, peak - 3); j <= juce::jmin (numTaps - 1, peak + 3); ++j)
    {
        const double w2 = (double) c.weights[j] * (double) c.weights[j];
        energy += w2;
        moment += w2 * (double) j;
    }

    const double centroid = energy > tinyPower ? moment / energy : (double) centreTap;
    return (double) c.delay + (double) (numTaps - 1) - centroid;
}

void ArtifactDetector::recentre (Channel& c) noexcept
{
    const double offset = effectiveDelay (c) - ((double) c.delay + (double) centreDelayOffset);
    int shift = juce::roundToInt (offset);

    if (std::abs (shift) < 4)
        return;

    shift = juce::jlimit (-c.delay, numTaps - 1, shift);

    if (shift == 0)
        return;

    // Same response at the new delay: the tap for a given absolute delay moves
    // by `shift` places in the window.
    float moved[numTaps] {};

    for (int j = 0; j < numTaps; ++j)
    {
        const int from = j - shift;
        moved[j] = juce::isPositiveAndBelow (from, numTaps) ? c.weights[from] : 0.0f;
    }

    std::copy (std::begin (moved), std::end (moved), std::begin (c.weights));
    c.delay += shift;
}

void ArtifactDetector::readFilteredReference (int referenceChannel, int delaySamples, int numSamples, float* dest) const noexcept
{
    if (! juce::isPositiveAndBelow (referenceChannel, (int) referenceRing.size()))
    {
        std::fill (dest, dest + numSamples, 0.0f);
        return;
    }

    const float* ring = referenceRing[(size_t) referenceChannel].data();
    const int64_t first = ringWritten - (int64_t) delaySamples - (int64_t) numSamples;

    for (int i = 0; i < numSamples; ++i)
    {
        const int64_t pos = first + (int64_t) i;
        dest[i] = pos >= 0 ? ring[(size_t) (pos & (int64_t) ringMask)] : 0.0f;
    }
}

//==============================================================================
void ArtifactDetector::requestAcquisition (Channel& c, bool shortSearch) noexcept
{
    c.lastAcquisitionWasShort = shortSearch;
    c.captureTarget = shortSearch ? shortCaptureLength : fullCaptureLength;
    c.captureCount  = 0;
    c.decimPhase    = 0;
    c.decimRefAcc = c.decimInAcc = 0.0f;

    // A short search looks either side of where the delay used to be, so the
    // reference is captured that much late and the search stays one-sided.
    c.captureDelayBase = shortSearch
        ? juce::jmax (0, juce::roundToInt (c.stableDelay) - (shortCaptureLength / 4) * decimation)
        : 0;

    c.acqState.store ((int) AcqState::capturing, std::memory_order_release);
}

void ArtifactDetector::feedCapture (Channel& c, const float* filteredInput, int referenceChannel, int numSamples) noexcept
{
    readFilteredReference (referenceChannel, c.captureDelayBase, numSamples, scratchReference.data());

    const float scale = 1.0f / (float) decimation;

    for (int i = 0; i < numSamples && c.captureCount < c.captureTarget; ++i)
    {
        c.decimRefAcc += scratchReference[(size_t) i];
        c.decimInAcc  += filteredInput[i];

        if (++c.decimPhase >= decimation)
        {
            c.captureReference[(size_t) c.captureCount] = c.decimRefAcc * scale;
            c.captureInput[(size_t) c.captureCount]     = c.decimInAcc * scale;
            ++c.captureCount;
            c.decimPhase = 0;
            c.decimRefAcc = c.decimInAcc = 0.0f;
        }
    }

    if (c.captureCount >= c.captureTarget)
        c.acqState.store ((int) AcqState::captured, std::memory_order_release);
}

bool ArtifactDetector::serviceAcquisitions()
{
    const juce::ScopedLock lock (serviceLock);
    bool didWork = false;

    for (int ch = 0; ch < numIn; ++ch)
    {
        auto& c = channels[(size_t) ch];

        if (c.acqState.load (std::memory_order_acquire) != (int) AcqState::captured)
            continue;

        didWork = true;

        const int length = c.captureTarget;
        const bool isShort = length == shortCaptureLength;
        auto& fft = isShort ? *fftShort : *fftFull;
        const int size = fft.getSize();

        std::fill (fftA.begin(), fftA.begin() + 2 * size, 0.0f);
        std::fill (fftB.begin(), fftB.begin() + 2 * size, 0.0f);
        std::copy (c.captureReference.begin(), c.captureReference.begin() + length, fftA.begin());
        std::copy (c.captureInput.begin(), c.captureInput.begin() + length, fftB.begin());

        fft.performRealOnlyForwardTransform (fftA.data());
        fft.performRealOnlyForwardTransform (fftB.data());

        // GCC-PHAT: input x conj(reference), every bin normalised to unit
        // magnitude so the peak is sharp whatever the path's EQ. Bins with next
        // to no energy would only add noise, so they are left out.
        double maxMagnitude = 0.0;

        for (int k = 0; k < size; ++k)
        {
            const double ar = fftA[(size_t) (2 * k)], ai = fftA[(size_t) (2 * k + 1)];
            const double br = fftB[(size_t) (2 * k)], bi = fftB[(size_t) (2 * k + 1)];
            const double re = br * ar + bi * ai;
            const double im = bi * ar - br * ai;
            fftB[(size_t) (2 * k)]     = (float) re;
            fftB[(size_t) (2 * k + 1)] = (float) im;
            maxMagnitude = juce::jmax (maxMagnitude, std::sqrt (re * re + im * im));
        }

        const double magnitudeFloor = maxMagnitude * 1.0e-4;

        for (int k = 0; k < size; ++k)
        {
            const double re = fftB[(size_t) (2 * k)], im = fftB[(size_t) (2 * k + 1)];
            const double magnitude = std::sqrt (re * re + im * im);

            if (magnitude > magnitudeFloor && magnitude > 0.0)
            {
                fftB[(size_t) (2 * k)]     = (float) (re / magnitude);
                fftB[(size_t) (2 * k + 1)] = (float) (im / magnitude);
            }
            else
            {
                fftB[(size_t) (2 * k)] = fftB[(size_t) (2 * k + 1)] = 0.0f;
            }
        }

        fft.performRealOnlyInverseTransform (fftB.data());

        const int maxLag = length / 2;
        int bestLag = 0;
        double best = 0.0, sumAbs = 0.0;

        for (int lag = 0; lag <= maxLag; ++lag)
        {
            // Magnitude, not value: an inverted path correlates negatively, and
            // taking only positive peaks would lock it onto a sidelobe.
            const double v = std::abs ((double) fftB[(size_t) lag]);
            sumAbs += v;

            if (v > best)
            {
                best = v;
                bestLag = lag;
            }
        }

        const double meanAbs = sumAbs / (double) (maxLag + 1);

        c.resultLag = bestLag;
        c.resultConfidence = meanAbs > 0.0 ? (float) (best / meanAbs) : 0.0f;
        c.acqState.store ((int) AcqState::done, std::memory_order_release);
    }

    return didWork;
}

void ArtifactDetector::adoptAcquisitionResult (int, Channel& c, double now) noexcept
{
    c.acqState.store ((int) AcqState::idle, std::memory_order_release);

    if (c.resultConfidence < acquisitionMinPeakToMean)
        return;   // not found; the caller requests the next search (full, if this one was short)

    const int foundDelay = c.captureDelayBase + c.resultLag * decimation + (decimation - 1) / 2;

    c.delay = juce::jmax (0, foundDelay - centreDelayOffset);
    std::fill (std::begin (c.weights), std::end (c.weights), 0.0f);
    c.integral = c.integralPower = 0.0;
    c.integralWeight = 0.0f;
    c.locked = true;
    c.lockedAtSeconds = now;
    c.lockVerified = false;
    c.fastResidualPower = c.fastPredictedPower = c.fastInputPower = 0.0;
    c.midResidualPower = c.midPredictedPower = 0.0;
    c.residualSpread = 0.0;
    c.highResidualSinceSeconds = -1.0;
    c.delayFrozen = false;
    c.lockedAtomic.store (true, std::memory_order_relaxed);

    // unlockedSinceSeconds is NOT cleared here: a lock only counts once it has
    // been verified. A return that still correlates but cannot be modelled
    // (noise as loud as the signal, say) locks and fails verification over and
    // over, and must not restart the mismatch timer each time.
}

void ArtifactDetector::retire (int ch, Channel& c, double now) noexcept
{
    if (c.retired)
        return;

    if (c.noise.phase == Condition::Phase::active)
    {
        auto e = makeEvent (c.noiseType, Severity::Warning, ch, now, eventCodeEnd);
        e.measuredValue = now - c.noise.conditionStartSeconds;
        e.difference    = c.noise.worstValue;

        if (! pushEvent (e))
            return;                         // queue full: try again next block
    }

    c.noise = {};

    if (c.mismatch.phase == Condition::Phase::active)
    {
        auto e = makeEvent (EventType::SignalMismatch, Severity::Critical, ch, now, eventCodeEnd);
        e.measuredValue = now - c.mismatch.conditionStartSeconds;

        if (! pushEvent (e))
            return;
    }

    c.mismatch = {};
    resetModel (c);
    c.inputFilter.reset();
    c.retired = true;
}

void ArtifactDetector::dropLock (int, Channel& c, double now, bool reportable) noexcept
{
    c.locked = false;
    c.lockedAtomic.store (false, std::memory_order_relaxed);
    c.alignedAtomic.store (false, std::memory_order_relaxed);
    c.highResidualSinceSeconds = -1.0;

    if (c.lockVerified || c.unlockedSinceSeconds < 0.0)
        c.unlockedSinceSeconds = now;      // a good lock was lost now; otherwise keep counting from the first loss

    c.lockVerified = false;

    if (c.noise.phase == Condition::Phase::pending)
        c.noise = {};

    c.runActive = false;
    c.burstPending = false;
    c.pendingClickValid = false;     // the leading edge of this loss, not a click
    c.smallSlipPending = false;

    if (reportable && c.everLocked && ! c.slipCheckPending)
    {
        // What this loss was (samples slipped, or a burst of something else)
        // is only known once the return has been found again.
        c.slipCheckPending = true;
        c.lossSeconds = now;
    }
}

//==============================================================================
void ArtifactDetector::processBlock (const float* const* input, int numInputChannels,
                                     const float* const* reference, int numReferenceChannels,
                                     int numSamples, int32_t statusBits, double blockStartSeconds) noexcept
{
    if (sampleRate <= 0.0 || numSamples <= 0 || scratchInput.empty())
        return;

    const int n = juce::jmin (numSamples, maxBlock);

    // 1. High-passed reference into the rings (always, so alignment data is
    //    continuous even while detection is suspended).
    for (int o = 0; o < numOut; ++o)
    {
        const float* src = (reference != nullptr && o < numReferenceChannels) ? reference[o] : nullptr;
        float* ring = referenceRing[(size_t) o].data();
        auto& filter = referenceFilter[o];

        for (int i = 0; i < n; ++i)
        {
            const float sample = src != nullptr && std::isfinite (src[i]) ? src[i] : 0.0f;
            ring[(size_t) ((ringWritten + i) & (int64_t) ringMask)] = filter.process (sample);
        }
    }

    ringWritten += n;

    const Settings settings = loadSettings();

    if (! settings.enabled || numOut <= 0)
    {
        // Switched off: close anything open rather than leave a start with no end.
        for (int ch = 0; ch < maxChannels; ++ch)
            retire (ch, channels[(size_t) ch], blockStartSeconds);

        return;
    }

    if (blockStartSeconds - rateWindowStartSeconds >= 1.0 || blockStartSeconds < rateWindowStartSeconds)
    {
        rateWindowStartSeconds = blockStartSeconds;
        eventsThisWindow = 0;
    }

    const bool suppressed = (statusBits & suppressingBits) != 0;
    const int channelsToProcess = juce::jmin (numInputChannels, numIn);

    for (int ch = 0; ch < channelsToProcess; ++ch)
    {
        auto& c = channels[(size_t) ch];

        if (! c.monitored.load (std::memory_order_relaxed))
        {
            // Unmonitoring closes whatever this channel had open and forgets
            // its alignment: when monitoring resumes the filters and the model
            // would be stale, and the first block would read as a click.
            retire (ch, c, blockStartSeconds);
            continue;
        }

        c.retired = false;
        processChannel (ch, input != nullptr ? input[ch] : nullptr, n, suppressed, blockStartSeconds, settings);
    }

    // Inputs that no longer exist (the device now has fewer) still get their Ends.
    for (int ch = channelsToProcess; ch < maxChannels; ++ch)
        retire (ch, channels[(size_t) ch], blockStartSeconds);
}

//==============================================================================
void ArtifactDetector::processChannel (int ch, const float* input, int n, bool suppressed,
                                       double blockStart, const Settings& s) noexcept
{
    auto& c = channels[(size_t) ch];

    const int refCh = juce::jlimit (0, numOut - 1, c.referenceOutput.load (std::memory_order_relaxed));

    if (refCh != c.activeReference)
    {
        // Compared against a different output from now on: the old alignment
        // and model describe a signal this input is no longer expected to be.
        c.activeReference = refCh;
        resetModel (c);
    }

    const double blockSeconds = (double) n / sampleRate;
    const double blockEnd = blockStart + blockSeconds;

    // High-pass the return (always, to keep the filter state continuous).
    float* x = scratchInput.data();
    double rawSum = 0.0, inSum = 0.0;

    for (int i = 0; i < n; ++i)
    {
        float raw = input != nullptr ? input[i] : 0.0f;

        if (! std::isfinite (raw))
            raw = 0.0f;     // a virtual device can send NaN; one would poison the filter state for good

        rawSum += (double) raw * (double) raw;
        x[i] = c.inputFilter.process (raw);
        inSum += (double) x[i] * (double) x[i];
    }

    const double inputPower = inSum / (double) n;
    c.rawInputPower += smoothing (blockSeconds, 0.3) * (rawSum / (double) n - c.rawInputPower);

    if (c.acqState.load (std::memory_order_acquire) == (int) AcqState::capturing)
    {
        if (suppressed)
            c.acqState.store ((int) AcqState::idle, std::memory_order_release);   // start over on clean signal
        else
            feedCapture (c, x, refCh, n);
    }

    if (c.baselineRecaptureRequested.exchange (false, std::memory_order_relaxed))
    {
        c.baselineValid = false;
        c.baselineSegment = 0;
        c.baselineStartSeconds = blockStart;
        c.baselineSum = 0.0;
        c.baselineBlocks = 0;
    }

    if (suppressed)
    {
        // Muted, fading or settling: nothing to compare against. Freeze the
        // model, forget anything unconfirmed, and restart the timers after.
        if (c.noise.phase == Condition::Phase::pending)
            c.noise = {};

        c.highResidualSinceSeconds = -1.0;
        c.unlockedSinceSeconds = -1.0;
        c.delayFrozen = false;
        c.runActive = c.burstPending = false;
        c.absentCooldownSamples = (int) (0.05 * sampleRate);   // nor is the moment detection resumes a click
        return;
    }

    //--------------------------------------------------------------------------
    // Not aligned: find the return.
    if (! c.locked)
    {
        const int state = c.acqState.load (std::memory_order_acquire);

        if (state == (int) AcqState::done && c.discardAcquisition)
        {
            c.discardAcquisition = false;       // captured before a reset: not for this model
            c.acqState.store ((int) AcqState::idle, std::memory_order_release);
        }
        else if (state == (int) AcqState::done)
            adoptAcquisitionResult (ch, c, blockStart);
        else if (state == (int) AcqState::idle)
            requestAcquisition (c, c.everLocked && ! c.lastAcquisitionWasShort && c.stableDelay > 0.0);

        if (! c.locked)
        {
            // Present but unmatched for long enough is a fault in its own right.
            // (Absent is the dropout detector's business, not ours.)
            const bool present = inputPower > presentPowerFloor;

            if (! present)
            {
                c.unlockedSinceSeconds = -1.0;
            }
            else
            {
                if (c.unlockedSinceSeconds < 0.0)
                    c.unlockedSinceSeconds = blockStart;

                if (c.mismatch.phase != Condition::Phase::active
                    && blockStart - c.unlockedSinceSeconds >= mismatchAfterSeconds)
                {
                    auto e = makeEvent (EventType::SignalMismatch, Severity::Critical, ch, c.unlockedSinceSeconds, eventCodeStart);
                    e.measuredValue = amplitudeDb (std::sqrt (inputPower));

                    if (pushEvent (e))
                    {
                        c.mismatch.phase = Condition::Phase::active;
                        c.mismatch.conditionStartSeconds = c.unlockedSinceSeconds;
                        c.slipCheckPending = false;   // reported as a mismatch instead
                    }
                }
            }
        }

        return;
    }

    //--------------------------------------------------------------------------
    // Aligned: predict the return from the reference and look at what is left.
    const int windowLength = n + numTaps - 1;
    float* r = scratchReference.data();
    readFilteredReference (refCh, c.delay, windowLength, r);

    const double sinceLock = blockStart - c.lockedAtSeconds;
    const bool converged = sinceLock >= convergeSeconds;
    const bool armed = converged && c.baselineValid;

    const double predictedRms = std::sqrt (juce::jmax (0.0, c.midPredictedPower));
    // Three floors, the highest wins: far outside the residual's own
    // sample-to-sample spread; a set distance below the signal; and well above
    // the residual's LEVEL. The last matters on a path the model cannot fully
    // follow (a steep high-pass, a resonant EQ, hum already present): there the
    // residual is large and smooth, and its ordinary swings would otherwise
    // clear a threshold set only by how little it changes sample to sample.
    const double clickThreshold = juce::jmax ((double) s.clickSigma * 1.2533 * c.residualSpread,
                                              predictedRms * std::pow (10.0, (double) s.clickMinRelDb / 20.0),
                                              clickResidualFactor * std::sqrt (juce::jmax (0.0, c.midResidualPower)));

    const double spreadStep = 1.0 - std::exp (-1.0 / (0.1 * sampleRate));
    const double peakGate = 2.25 * c.rawInputPower;                 // |raw| above 1.5 sigma
    const int    mergeSamples = juce::jmax (1, (int) (clickMergeSeconds * sampleRate));
    const double absentGate = 0.01 * c.midPredictedPower;           // return 20 dB under prediction

    double windowEnergy = 0.0;

    for (int j = 0; j < numTaps; ++j)
        windowEnergy += (double) r[j] * (double) r[j];

    // The step is normalised by the reference energy in the window PLUS its
    // long-run average. Without the second term, a window that happens to be
    // nearly empty (it holds only a handful of independent samples at high
    // rates) gives a huge step, the model lurches, and the residual spikes —
    // which reads exactly like a click.
    double referencePower = c.referencePower;
    const double referencePowerStep = 1.0 - std::exp (-1.0 / (0.05 * sampleRate));

    double sumE = 0.0, sumY = 0.0;
    int    sumN = 0;
    double peakE = 0.0, offE = 0.0;
    int peakN = 0, offN = 0;

    const int    maxClickSamples = juce::jmax (2, (int) (clickMaxSeconds * sampleRate));
    const int    cooldownSamples = (int) (0.03 * sampleRate);
    const int    linearHoldLength = (int) (0.05 * sampleRate);
    double fastIn = c.sampleInputPower;
    const double fastInStep = 1.0 - std::exp (-1.0 / (0.001 * sampleRate));
    float* w = c.weights;

    const double integralLeak = 1.0 - juce::MathConstants<double>::twoPi * 30.0 / sampleRate;
    const double integralPowerStep = 1.0 - std::exp (-1.0 / (1.0 * sampleRate));

    // The integral is scaled to the size of one ordinary tap's input and then
    // adapted as one more tap of the same normalised filter, so no single
    // update can overshoot however the integral happens to swing.
    const double integralScale = c.integralPower > tinyPower ? std::sqrt (c.referencePower / c.integralPower) : 0.0;
    const bool integralReady = sinceLock >= 0.25 && integralScale > 0.0;
    const float integralRate = sinceLock < 1.25 ? 1.0f : 1.0f / 64.0f;
    double integral = c.integral, integralPower = c.integralPower;
    float  integralWeight = c.integralWeight;

    for (int i = 0; i < n; ++i)
    {
        const float* window = r + i;

        float predicted = dotTaps (w, window);

        referencePower += referencePowerStep * ((double) window[numTaps - 1] * (double) window[numTaps - 1] - referencePower);
        integral = integralLeak * integral + (double) window[centreTap];
        integralPower += integralPowerStep * (integral * integral - integralPower);
        const float scaledIntegral = (float) (integral * integralScale);
        predicted += integralWeight * scaledIntegral;

        const float error = x[i] - predicted;
        const double e2 = (double) error * (double) error;
        const double absError = std::abs ((double) error);

        fastIn += fastInStep * ((double) x[i] * (double) x[i] - fastIn);

        const bool returnAbsent = converged && fastIn < absentGate;

        if (returnAbsent)
        {
            // A dropout edge is not a click, and neither is the recovery.
            c.absentCooldownSamples = cooldownSamples;
            c.runActive = false;
        }
        else if (c.absentCooldownSamples > 0)
        {
            --c.absentCooldownSamples;
        }

        if (c.linearHoldSamples > 0)
            --c.linearHoldSamples;

        // A click is judged on the sample-to-sample CHANGE in the residual, not
        // its size. What the model cannot quite follow at the bottom of the band
        // shows up as slow, smooth swells that are large but not impulsive; a
        // click is the opposite. (The spread and threshold are in the same terms.)
        const double step = std::abs ((double) error - c.previousError);
        c.previousError = (double) error;

        const bool outlier = armed && step > clickThreshold
                          && c.absentCooldownSamples == 0 && c.linearHoldSamples == 0;

        if (c.runActive)
        {
            ++c.runSamples;
            c.runErrorPredicted += (double) error * (double) predicted;
            c.runErrorPower     += e2;
            c.runPredictedPower += (double) predicted * (double) predicted;

            // Would the same model fit if the return were a few samples earlier
            // or later? (Only while a run is open, so the cost is bounded.)
            for (int k = 0; k < numShiftTrials; ++k)
            {
                const int shift = k < maxTrialShift ? k - maxTrialShift : k - maxTrialShift + 1;
                float shifted = integralWeight * scaledIntegral;

                for (int j = juce::jmax (0, shift); j < juce::jmin (numTaps, numTaps + shift); ++j)
                    shifted += w[j - shift] * window[j];

                const double trialError = (double) x[i] - (double) shifted;
                c.runShiftErrorPower[k] += trialError * trialError;
            }

            if (outlier)
            {
                c.runPeak = juce::jmax (c.runPeak, absError);
                c.runSinceOutlier = 0;
            }
            else
            {
                ++c.runSinceOutlier;
            }

            if (c.runSinceOutlier > mergeSamples)
            {
                // It stopped: a click.
                c.runActive = false;
                reportClick (ch, c, c.runStartSeconds, c.runPeak,
                             (double) (c.runSamples - c.runSinceOutlier) / sampleRate, clickThreshold);
            }
            else if (c.runSamples >= maxClickSamples)
            {
                // Too long to be a click. If the error is simply the signal
                // scaled, the path's gain (or polarity) changed: let the model
                // re-learn it and say nothing — that is the level detector's
                // event. Otherwise something longer has started; what, is
                // decided at block level.
                c.runActive = false;
                c.runWaitForQuiet = true;
                c.quietSamples = 0;

                const double denominator = std::sqrt (c.runErrorPower * c.runPredictedPower);
                const double coherence = denominator > tinyPower ? std::abs (c.runErrorPredicted) / denominator : 0.0;

                int bestTrial = -1;

                for (int k = 0; k < numShiftTrials; ++k)
                    if (c.runShiftErrorPower[k] < 0.05 * c.runErrorPower
                        && (bestTrial < 0 || c.runShiftErrorPower[k] < c.runShiftErrorPower[bestTrial]))
                        bestTrial = k;

                if (bestTrial >= 0)
                {
                    // The return slipped by a few samples: move the model to
                    // match (same response, new delay) and report the slip.
                    const int shift = bestTrial < maxTrialShift ? bestTrial - maxTrialShift : bestTrial - maxTrialShift + 1;
                    float moved[numTaps] {};

                    for (int j = 0; j < numTaps; ++j)
                        moved[j] = juce::isPositiveAndBelow (j - shift, numTaps) ? w[j - shift] : 0.0f;

                    std::copy (std::begin (moved), std::end (moved), w);
                    c.smallSlipPending = true;
                    c.smallSlipSeconds = c.runStartSeconds;
                    c.pendingClickValid = false;
                    c.linearHoldSamples = linearHoldLength;
                }
                else if (coherence >= 0.95)
                {
                    // error = k * prediction, so the return is (1 + k) times
                    // what the model says: scale the model, no re-learning
                    // needed. (A scale near zero is a dropout: leave the model
                    // alone so it still fits when the return comes back.)
                    const double scale = 1.0 + c.runErrorPredicted / juce::jmax (tinyPower, c.runPredictedPower);

                    if (std::abs (scale) > 0.1 && std::abs (scale) < 10.0)
                    {
                        for (int j = 0; j < numTaps; ++j)
                            w[j] *= (float) scale;

                        integralWeight *= (float) scale;
                    }

                    c.linearHoldSamples = linearHoldLength;
                }
                else if (! c.burstPending)
                {
                    c.burstPending = true;
                    c.burstStartSeconds = c.runStartSeconds;
                    c.burstPeak = c.runPeak;
                }
            }
        }
        else if (c.runWaitForQuiet)
        {
            c.quietSamples = outlier ? 0 : c.quietSamples + 1;

            if (c.quietSamples > mergeSamples)
                c.runWaitForQuiet = false;
        }
        else if (outlier)
        {
            c.runActive = true;
            c.runSamples = 1;
            c.runSinceOutlier = 0;
            c.runStartSeconds = blockStart + (double) i / sampleRate;
            c.runPeak = absError;
            c.runErrorPredicted = (double) error * (double) predicted;
            c.runErrorPower = e2;
            c.runPredictedPower = (double) predicted * (double) predicted;
            std::fill (std::begin (c.runShiftErrorPower), std::end (c.runShiftErrorPower), 0.0);
        }

        if (! c.runActive)
        {
            // The background residual: everything that is not part of a
            // possible click. Clicks are reported on their own, and must not
            // also read as a rise in noise.
            sumE += e2;
            sumY += (double) predicted * (double) predicted;
            ++sumN;

            const float raw = input != nullptr ? input[i] : 0.0f;

            if ((double) raw * (double) raw > peakGate)   { peakE += e2; ++peakN; }
            else                                          { offE += e2;  ++offN; }

            // Outliers are kept out of the spread too, or one click would raise
            // the bar for the next; and so is a dropout or a gain change.
            if (! outlier && ! returnAbsent && c.linearHoldSamples == 0)
                c.residualSpread += spreadStep * (juce::jmin (step, 4.0 * c.residualSpread + 1.0e-12) - c.residualSpread);
        }

        if (! converged)
            c.residualSpread += spreadStep * 10.0 * (step - c.residualSpread);

        // Adapt — except during a possible click (which says nothing about the
        // path) and while the return is absent (the model must survive a
        // dropout to recognise the recovery).
        if (! c.runActive && ! returnAbsent)
        {
            const float gain = adaptStep * error
                                 / (float) (windowEnergy + (double) scaledIntegral * (double) scaledIntegral
                                              + (double) numTaps * referencePower + 1.0e-12);

            for (int j = 0; j < numTaps; ++j)
                w[j] += gain * window[j];

            // (Not until the integral's scale has settled.) AC coupling is a
            // fixed property of the path, so after the first second this
            // coefficient moves very slowly: any jitter in it is multiplied by
            // the signal's largest low-frequency swings and comes out as slow
            // bumps in the residual that pass for clicks.
            if (integralReady)
                integralWeight += gain * scaledIntegral * integralRate;
        }

        if (i + 1 < n)
            windowEnergy += (double) window[numTaps] * (double) window[numTaps]
                          - (double) window[0] * (double) window[0];

        if (windowEnergy < 0.0)
            windowEnergy = 0.0;
    }

    c.sampleInputPower = fastIn;
    c.referencePower = referencePower;
    c.integral = integral;
    c.integralPower = integralPower;
    c.integralWeight = integralWeight;

    //--------------------------------------------------------------------------
    // Block statistics.
    const double blockE = sumE / (double) juce::jmax (1, sumN);
    const double blockY = sumY / (double) juce::jmax (1, sumN);

    if (! std::isfinite (blockE) || ! std::isfinite (blockY))
    {
        // Should be impossible; if the model ever does diverge, start it over
        // rather than report nonsense for the rest of the run.
        std::fill (std::begin (c.weights), std::end (c.weights), 0.0f);
        c.integral = c.integralPower = 0.0;
        c.integralWeight = 0.0f;
        dropLock (ch, c, blockEnd, false);
        c.lastAcquisitionWasShort = true;
        return;
    }

    const bool absent = converged && inputPower < 0.01 * (sumY / (double) n + tinyPower) && sumN > 0;
    const bool relearning = c.linearHoldSamples > 0;

    if (! absent && sumN >= n / 4)
    {
        const double fast = smoothing (blockSeconds, 0.015);
        const double mid  = smoothing (blockSeconds, 0.025);

        c.fastResidualPower  += fast * (blockE - c.fastResidualPower);
        c.fastPredictedPower += fast * (blockY - c.fastPredictedPower);
        c.fastInputPower     += fast * (inputPower - c.fastInputPower);
        c.midResidualPower   += mid * (blockE - c.midResidualPower);
        c.midPredictedPower  += mid * (blockY - c.midPredictedPower);

        const double peakStep = smoothing (blockSeconds, 0.1);

        if (peakN > 0)  c.peakResidualPower    += peakStep * (peakE / (double) peakN - c.peakResidualPower);
        if (offN > 0)   c.offPeakResidualPower += peakStep * (offE / (double) offN - c.offPeakResidualPower);
    }

    const float fastRatioDb = powerRatioDb (c.fastResidualPower, c.fastPredictedPower);
    const float midRatioDb  = powerRatioDb (c.midResidualPower, c.midPredictedPower);

    c.residualDbAtomic.store (midRatioDb, std::memory_order_relaxed);
    c.latencyMsAtomic.store ((float) (effectiveDelay (c) * 1000.0 / sampleRate), std::memory_order_relaxed);

    if (converged && ! absent)
    {
        c.intervalResidual  += sumE;
        c.intervalPredicted += sumY;
    }

    if (absent)
    {
        // A dropout: hold everything as it was until the return comes back.
        c.highResidualSinceSeconds = -1.0;
        c.delayFrozen = false;

        if (c.noise.phase == Condition::Phase::pending)
            c.noise = {};

        c.burstPending = false;
        return;
    }

    //--------------------------------------------------------------------------
    // Did a lock that looked good turn out wrong, or has the return stopped
    // matching? (Residual about as large as the prediction = uncorrelated.)
    if (! c.lockVerified && sinceLock >= verifyLockSeconds)
    {
        if (midRatioDb > verifyLockRatioDb)
        {
            dropLock (ch, c, blockEnd, false);
            c.lastAcquisitionWasShort = true;    // so the next search is a full one
            return;
        }

        c.lockVerified = true;
        c.alignedAtomic.store (true, std::memory_order_relaxed);
        c.unlockedSinceSeconds = -1.0;

        if (! c.everLocked)
        {
            c.everLocked = true;
            c.stableDelay = effectiveDelay (c);
        }

        if (! c.baselineValid)
        {
            c.baselineSegment = 0;
            c.baselineStartSeconds = blockEnd;
            c.baselineSum = 0.0;
            c.baselineBlocks = 0;
        }
    }

    if (c.lockVerified && c.mismatch.phase == Condition::Phase::active)
    {
        auto e = makeEvent (EventType::SignalMismatch, Severity::Critical, ch, blockEnd, eventCodeEnd);
        e.measuredValue = blockEnd - c.mismatch.conditionStartSeconds;

        if (pushEvent (e))
        {
            c.mismatch = {};
            c.slipCheckPending = false;     // the mismatch pair already covers that loss
        }
    }

    if (c.lockVerified && c.slipCheckPending && c.mismatch.phase != Condition::Phase::active)
    {
        // Found again after a loss: at a different delay, samples were dropped
        // or repeated; at the same one, it was a burst of something that was
        // not the signal. (Retried each block until it is in the queue.)
        const double delta = effectiveDelay (c) - c.stableDelay;
        const bool slipped = std::abs (delta) >= (double) slipMinSamples;

        auto e = makeEvent (slipped ? EventType::TimingSlip : EventType::Noise, Severity::Warning,
                            ch, c.lossSeconds, eventCodeInstant);

        if (slipped)
        {
            e.measuredValue = delta;                                // samples; + = later
            e.expectedValue = c.stableDelay;
            e.difference    = delta * 1000.0 / sampleRate;          // ms
        }
        else
        {
            e.measuredValue = 0.0;                                  // residual ~ as large as the signal
            e.expectedValue = c.baselineValid ? (double) c.baselineDb : 0.0;
            e.difference    = c.lockedAtSeconds - c.lossSeconds;    // seconds until found again (upper bound)
        }

        if (pushEvent (e))
        {
            c.slipCheckPending = false;
            c.stableDelay = effectiveDelay (c);
        }
    }

    if (converged && relearning)
    {
        c.highResidualSinceSeconds = -1.0;
    }
    else if (converged)
    {
        if (fastRatioDb > lockLossRatioDb)
        {
            if (c.highResidualSinceSeconds < 0.0)
                c.highResidualSinceSeconds = blockStart;

            if (blockEnd - c.highResidualSinceSeconds >= lockLossHoldSeconds)
            {
                dropLock (ch, c, c.highResidualSinceSeconds, c.lockVerified);
                return;
            }
        }
        else
        {
            c.highResidualSinceSeconds = -1.0;
        }
    }

    if (! c.lockVerified)
        return;

    //--------------------------------------------------------------------------
    // The residual is measured relative to the signal, so the path's own noise
    // floor sits closer when the level drops and further when it rises. A
    // baseline learned at one level is not a fair bar at another: re-learn it
    // when the predicted level has moved by 6 dB (an output-level change, or a
    // gain change in the path), unless a fault is already being tracked.
    if (c.baselineValid && c.noise.phase != Condition::Phase::active
        && (c.midPredictedPower > 4.0 * c.baselinePredictedPower || c.midPredictedPower < 0.25 * c.baselinePredictedPower))
    {
        c.baselineValid = false;
        c.baselineSegment = 0;
        c.baselineStartSeconds = blockEnd;
        c.baselineSum = 0.0;
        c.baselineBlocks = 0;
        c.noise = {};
        c.burstPending = false;
    }

    //--------------------------------------------------------------------------
    // Baseline: the lowest of three half-second means, so one burst during
    // capture cannot become "normal".
    if (! c.baselineValid)
    {
        c.baselineSum += (double) midRatioDb;
        ++c.baselineBlocks;

        if (blockEnd - c.baselineStartSeconds >= baselineSegmentSeconds && c.baselineBlocks > 0)
        {
            const float segmentMean = (float) (c.baselineSum / (double) c.baselineBlocks);
            c.baselineCandidate = c.baselineSegment == 0 ? segmentMean : juce::jmin (c.baselineCandidate, segmentMean);
            c.baselineSum = 0.0;
            c.baselineBlocks = 0;
            c.baselineStartSeconds = blockEnd;

            if (++c.baselineSegment >= 3)
            {
                c.baselineDb = c.baselineCandidate;
                c.settledRatioDb = c.baselineCandidate;
                c.baselinePredictedPower = c.midPredictedPower;
                c.baselineValid = true;
                c.stableDelay = effectiveDelay (c);
            }
        }

        return;
    }

    //--------------------------------------------------------------------------
    // A slip of a few samples that was fitted on the spot.
    if (c.smallSlipPending && ! c.runActive)
    {
        const double now = effectiveDelay (c);
        const double delta = now - c.stableDelay;
        bool reported = true;

        if (std::abs (delta) >= (double) slipMinSamples)
        {
            auto e = makeEvent (EventType::TimingSlip, Severity::Warning, ch, c.smallSlipSeconds, eventCodeInstant);
            e.measuredValue = delta;
            e.expectedValue = c.stableDelay;
            e.difference    = delta * 1000.0 / sampleRate;
            reported = pushEvent (e);
        }

        if (reported)      // otherwise the queue is full: try again next block
        {
            c.smallSlipPending = false;
            c.stableDelay = now;
            c.delayFrozen = false;
            c.burstPending = false;
        }
    }

    // A click that was held back and turned out to be only a click.
    if (c.pendingClickValid && blockEnd - c.pendingClickSeconds >= clickHoldSeconds)
        flushPendingClick (ch, c);

    c.clickRate *= std::exp (-blockSeconds / clickRateSeconds);

    //--------------------------------------------------------------------------
    // Delay steps the model absorbed without losing the return (a few samples).
    const bool residualNormal = midRatioDb < c.baselineDb + stableResidualMarginDb;

    if (residualNormal)
    {
        const double now = effectiveDelay (c);

        bool settled = true;

        if (c.delayFrozen)
        {
            const double delta = now - c.stableDelay;

            if (std::abs (delta) >= (double) slipMinSamples)
            {
                auto e = makeEvent (EventType::TimingSlip, Severity::Warning, ch, c.delayFrozenSeconds, eventCodeInstant);
                e.measuredValue = delta;
                e.expectedValue = c.stableDelay;
                e.difference    = delta * 1000.0 / sampleRate;
                settled = pushEvent (e);

                if (settled)
                    c.burstPending = false;     // that is what the burst was
            }

            if (settled)
                c.delayFrozen = false;
        }

        if (settled)       // otherwise the queue is full: keep the old delay and try again next block
            c.stableDelay = now;

        if (c.burstPending && c.noise.phase == Condition::Phase::inactive)
        {
            // Longer than a click, over before it became anything else: a
            // short burst of something that was not the signal.
            c.burstPending = false;
            reportClick (ch, c, c.burstStartSeconds, c.burstPeak, blockStart - c.burstStartSeconds, clickThreshold);
        }
    }
    else if (! c.delayFrozen)
    {
        c.delayFrozen = true;
        c.delayFrozenSeconds = blockStart;
    }
    else if (blockStart - c.delayFrozenSeconds > 2.0)
    {
        // A long fault: any step in the delay can no longer be told from
        // ordinary drift over that time, so stop holding the old value.
        c.stableDelay = effectiveDelay (c);
    }

    // Keep the model's main tap near the middle of its window as the path's
    // delay drifts (clocks that are not locked to each other).
    recentre (c);

    if (residualNormal && c.noise.phase == Condition::Phase::inactive)
    {
        // The model keeps refining for some seconds after the baseline is
        // taken. Let the baseline follow the residual DOWN (never up), so
        // sensitivity is not stuck at how good the fit was two seconds in.
        c.settledRatioDb += (float) smoothing (blockSeconds, 5.0) * (midRatioDb - c.settledRatioDb);

        if (c.settledRatioDb + 1.0f < c.baselineDb)
            c.baselineDb = c.settledRatioDb + 1.0f;
    }

    //--------------------------------------------------------------------------
    // Sustained residual rise: noise, or distortion if it rides the peaks.
    const float trigger = juce::jmax (c.baselineDb + s.noiseRiseDb, s.noiseMinRatioDb);
    const double clicksPerSecond = c.clickRate / clickRateSeconds;
    const bool  crackling = clicksPerSecond >= crackleRatePerSecond && blockStart - c.lastClickSeconds < crackleQuietSeconds;
    const bool  over    = (midRatioDb > trigger || crackling) && ! relearning;

    // Clear means the residual is back down AND the clicking has stopped; the
    // fault ended at whichever of those happened last.
    if (midRatioDb < trigger - noiseEndHysteresisDb)
    {
        if (c.ratioClearSinceSeconds < 0.0)
            c.ratioClearSinceSeconds = blockStart;
    }
    else
    {
        c.ratioClearSinceSeconds = -1.0;
    }

    const bool   clear = c.ratioClearSinceSeconds >= 0.0 && blockStart - c.lastClickSeconds >= crackleQuietSeconds;
    const double clearedAt = juce::jmax (c.ratioClearSinceSeconds, c.lastClickSeconds);
    auto& cond = c.noise;

    switch (cond.phase)
    {
        case Condition::Phase::inactive:
            if (over)
            {
                cond.phase = Condition::Phase::pending;
                cond.phaseStartSeconds = blockStart;
                cond.conditionStartSeconds = blockStart;
                cond.worstValue = midRatioDb;
            }
            break;

        case Condition::Phase::pending:
            if (! over)
            {
                cond = {};
                break;
            }

            cond.worstValue = juce::jmax (cond.worstValue, (double) midRatioDb);

            if (blockEnd - cond.phaseStartSeconds >= s.noiseHoldMs / 1000.0)
            {
                const float peakDb = powerRatioDb (c.peakResidualPower, c.offPeakResidualPower);
                c.noiseType = peakDb >= s.distortionPeakDb ? EventType::Distortion : EventType::Noise;

                auto e = makeEvent (c.noiseType, Severity::Warning, ch, cond.conditionStartSeconds, eventCodeStart);
                e.measuredValue = cond.worstValue;
                e.expectedValue = c.baselineDb;
                e.difference    = cond.worstValue - (double) c.baselineDb;
                e.thresholdUsed = trigger;

                if (pushEvent (e))
                {
                    cond.phase = Condition::Phase::active;
                    cond.clearSinceSeconds = -1.0;
                    c.burstPending = false;     // reported as this condition instead
                }
            }
            break;

        case Condition::Phase::active:
            cond.worstValue = juce::jmax (cond.worstValue, (double) midRatioDb);

            if (clear)
            {
                if (cond.clearSinceSeconds < 0.0)
                    cond.clearSinceSeconds = clearedAt;

                if (blockEnd - cond.clearSinceSeconds >= s.noiseEndHoldMs / 1000.0)
                {
                    auto e = makeEvent (c.noiseType, Severity::Warning, ch, cond.clearSinceSeconds, eventCodeEnd);
                    e.measuredValue = cond.clearSinceSeconds - cond.conditionStartSeconds;
                    e.expectedValue = c.baselineDb;
                    e.difference    = cond.worstValue;
                    e.thresholdUsed = trigger;

                    if (pushEvent (e))
                        cond = {};
                }
            }
            else
            {
                cond.clearSinceSeconds = -1.0;
            }
            break;
    }
}

void ArtifactDetector::reportClick (int ch, Channel& c, double seconds, double peak, double durationSeconds, double threshold) noexcept
{
    // Held for a moment: if the return is lost or found to have slipped right
    // after, this was the leading edge of that fault and is reported as it,
    // not as a click as well.
    if (c.pendingClickValid)
        flushPendingClick (ch, c);

    c.pendingClickValid     = true;
    c.pendingClickSeconds   = seconds;
    c.pendingClickPeak      = peak;
    c.pendingClickDuration  = durationSeconds;
    c.pendingClickThreshold = threshold;
    c.pendingClickSignalRms = std::sqrt (juce::jmax (0.0, c.midPredictedPower));
}

void ArtifactDetector::flushPendingClick (int ch, Channel& c) noexcept
{
    c.pendingClickValid = false;
    ++c.intervalClicks;
    c.clickRate += 1.0;
    c.lastClickSeconds = c.pendingClickSeconds;

    const double seconds = c.pendingClickSeconds;

    if (seconds - c.clickWindowStartSeconds >= 1.0 || seconds < c.clickWindowStartSeconds)
    {
        c.clickWindowStartSeconds = seconds;
        c.clickEventsThisWindow = 0;
    }

    // Small clicks are real, and every one is counted in the measurement row,
    // but they do not each get an event: on real hardware a path can tick
    // faintly, far below audibility, every couple of minutes all night, and
    // hundreds of identical rows would bury anything that mattered. They are
    // summarised instead: the first at once, then at most one row an hour.
    const double eventFloor = c.pendingClickSignalRms * std::pow (10.0, (double) sClickEventRelDb.load (std::memory_order_relaxed) / 20.0);

    if (c.pendingClickPeak < eventFloor)
    {
        ++c.smallClicksSinceReport;

        if (c.pendingClickPeak > c.smallClickLargestPeak)
        {
            c.smallClickLargestPeak = c.pendingClickPeak;
            c.smallClickLargestDuration = c.pendingClickDuration;
        }

        if (seconds - c.lastSmallClickReportSeconds >= smallClickReportSeconds && c.noise.phase != Condition::Phase::active)
        {
            auto e = makeEvent (EventType::Click, Severity::Info, ch, seconds, eventCodeInstant);
            e.measuredValue = amplitudeDb (c.smallClickLargestPeak);      // largest since the last report, dBFS
            e.expectedValue = (double) c.smallClicksSinceReport;          // how many since the last report
            e.difference    = c.smallClickLargestDuration * 1000.0;       // ms
            e.thresholdUsed = amplitudeDb (eventFloor);                   // what it takes to be an event, dBFS

            if (pushEvent (e))
            {
                c.lastSmallClickReportSeconds = seconds;
                c.smallClicksSinceReport = 0;
                c.smallClickLargestPeak = c.smallClickLargestDuration = 0.0;
            }
        }

        return;
    }

    // A crackle storm must not flood the log: a few events a second per
    // channel, with every click still counted in the measurement row — and
    // once it is a storm it is reported as Noise instead.
    if (c.clickEventsThisWindow >= maxClickEventsPerSecondPerChannel || c.noise.phase == Condition::Phase::active)
        return;

    auto e = makeEvent (EventType::Click, Severity::Warning, ch, seconds, eventCodeInstant);
    e.measuredValue = amplitudeDb (c.pendingClickPeak);           // peak of what was added, dBFS
    e.expectedValue = amplitudeDb (1.2533 * c.residualSpread);    // the normal residual level, dBFS
    e.difference    = c.pendingClickDuration * 1000.0;            // ms
    e.thresholdUsed = amplitudeDb (c.pendingClickThreshold);

    if (pushEvent (e))
        ++c.clickEventsThisWindow;
}

//==============================================================================
ArtifactDetector::IntervalFigures ArtifactDetector::takeIntervalFigures (int inputChannel) noexcept
{
    IntervalFigures f;

    if (! juce::isPositiveAndBelow (inputChannel, maxChannels))
        return f;

    auto& c = channels[(size_t) inputChannel];

    f.locked     = c.locked && c.lockVerified && c.intervalPredicted > 0.0;
    f.clickCount = c.intervalClicks;

    if (f.locked)
    {
        f.residualDb = powerRatioDb (c.intervalResidual, c.intervalPredicted);
        f.latencyMs  = (float) (effectiveDelay (c) * 1000.0 / sampleRate);
    }

    c.intervalResidual = c.intervalPredicted = 0.0;
    c.intervalClicks = 0;
    return f;
}

bool ArtifactDetector::isLocked (int inputChannel) const noexcept
{
    return juce::isPositiveAndBelow (inputChannel, maxChannels)
        && channels[(size_t) inputChannel].lockedAtomic.load (std::memory_order_relaxed);
}

bool ArtifactDetector::isAligned (int inputChannel) const noexcept
{
    return juce::isPositiveAndBelow (inputChannel, maxChannels)
        && channels[(size_t) inputChannel].alignedAtomic.load (std::memory_order_relaxed);
}

float ArtifactDetector::getLatencyMs (int inputChannel) const noexcept
{
    return juce::isPositiveAndBelow (inputChannel, maxChannels)
             ? channels[(size_t) inputChannel].latencyMsAtomic.load (std::memory_order_relaxed) : 0.0f;
}

float ArtifactDetector::getResidualDb (int inputChannel) const noexcept
{
    return juce::isPositiveAndBelow (inputChannel, maxChannels)
             ? channels[(size_t) inputChannel].residualDbAtomic.load (std::memory_order_relaxed) : silenceFloorDb;
}

} // namespace pnam
