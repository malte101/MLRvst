/*
  ==============================================================================

    StepSampler.h
    Monophonic JUCE sampler for step sequencer
    - Separate from audio strip
    - Built-in ADSR envelope
    - Loads its own samples
    - Connected to strip volume/pan

  ==============================================================================
*/

#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

// Filter type enum (matches AudioEngine)
enum class FilterType
{
    LowPass = 0,
    HighPass = 1,
    BandPass = 2
};

class StepSampler
{
public:
    StepSampler()
    {
        // Monophonic: only 1 voice (retriggering stops previous note)
        synth.addVoice(new juce::SamplerVoice());
        
        // Register audio formats
        formatManager.registerBasicFormats();
        
        // Initialize filter to low-pass at 1000Hz
        updateFilter();
        updateAmpEnvelopeParameters();
    }
    
    ~StepSampler()
    {
        // Clean up temp file
        if (tempAudioFile.existsAsFile())
            tempAudioFile.deleteFile();
    }
    
    void prepareToPlay(double newSampleRate, int samplesPerBlock)
    {
        synth.setCurrentPlaybackSampleRate(newSampleRate);
        this->sampleRate = newSampleRate;
        ampEnvelope.setSampleRate(newSampleRate);
        ampEnvelope.reset();
        updateAmpEnvelopeParameters();
        
        // Prepare filter
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = newSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
        spec.numChannels = 2;
        
        filter.prepare(spec);
        updateFilter();
    }
    
    void loadSample(const juce::File& file)
    {
        // Clear existing sounds
        synth.clearSounds();
        
        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
        if (reader != nullptr)
        {
            // All MIDI notes trigger same sample
            juce::BigInteger allNotes;
            allNotes.setRange(0, 128, true);
            
            // Create sampler sound with drum-like ADSR
            synth.addSound(new juce::SamplerSound(
                "StepSample",           // Name
                *reader,                // Audio reader
                allNotes,               // All MIDI notes
                60,                     // Root note (C4)
                0.001,                  // Keep internal attack minimal
                0.005,                  // Keep internal release minimal
                10.0                    // Max sample length (seconds)
            ));
            
            hasAudio = true;
            
            DBG("StepSampler: Loaded sample - " << file.getFileName());
        }
        else
        {
            hasAudio = false;
            DBG("StepSampler: Failed to load sample");
        }
    }
    
    void loadSampleFromBuffer(const juce::AudioBuffer<float>& buffer, double sourceSampleRate)
    {
        synth.clearSounds();
        
        if (buffer.getNumSamples() == 0)
        {
            hasAudio = false;
            return;
        }
        
        DBG("StepSampler: Loading buffer - source=" << sourceSampleRate << "Hz, playback=" << sampleRate << "Hz, samples=" << buffer.getNumSamples());
        
        // Resample buffer to match playback sample rate if needed
        juce::AudioBuffer<float> resampledBuffer;
        double targetSampleRate = sampleRate;  // Use playback sample rate
        
        if (std::abs(sourceSampleRate - targetSampleRate) < 0.1)
        {
            // Sample rates match - use buffer directly
            resampledBuffer.setSize(buffer.getNumChannels(), buffer.getNumSamples());
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                resampledBuffer.copyFrom(ch, 0, buffer, ch, 0, buffer.getNumSamples());
            
            DBG("StepSampler: No resampling needed");
        }
        else
        {
            // Need to resample
            double ratio = targetSampleRate / sourceSampleRate;
            int newLength = static_cast<int>(buffer.getNumSamples() * ratio);
            
            resampledBuffer.setSize(buffer.getNumChannels(), newLength);
            
            // Simple linear interpolation resampling
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const float* input = buffer.getReadPointer(ch);
                float* output = resampledBuffer.getWritePointer(ch);
                
                for (int i = 0; i < newLength; ++i)
                {
                    double sourcePos = i / ratio;
                    int index = static_cast<int>(sourcePos);
                    float frac = static_cast<float>(sourcePos - index);
                    
                    if (index < buffer.getNumSamples() - 1)
                    {
                        // Linear interpolation
                        output[i] = input[index] * (1.0f - frac) + input[index + 1] * frac;
                    }
                    else if (index < buffer.getNumSamples())
                    {
                        output[i] = input[index];
                    }
                    else
                    {
                        output[i] = 0.0f;
                    }
                }
            }
            
            DBG("StepSampler: Resampled " << sourceSampleRate << "Hz -> " << targetSampleRate << "Hz (" << buffer.getNumSamples() << " -> " << newLength << " samples, ratio=" << ratio << ")");
        }
        
        // Write resampled buffer to memory block at target sample rate
        juce::MemoryBlock memoryBlock;
        
