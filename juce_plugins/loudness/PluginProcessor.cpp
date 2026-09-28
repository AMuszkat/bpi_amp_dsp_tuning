#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    /** Default contour sets, quietest first.

        Every set starts FLAT: all gains 0 dB, so the plugin does nothing until contours are
        dialled in. Only the thresholds (where each set takes over) and the shelf frequency /
        Q starting points are pre-set. The Fletcher-Munson shape this is for -- the ear losing
        low-frequency sensitivity as level drops, so the quietest setting wants the most bass
        lift and the reference setting stays flat -- is now the user's to draw on the pad.
    */
    struct SettingDefaults
    {
        float thresholdDb;
        float gain1, freq1, q1;
        float gain2, freq2, q2;
    };

    constexpr SettingDefaults defaultSettings[LoudnessIDs::numSettings] =
    {
        { -60.0f, 0.0f, 60.0f, 0.7f,  0.0f, 160.0f, 0.7f },   // very quiet
        { -40.0f, 0.0f, 60.0f, 0.7f,  0.0f, 160.0f, 0.7f },
        { -25.0f, 0.0f, 60.0f, 0.7f,  0.0f, 160.0f, 0.7f },
        { -12.0f, 0.0f, 60.0f, 0.7f,  0.0f, 160.0f, 0.7f },   // reference
    };
}

//==============================================================================
LoudnessProcessor::LoudnessProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                        #if ! JucePlugin_IsMidiEffect
                         #if ! JucePlugin_IsSynth
                           .withInput  ("Input",  juce::AudioChannelSet::mono(), true)
                         #endif
                           .withOutput ("Output", juce::AudioChannelSet::mono(), true)
                        #endif
                       ),
#endif
       apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    for (int s = 0; s < LoudnessIDs::numSettings; ++s)
    {
        thresholdParam[(size_t) s] = apvts.getRawParameterValue (LoudnessIDs::threshold (s));

        for (int b = 0; b < LoudnessIDs::numBands; ++b)
        {
            gainParam[(size_t) s][(size_t) b] = apvts.getRawParameterValue (LoudnessIDs::band (s, b, "gain"));
            freqParam[(size_t) s][(size_t) b] = apvts.getRawParameterValue (LoudnessIDs::band (s, b, "freq"));
            qParam[(size_t) s][(size_t) b]    = apvts.getRawParameterValue (LoudnessIDs::band (s, b, "q"));
        }
    }

    modeParam       = apvts.getRawParameterValue (LoudnessIDs::mode);
    attackParam     = apvts.getRawParameterValue (LoudnessIDs::attack);
    releaseParam    = apvts.getRawParameterValue (LoudnessIDs::release);
    hysteresisParam = apvts.getRawParameterValue (LoudnessIDs::hysteresis);
    glideParam      = apvts.getRawParameterValue (LoudnessIDs::glide);
    detectorParam   = apvts.getRawParameterValue (LoudnessIDs::detector);
    bypassParam     = apvts.getRawParameterValue (LoudnessIDs::bypass);
}

