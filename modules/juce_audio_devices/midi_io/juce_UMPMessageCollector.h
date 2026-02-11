/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

    //==============================================================================
    /**
    Collects incoming UMP packets from a MIDI input thread and makes them
    available in an audio callback as a UMPBuffer.

    This is the UMP equivalent of MidiMessageCollector. Packets arrive on the
    MIDI input thread via addPacketToQueue() and are extracted on the audio
    thread via removeNextBlockOfPackets(). The same time-scaling algorithm
    used by MidiMessageCollector is applied to map real-time arrival stamps
    into sample-accurate positions within the audio block.

    @see UMPBuffer, MidiMessageCollector

    @tags{Audio}
*/
    class JUCE_API  UMPMessageCollector : public MidiKeyboardState::Listener,
                                          public MidiInputCallback {
    public:
        //==============================================================================
        UMPMessageCollector();
        ~UMPMessageCollector();

        //==============================================================================
        /** Prepares the collector for use at a given sample rate.

        Call this before the first audio callback and whenever the sample rate
        changes.
    */
    void reset (double sampleRate);

        /** Adds a UMP packet to the queue.

        Call this from the MIDI input thread. The packet data is copied.
        The @p time parameter should be in seconds, as provided by MidiInput
        callbacks (i.e. `Time::getMillisecondCounterHiRes() * 0.001`).
    */
    void addPacketToQueue (ump::View packet, double time);

        /** Extracts all queued packets into @p destBuffer, mapping their arrival
        times into sample positions within the current block.

        Call this at the top of each audio callback. The destination buffer is
        not cleared first.

        @param destBuffer   the UMPBuffer to receive the packets
        @param numSamples   the number of samples in the current audio block
    */
    void removeNextBlockOfPackets (UMPBuffer& destBuffer, int numSamples);

    /** Pre-allocates internal storage to avoid allocation during processing. */
    void ensureStorageAllocated (size_t numPackets);

    
    //==============================================================================
    /** @internal */
    void handleNoteOn(MidiKeyboardState *, int midiChannel, int midiNoteNumber, float velocity) override;
    /** @internal */
    void handleNoteOff(MidiKeyboardState *, int midiChannel, int midiNoteNumber, float velocity) override;
    /** @internal */
    void handleIncomingMidiMessage(MidiInput *, const MidiMessage &) override {}
    /** @internal */
    void handleIncomingUMPPacket(MidiInput *, ump::View, double) override;
private:
    //==============================================================================
    struct StoredPacket
    {
        size_t storageIndex;
        int sampleNumber;
        uint8_t packetSize;
    };

    CriticalSection lock;
    double currentSampleRate = 44100.0;
    double lastCallbackTime = 0;
    std::vector<uint32_t> storage;
    std::vector<StoredPacket> incoming;

#if JUCE_DEBUG
    bool hasCalledReset = false;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UMPMessageCollector)
};

}// namespace juce