        {
            juce::WavAudioFormat wavFormat;
            std::unique_ptr<juce::OutputStream> memoryStream =
                std::make_unique<juce::MemoryOutputStream>(memoryBlock, false);

            const auto writerOptions = juce::AudioFormatWriterOptions{}
                .withSampleRate(targetSampleRate)
                .withNumChannels(resampledBuffer.getNumChannels())
                .withBitsPerSample(24)
                .withQualityOptionIndex(0);
            std::unique_ptr<juce::AudioFormatWriter> writer(wavFormat.createWriterFor(memoryStream, writerOptions));
            
            if (writer)
            {
                writer->writeFromAudioSampleBuffer(resampledBuffer, 0, resampledBuffer.getNumSamples());
                writer->flush();
                DBG("StepSampler: WAV written - " << resampledBuffer.getNumSamples() << " samples at " << targetSampleRate << "Hz");
            } else
            {
                DBG("StepSampler: Failed to create WAV writer");
                hasAudio = false;
                return;
            }
        }
        
        // Create reader from memory block
        juce::MemoryInputStream* memoryInput = new juce::MemoryInputStream(memoryBlock, true);
        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::AudioFormatReader> reader(
            wavFormat.createReaderFor(memoryInput, true));  // true = reader owns and deletes the stream
        
        if (reader)
        {
            DBG("StepSampler: Reader created - sampleRate=" << reader->sampleRate << ", length=" << reader->lengthInSamples << ", channels=" << (int)reader->numChannels);
            
            juce::BigInteger allNotes;
            allNotes.setRange(0, 128, true);
            
            synth.addSound(new juce::SamplerSound(
                "StepSample",
                *reader,
                allNotes,
                60,        // Root note
                0.001,     // Keep internal attack minimal
                0.005,     // Keep internal release minimal
                10.0       // Max length
            ));
            
            hasAudio = true;
            
            DBG("StepSampler: Loaded from buffer (" << resampledBuffer.getNumSamples() << " samples at " << targetSampleRate << "Hz)");
        }
        else
        {
            hasAudio = false;
            DBG("StepSampler: Failed to create reader from memory");
        }
    }
    
    void triggerNote(float velocity = 1.0f)
    {
        if (!hasAudio) return;
        
        // Monophonic: stop any playing note first
        synth.allNotesOff(0, true);
        
        // Trigger new note with pitch offset
        int midiNote = 60 + pitchOffset;  // C4 + offset
        midiNote = juce::jlimit(0, 127, midiNote);
        
        synth.noteOn(1,      // MIDI channel
                    midiNote, // Note number (with pitch offset)
                    velocity); // Velocity (0-1)

        ampEnvelope.noteOn();
        isPlaying = true;
    }
    
    void stopNote()
    {
        // Stop all notes (since we might have different pitches)
        synth.allNotesOff(0, true);
        ampEnvelope.noteOff();
        isPlaying = false;
    }
    
    void allNotesOff()
    {
        synth.allNotesOff(0, true);
        ampEnvelope.noteOff();
        isPlaying = false;
    }
    
    void setVolume(float vol)
    {
        // Volume 0.0 to 1.0
        this->volume = juce::jlimit(0.0f, 1.0f, vol);
    }
    
    void setPan(float panValue)
    {
        // Pan -1.0 (left) to 1.0 (right)
        this->pan = juce::jlimit(-1.0f, 1.0f, panValue);
    }
    
    void setSpeed(float speed)
    {
        // Speed control via pitch shifting
        // 0.5 = down octave, 1.0 = normal, 2.0 = up octave
        // Convert to semitone offset: 12 semitones per octave
        if (speed > 0.0f)
        {
            double semitones = 12.0 * std::log2(speed);
            pitchOffset = static_cast<int>(std::round(semitones));
            pitchOffset = juce::jlimit(-24, 24, pitchOffset);  // ±2 octaves
        }
    }

    void setAmpAttackMs(float ms)
    {
        ampAttackMs = juce::jlimit(0.0f, 400.0f, ms);
        updateAmpEnvelopeParameters();
    }

    void setAmpDecayMs(float ms)
    {
        ampDecayMs = juce::jlimit(1.0f, 4000.0f, ms);
        updateAmpEnvelopeParameters();
    }

    void setAmpReleaseMs(float ms)
    {
        ampReleaseMs = juce::jlimit(1.0f, 4000.0f, ms);
        updateAmpEnvelopeParameters();
    }

    float getAmpAttackMs() const { return ampAttackMs; }
    float getAmpDecayMs() const { return ampDecayMs; }
    float getAmpReleaseMs() const { return ampReleaseMs; }
    
    // Filter control methods
    void setFilterEnabled(bool enabled) 
    { 
        filterEnabled = enabled; 
    }
    
    void setFilterFrequency(float freq)
    {
        filterFrequency = juce::jlimit(20.0f, 20000.0f, freq);
        updateFilter();
    }
    
    void setFilterResonance(float res)
    {
        filterResonance = juce::jlimit(0.1f, 10.0f, res);
        updateFilter();
    }
    
    void setFilterType(FilterType type)
    {
        filterType = type;
        updateFilter();
    }
    
    // Getters
    float getVolume() const { return volume; }
    float getPan() const { return pan; }
    int getPitchOffset() const { return pitchOffset; }
    bool isFilterEnabled() const { return filterEnabled; }
    float getFilterFrequency() const { return filterFrequency; }
    float getFilterResonance() const { return filterResonance; }
    FilterType getFilterType() const { return filterType; }
    
    void process(juce::AudioBuffer<float>& output, int startSample, int numSamples)
    {
        if (!hasAudio || numSamples <= 0)
            return;
        
        // Create temporary buffer for synth output
        tempBuffer.setSize(output.getNumChannels(), numSamples, false, false, true);
        tempBuffer.clear();
        
        // Render synth to temp buffer
        juce::MidiBuffer midiMessages;  // Empty
        synth.renderNextBlock(tempBuffer, midiMessages, 0, numSamples);

        // Per-step envelope (A/D/R) for step mode dynamics.
        const int channels = tempBuffer.getNumChannels();
        for (int sample = 0; sample < numSamples; ++sample)
        {
            const float env = ampEnvelope.getNextSample();
            for (int ch = 0; ch < channels; ++ch)
                tempBuffer.getWritePointer(ch)[sample] *= env;
        }
        
        // Apply filter BEFORE volume/pan
        if (filterEnabled && tempBuffer.getNumChannels() > 0)
        {
            juce::dsp::AudioBlock<float> block(tempBuffer);
            juce::dsp::ProcessContextReplacing<float> context(block);
            filter.process(context);
        }
        
        // Apply volume and pan, then add to output
        for (int channel = 0; channel < output.getNumChannels() && channel < 2; ++channel)
        {
            // Calculate pan gains
            float leftGain = 1.0f;
            float rightGain = 1.0f;
            
            if (channel == 0)  // Left
            {
                leftGain = pan <= 0.0f ? 1.0f : (1.0f - pan);
            }
            else if (channel == 1)  // Right
            {
                rightGain = pan >= 0.0f ? 1.0f : (1.0f + pan);
            }
            
            float gain = volume * (channel == 0 ? leftGain : rightGain);
            
            // Add to output buffer
            output.addFrom(channel, startSample, tempBuffer, channel, 0, numSamples, gain);
        }
    }
    
    bool getIsPlaying() const { return isPlaying; }
    bool getHasAudio() const { return hasAudio; }