LoudnessProcessor::~LoudnessProcessor() = default;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout LoudnessProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Shelf frequency: skewed so the useful 40-200 Hz region gets most of the travel.
    const juce::NormalisableRange<float> freqRange { 20.0f, 2000.0f, 0.1f, 0.30f };
    const juce::NormalisableRange<float> gainRange { -24.0f, 24.0f, 0.1f };
    const juce::NormalisableRange<float> qRange    { 0.1f, 4.0f, 0.01f, 0.6f };
    const juce::NormalisableRange<float> thrRange  { -90.0f, 0.0f, 0.1f };

    for (int s = 0; s < LoudnessIDs::numSettings; ++s)
    {
        const auto& d = defaultSettings[s];
        const auto tag = juce::String (s + 1);

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { LoudnessIDs::threshold (s), 1 },
            "Set " + tag + " Threshold", thrRange, d.thresholdDb,
            juce::AudioParameterFloatAttributes().withLabel ("dBFS")));

        const float gains[] { d.gain1, d.gain2 };
        const float freqs[] { d.freq1, d.freq2 };
        const float qs[]    { d.q1,    d.q2    };

        for (int b = 0; b < LoudnessIDs::numBands; ++b)
        {
            const auto name = "Set " + tag + " EQ" + juce::String (b + 1) + " ";

            layout.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { LoudnessIDs::band (s, b, "gain"), 1 },
                name + "Gain", gainRange, gains[b],
                juce::AudioParameterFloatAttributes().withLabel ("dB")));

            layout.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { LoudnessIDs::band (s, b, "freq"), 1 },
                name + "Freq", freqRange, freqs[b],
                juce::AudioParameterFloatAttributes().withLabel ("Hz")));

            layout.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { LoudnessIDs::band (s, b, "q"), 1 },
                name + "Q", qRange, qs[b]));
        }
    }

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { LoudnessIDs::mode, 1 }, "Mode",
        juce::StringArray { "Auto", "Force 1", "Force 2", "Force 3", "Force 4" }, 0));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { LoudnessIDs::detector, 1 }, "Detector",
        juce::StringArray { "RMS", "Peak" }, 0));

    // VU-like defaults: slow enough that the contour does not pump on transients.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { LoudnessIDs::attack, 1 }, "Detector Attack",
        juce::NormalisableRange<float> { 1.0f, 1000.0f, 0.1f, 0.4f }, 100.0f,
        juce::AudioParameterFloatAttributes().withLabel ("ms")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { LoudnessIDs::release, 1 }, "Detector Release",
        juce::NormalisableRange<float> { 1.0f, 3000.0f, 0.1f, 0.4f }, 300.0f,
        juce::AudioParameterFloatAttributes().withLabel ("ms")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { LoudnessIDs::hysteresis, 1 }, "Hysteresis",
        juce::NormalisableRange<float> { 0.0f, 12.0f, 0.1f }, 3.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { LoudnessIDs::glide, 1 }, "Glide",
        juce::NormalisableRange<float> { 1.0f, 1000.0f, 1.0f, 0.5f }, 120.0f,
        juce::AudioParameterFloatAttributes().withLabel ("ms")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { LoudnessIDs::bypass, 1 }, "Bypass", false));

    return layout;
}

//==============================================================================
void LoudnessProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // A host can prepare the plugin before any audio device exists: JUCE's own LV2 wrapper
    // instantiates with sampleRate 0 / samplesPerBlock 0 when the graph has no device yet
    // (LV2PluginInstance ctor -> prepare). Every filter coefficient below divides by the
    // sample rate, so 0 would put inf/NaN into the biquads and trip JUCE's own assertions.
    // Fall back to something sane; the host prepares again for real once a device opens.
    currentSampleRate      = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int preparedBlock = samplesPerBlock > 0 ? samplesPerBlock : 512;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = currentSampleRate;
    spec.maximumBlockSize = (juce::uint32) preparedBlock;
    spec.numChannels      = 1;                       // mono, like crossover/firConv

    detector.prepare (spec);
    lastAttack = lastRelease = -1.0f;
    lastDetectorType = -1;
    refreshDetectorSettings();

    for (auto& f : eq)
    {
        // Allocate the coefficients once here. Later updates assign from a
        // std::array, which reuses this storage and so never allocates on the
        // audio thread.
        f.coefficients = new juce::dsp::IIR::Coefficients<float> (
            juce::dsp::IIR::ArrayCoefficients<float>::makeLowShelf (currentSampleRate, 100.0, 0.7, 1.0));
        f.prepare (spec);
    }

    // Start already settled on whichever setting the current parameters select,
    // so the plugin does not sweep from a wrong contour when playback begins.
    currentSetting = juce::jlimit (0, LoudnessIDs::numSettings - 1,
                                   (int) modeParam->load() - 1);
    if ((int) modeParam->load() == 0)
        currentSetting = 0;

    const float glideSeconds = glideParam->load() * 0.001f;
    lastGlide = glideParam->load();

    for (int b = 0; b < LoudnessIDs::numBands; ++b)
    {
        gainSmoothed[(size_t) b].reset (currentSampleRate, glideSeconds);
        freqSmoothed[(size_t) b].reset (currentSampleRate, glideSeconds);
        qSmoothed[(size_t) b].reset (currentSampleRate, glideSeconds);

        gainSmoothed[(size_t) b].setCurrentAndTargetValue (gainParam[(size_t) currentSetting][(size_t) b]->load());
        freqSmoothed[(size_t) b].setCurrentAndTargetValue (freqParam[(size_t) currentSetting][(size_t) b]->load());
        qSmoothed[(size_t) b].setCurrentAndTargetValue (qParam[(size_t) currentSetting][(size_t) b]->load());
    }

    reset();
}

