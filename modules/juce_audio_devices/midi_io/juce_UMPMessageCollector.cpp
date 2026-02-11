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

UMPMessageCollector::UMPMessageCollector()
{
}

UMPMessageCollector::~UMPMessageCollector()
{
}

//==============================================================================
void UMPMessageCollector::reset (const double newSampleRate)
{
    const ScopedLock sl (lock);

    jassert (newSampleRate > 0);

   #if JUCE_DEBUG
    hasCalledReset = true;
   #endif

    currentSampleRate = newSampleRate;
    storage.clear();
    incoming.clear();
    lastCallbackTime = Time::getMillisecondCounterHiRes();
}

void UMPMessageCollector::addPacketToQueue (ump::View packet, double time)
{
    const ScopedLock sl (lock);

   #if JUCE_DEBUG
    jassert (hasCalledReset); // you need to call reset() to set the correct sample rate before using this object
   #endif

    jassert (packet.size() >= 1 && packet.size() <= 4);

    // time is in milliseconds (from Time::getMillisecondCounterHiRes);
    // lastCallbackTime is also in milliseconds.
    // Clamp to 0: a negative value means the event arrived after the last
    // audio-thread drain but with an OS timestamp older than lastCallbackTime.
    // Placing it at sample 0 ensures it is included in the next block.
    auto sampleNumber = jmax (0, (int) ((time - lastCallbackTime) * 0.001 * currentSampleRate));

    const auto storageIndex = storage.size();
    const auto packetSize = static_cast<uint8_t> (packet.size());
    storage.insert (storage.end(), packet.begin(), packet.end());

    incoming.push_back ({ storageIndex, sampleNumber, packetSize });

    // discard anything older than one second to prevent unbounded growth
    if (sampleNumber > (int) currentSampleRate)
    {
        const auto cutoff = sampleNumber - (int) currentSampleRate;
        auto newEnd = std::remove_if (incoming.begin(), incoming.end(),
                                      [cutoff] (const StoredPacket& p) { return p.sampleNumber < cutoff; });
        incoming.erase (newEnd, incoming.end());
        // Note: we don't compact storage here — it will be cleared
        // on the next removeNextBlockOfPackets call anyway.
    }
}

void UMPMessageCollector::removeNextBlockOfPackets (UMPBuffer& destBuffer,
                                                     const int numSamples)
{
    const ScopedLock sl (lock);

   #if JUCE_DEBUG
    jassert (hasCalledReset); // you need to call reset() to set the correct sample rate before using this object
   #endif

    jassert (numSamples > 0);

    auto timeNow = Time::getMillisecondCounterHiRes();
    auto msElapsed = timeNow - lastCallbackTime;

    lastCallbackTime = timeNow;

    if (! incoming.empty())
    {
        int numSourceSamples = jmax (1, roundToInt (msElapsed * 0.001 * currentSampleRate));
        int startSample = 0;
        int scale = 1 << 16;

        if (numSourceSamples > numSamples)
        {
            // if the queued events span more time than the block we have,
            // scale them down to fit
            const int maxBlockLengthToUse = numSamples << 5;

            if (numSourceSamples > maxBlockLengthToUse)
            {
                startSample = numSourceSamples - maxBlockLengthToUse;
                numSourceSamples = maxBlockLengthToUse;
            }

            scale = (numSamples << 10) / numSourceSamples;

            for (const auto& p : incoming)
            {
                if (p.sampleNumber < startSample)
                    continue;

                const auto pos = ((p.sampleNumber - startSample) * scale) >> 10;
                destBuffer.addPacket (ump::View { &storage[p.storageIndex] },
                                      jlimit (0, numSamples - 1, pos));
            }
        }
        else
        {
            // if the events span less time than the block, push them
            // toward the end
            startSample = numSamples - numSourceSamples;

            for (const auto& p : incoming)
            {
                destBuffer.addPacket (ump::View { &storage[p.storageIndex] },
                                      jlimit (0, numSamples - 1, p.sampleNumber + startSample));
            }
        }

        incoming.clear();
        storage.clear();
    }
}

void UMPMessageCollector::ensureStorageAllocated (size_t numPackets)
{
    incoming.reserve (numPackets);
    storage.reserve (numPackets * 4);
}

//==============================================================================
void UMPMessageCollector::handleNoteOn (MidiKeyboardState*, int midiChannel, int midiNoteNumber, float velocity)
{
    const auto v1 = ump::Factory::makeNoteOnV1 (0,
                                                (uint8_t) midiChannel - 1,
                                                (uint8_t) midiNoteNumber,
                                                (uint8_t) (velocity * (1 << 7)));
    addPacketToQueue (ump::View { v1.data() }, Time::getMillisecondCounterHiRes());
}

void UMPMessageCollector::handleNoteOff (MidiKeyboardState*, int midiChannel, int midiNoteNumber, float velocity)
{
    const auto v1 = ump::Factory::makeNoteOffV1 (0,
                                                    (uint8_t) midiChannel - 1,
                                                    (uint8_t) midiNoteNumber,
                                                    (uint8_t) (velocity * (1 << 7)));
    addPacketToQueue (ump::View { v1.data() }, Time::getMillisecondCounterHiRes());
}

void UMPMessageCollector::handleIncomingUMPPacket (MidiInput *, ump::View packet, double time)
{
    addPacketToQueue (packet, time);
}


} // namespace juce