private:
    juce::Synthesiser synth;  // JUCE Synthesiser (not Sampler!)
    juce::AudioFormatManager formatManager;
    juce::AudioBuffer<float> tempBuffer;
    juce::File tempAudioFile;  // Temporary file for buffer-loaded samples
    
    double sampleRate = 44100.0;
    bool hasAudio = false;
    bool isPlaying = false;
    juce::ADSR ampEnvelope;
    juce::ADSR::Parameters ampEnvelopeParams;
    float ampAttackMs = 0.0f;
    float ampDecayMs = 4000.0f;
    float ampReleaseMs = 110.0f;
    
    // Volume and pan (connected to strip controls)
    float volume = 1.0f;
    float pan = 0.0f;
    int pitchOffset = 0;  // Semitone offset for speed/pitch control
    
    // Filter state
    juce::dsp::ProcessorDuplicator<
        juce::dsp::IIR::Filter<float>,
        juce::dsp::IIR::Coefficients<float>> filter;
    
    bool filterEnabled = false;
    float filterFrequency = 1000.0f;
    float filterResonance = 0.7f;
    FilterType filterType = FilterType::LowPass;
    
    // Update filter coefficients
    void updateFilter()
    {
        auto coeffs = juce::dsp::IIR::Coefficients<float>::makeLowPass(
            sampleRate, filterFrequency, filterResonance);
        
        switch (filterType)
        {
            case FilterType::LowPass:
                coeffs = juce::dsp::IIR::Coefficients<float>::makeLowPass(
                    sampleRate, filterFrequency, filterResonance);
                break;
            case FilterType::HighPass:
                coeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass(
                    sampleRate, filterFrequency, filterResonance);
                break;
            case FilterType::BandPass:
                coeffs = juce::dsp::IIR::Coefficients<float>::makeBandPass(
                    sampleRate, filterFrequency, filterResonance);
                break;
        }
        
        *filter.state = *coeffs;
    }

    void updateAmpEnvelopeParameters()
    {
        ampEnvelopeParams.attack = juce::jlimit(0.0f, 0.4f, ampAttackMs * 0.001f);
        ampEnvelopeParams.decay = juce::jlimit(0.001f, 4.0f, ampDecayMs * 0.001f);
        ampEnvelopeParams.sustain = 0.0f;
        ampEnvelopeParams.release = juce::jlimit(0.001f, 4.0f, ampReleaseMs * 0.001f);
        ampEnvelope.setParameters(ampEnvelopeParams);
    }
};