void LoudnessProcessor::reset()
{
    detector.reset();
    for (auto& f : eq)
        f.reset();
}

void LoudnessProcessor::releaseResources() {}

void LoudnessProcessor::refreshDetectorSettings() noexcept
{
    const float atk = attackParam->load();
    const float rel = releaseParam->load();
    const int type  = (int) detectorParam->load();

    if (! juce::approximatelyEqual (atk, lastAttack))
    {
        detector.setAttackTime (atk);
        lastAttack = atk;
    }

    if (! juce::approximatelyEqual (rel, lastRelease))
    {
        detector.setReleaseTime (rel);
        lastRelease = rel;
    }

    if (type != lastDetectorType)
    {
        detector.setLevelCalculationType (
            type == 0 ? juce::dsp::BallisticsFilterLevelCalculationType::RMS
                      : juce::dsp::BallisticsFilterLevelCalculationType::peak);
        lastDetectorType = type;
    }
}

//==============================================================================
int LoudnessProcessor::chooseSetting (float detectedDb) noexcept
{
    // Manual override -- indispensable for auditioning a contour while editing it.
    const int mode = (int) modeParam->load();
    if (mode > 0)
    {
        currentSetting = juce::jlimit (0, LoudnessIDs::numSettings - 1, mode - 1);
        return currentSetting;
    }

    std::array<float, LoudnessIDs::numSettings> thresholds;
    for (int s = 0; s < LoudnessIDs::numSettings; ++s)
        thresholds[(size_t) s] = thresholdParam[(size_t) s]->load();

    // The active setting is the one with the highest threshold at or below the
    // level. Thresholds are free-running parameters, so never assume they are
    // ordered -- below every threshold, fall back to the lowest one.
    int candidate = -1;
    for (int s = 0; s < LoudnessIDs::numSettings; ++s)
        if (detectedDb >= thresholds[(size_t) s] && (candidate < 0 || thresholds[(size_t) s] > thresholds[(size_t) candidate]))
            candidate = s;

    if (candidate < 0)
    {
        candidate = 0;
        for (int s = 1; s < LoudnessIDs::numSettings; ++s)
            if (thresholds[(size_t) s] < thresholds[(size_t) candidate])
                candidate = s;
    }

    if (candidate != currentSetting)
    {
        // Only commit once the level has cleared the boundary by the hysteresis
        // amount, otherwise a level parked on a threshold chatters.
        const float hyst = hysteresisParam->load();

        if (thresholds[(size_t) candidate] > thresholds[(size_t) currentSetting])
        {
            if (detectedDb >= thresholds[(size_t) candidate] + hyst)
                currentSetting = candidate;
        }
        else if (detectedDb < thresholds[(size_t) currentSetting] - hyst)
        {
            currentSetting = candidate;
        }
    }

    return currentSetting;
}

void LoudnessProcessor::updateGlideTargets (int setting) noexcept
{
    for (int b = 0; b < LoudnessIDs::numBands; ++b)
    {
        gainSmoothed[(size_t) b].setTargetValue (gainParam[(size_t) setting][(size_t) b]->load());
        freqSmoothed[(size_t) b].setTargetValue (freqParam[(size_t) setting][(size_t) b]->load());
        qSmoothed[(size_t) b].setTargetValue (qParam[(size_t) setting][(size_t) b]->load());
    }
}

//==============================================================================
#ifndef JucePlugin_PreferredChannelConfigurations
bool LoudnessProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono())
        return false;

   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

void LoudnessProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    if (buffer.getNumChannels() == 0)
        return;

    refreshDetectorSettings();

    const float glideMs = glideParam->load();
    if (! juce::approximatelyEqual (glideMs, lastGlide))
    {
        const double seconds = glideMs * 0.001;
        for (int b = 0; b < LoudnessIDs::numBands; ++b)
        {
            gainSmoothed[(size_t) b].reset (currentSampleRate, seconds);
            freqSmoothed[(size_t) b].reset (currentSampleRate, seconds);
            qSmoothed[(size_t) b].reset (currentSampleRate, seconds);
        }
        lastGlide = glideMs;
    }

    const bool bypassed = bypassParam->load() > 0.5f;
    auto* samples = buffer.getWritePointer (0);
    const int numSamples = buffer.getNumSamples();

    for (int pos = 0; pos < numSamples; pos += controlBlockSize)
    {
        const int todo = juce::jmin (controlBlockSize, numSamples - pos);

        // 1. Detect on the INPUT, before the EQ touches it. Reading the output
        //    instead would let the bass boost feed its own level back.
        float envelope = 0.0f;
        for (int i = 0; i < todo; ++i)
            envelope = detector.processSample (0, samples[pos + i]);

        const float db = juce::Decibels::gainToDecibels (envelope, -120.0f);
        levelDb.store (db, std::memory_order_relaxed);

        // 2. Pick the contour set, then 3. glide the EQ toward it.
        const int setting = chooseSetting (db);
        activeSetting.store (setting, std::memory_order_relaxed);
        updateGlideTargets (setting);

        for (int b = 0; b < LoudnessIDs::numBands; ++b)
        {
            const float g = gainSmoothed[(size_t) b].skip (todo);
            const float f = juce::jlimit (10.0f, (float) (currentSampleRate * 0.45),
                                          freqSmoothed[(size_t) b].skip (todo));
            const float q = juce::jmax (0.05f, qSmoothed[(size_t) b].skip (todo));

            liveGain[(size_t) b].store (g, std::memory_order_relaxed);
            liveFreq[(size_t) b].store (f, std::memory_order_relaxed);
            liveQ[(size_t) b].store (q, std::memory_order_relaxed);

            // Assigning from a std::array reuses the storage allocated in
            // prepareToPlay, so this does not allocate.
            *eq[(size_t) b].coefficients = juce::dsp::IIR::ArrayCoefficients<float>::makeLowShelf (
                currentSampleRate, f, q, juce::Decibels::decibelsToGain (g));
        }

        if (! bypassed)
        {
            auto block = juce::dsp::AudioBlock<float> (buffer)
                            .getSubsetChannelBlock (0, 1)
                            .getSubBlock ((size_t) pos, (size_t) todo);
            juce::dsp::ProcessContextReplacing<float> context (block);

            for (auto& f : eq)
                f.process (context);
        }
    }
}

void LoudnessProcessor::getLiveBandParams (int bandIndex, float& gainDb, float& freqHz, float& q) const noexcept
{
    jassert (juce::isPositiveAndBelow (bandIndex, LoudnessIDs::numBands));
    gainDb = liveGain[(size_t) bandIndex].load (std::memory_order_relaxed);
    freqHz = liveFreq[(size_t) bandIndex].load (std::memory_order_relaxed);
    q      = liveQ[(size_t) bandIndex].load (std::memory_order_relaxed);
}

//==============================================================================
bool LoudnessProcessor::hasEditor() const                                   { return true; }
juce::AudioProcessorEditor* LoudnessProcessor::createEditor()               { return new LoudnessEditor (*this); }
const juce::String LoudnessProcessor::getName() const                       { return JucePlugin_Name; }
bool LoudnessProcessor::isMidiEffect() const                                { return false; }
double LoudnessProcessor::getTailLengthSeconds() const                      { return 0.0; }
int LoudnessProcessor::getNumPrograms()                                     { return 1; }
int LoudnessProcessor::getCurrentProgram()                                  { return 0; }
void LoudnessProcessor::setCurrentProgram (int)                             {}
const juce::String LoudnessProcessor::getProgramName (int)                  { return {}; }
void LoudnessProcessor::changeProgramName (int, const juce::String&)        {}

bool LoudnessProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool LoudnessProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

//==============================================================================
namespace
{
    const juce::Identifier viewTag ("VIEW");
    const juce::Identifier minHzId ("minHz"), maxHzId ("maxHz"), bottomDbId ("bottomDb"), topDbId ("topDb");
}

LoudnessProcessor::ViewLimits LoudnessProcessor::ViewLimits::sanitised() const noexcept
{
    ViewLimits v = *this;
    const ViewLimits defaults;

    v.minHz = juce::jlimit (lowestHz, highestHz, std::isfinite (v.minHz) ? v.minHz : defaults.minHz);
    v.maxHz = juce::jlimit (lowestHz, highestHz, std::isfinite (v.maxHz) ? v.maxHz : defaults.maxHz);

    if (v.maxHz < v.minHz * minRatio)
    {
        v.minHz = defaults.minHz;
        v.maxHz = defaults.maxHz;
    }

    v.bottomDb = juce::jlimit (lowestDb, highestDb, std::isfinite (v.bottomDb) ? v.bottomDb : defaults.bottomDb);
    v.topDb    = juce::jlimit (lowestDb, highestDb, std::isfinite (v.topDb) ? v.topDb : defaults.topDb);

    if (v.topDb < v.bottomDb + minSpanDb)
    {
        v.bottomDb = defaults.bottomDb;
        v.topDb    = defaults.topDb;
    }

    return v;
}

LoudnessProcessor::ViewLimits LoudnessProcessor::getViewLimits() const noexcept
{
    const juce::SpinLock::ScopedLockType lock (viewLock);
    return viewLimits;
}

void LoudnessProcessor::setViewLimits (const ViewLimits& newLimits) noexcept
{
    {
        const juce::SpinLock::ScopedLockType lock (viewLock);
        viewLimits = newLimits.sanitised();
    }

    viewVersion.fetch_add (1, std::memory_order_release);
}

void LoudnessProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // copyState() is a deep copy, so adding the view to it cannot race the live tree.
    auto state = apvts.copyState();
    const auto view = getViewLimits();

    juce::ValueTree viewTree (viewTag);
    viewTree.setProperty (minHzId, view.minHz, nullptr);
    viewTree.setProperty (maxHzId, view.maxHz, nullptr);
    viewTree.setProperty (bottomDbId, view.bottomDb, nullptr);
    viewTree.setProperty (topDbId, view.topDb, nullptr);
    state.appendChild (viewTree, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void LoudnessProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        auto state = juce::ValueTree::fromXml (*xml);

        // Sessions saved before the view existed simply keep the default view.
        if (auto viewTree = state.getChildWithName (viewTag); viewTree.isValid())
        {
            const ViewLimits defaults;
            ViewLimits view;
            view.minHz    = (float) viewTree.getProperty (minHzId, defaults.minHz);
            view.maxHz    = (float) viewTree.getProperty (maxHzId, defaults.maxHz);
            view.bottomDb = (float) viewTree.getProperty (bottomDbId, defaults.bottomDb);
            view.topDb    = (float) viewTree.getProperty (topDbId, defaults.topDb);
            setViewLimits (view);

            state.removeChild (viewTree, nullptr);  // not APVTS state; keep it out of the tree
        }

        apvts.replaceState (state);
    }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LoudnessProcessor();
}
