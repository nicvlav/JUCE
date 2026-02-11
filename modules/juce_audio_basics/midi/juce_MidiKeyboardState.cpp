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

MidiKeyboardState::MidiKeyboardState()
{
    zerostruct (noteStates);
}

//==============================================================================
void MidiKeyboardState::reset()
{
    const ScopedLock sl (lock);
    zerostruct (noteStates);
    eventsToAdd.clear();
}

bool MidiKeyboardState::isNoteOn (const int midiChannel, const int n) const noexcept
{
    jassert (midiChannel > 0 && midiChannel <= 16);

    return isPositiveAndBelow (n, 128)
            && (noteStates[n] & (1 << (midiChannel - 1))) != 0;
}

bool MidiKeyboardState::isNoteOnForChannels (const int midiChannelMask, const int n) const noexcept
{
    return isPositiveAndBelow (n, 128)
            && (noteStates[n] & midiChannelMask) != 0;
}

void MidiKeyboardState::noteOn (const int midiChannel, const int midiNoteNumber, const float velocity)
{
    jassert (midiChannel > 0 && midiChannel <= 16);
    jassert (isPositiveAndBelow (midiNoteNumber, 128));

    const ScopedLock sl (lock);

    if (isPositiveAndBelow (midiNoteNumber, 128))
    {
        const int timeNow = (int) Time::getMillisecondCounter();
        eventsToAdd.addEvent (MidiMessage::noteOn (midiChannel, midiNoteNumber, velocity), timeNow);
        eventsToAdd.clear (0, timeNow - 500);

        noteOnInternal (midiChannel, midiNoteNumber, velocity);
    }
}

void MidiKeyboardState::noteOnInternal  (const int midiChannel, const int midiNoteNumber, const float velocity)
{
    if (isPositiveAndBelow (midiNoteNumber, 128))
    {
        noteStates[midiNoteNumber] = static_cast<uint16> (noteStates[midiNoteNumber] | (1 << (midiChannel - 1)));
        listeners.call ([&] (Listener& l) { l.handleNoteOn (this, midiChannel, midiNoteNumber, velocity); });
    }
}

void MidiKeyboardState::noteOff (const int midiChannel, const int midiNoteNumber, const float velocity)
{
    const ScopedLock sl (lock);

    if (isNoteOn (midiChannel, midiNoteNumber))
    {
        const int timeNow = (int) Time::getMillisecondCounter();
        eventsToAdd.addEvent (MidiMessage::noteOff (midiChannel, midiNoteNumber), timeNow);
        eventsToAdd.clear (0, timeNow - 500);

        noteOffInternal (midiChannel, midiNoteNumber, velocity);
    }
}

void MidiKeyboardState::noteOffInternal  (const int midiChannel, const int midiNoteNumber, const float velocity)
{
    if (isNoteOn (midiChannel, midiNoteNumber))
    {
        noteStates[midiNoteNumber] = static_cast<uint16> (noteStates[midiNoteNumber] & ~(1 << (midiChannel - 1)));
        listeners.call ([&] (Listener& l) { l.handleNoteOff (this, midiChannel, midiNoteNumber, velocity); });
    }
}

void MidiKeyboardState::allNotesOff (const int midiChannel)
{
    const ScopedLock sl (lock);

    if (midiChannel <= 0)
    {
        for (int i = 1; i <= 16; ++i)
            allNotesOff (i);
    }
    else
    {
        for (int i = 0; i < 128; ++i)
            noteOff (midiChannel, i, 0.0f);
    }
}

void MidiKeyboardState::processNextMidiEvent (const MidiMessage& message)
{
    if (message.isNoteOn())
    {
        noteOnInternal (message.getChannel(), message.getNoteNumber(), message.getFloatVelocity());
    }
    else if (message.isNoteOff())
    {
        noteOffInternal (message.getChannel(), message.getNoteNumber(), message.getFloatVelocity());
    }
    else if (message.isAllNotesOff())
    {
        for (int i = 0; i < 128; ++i)
            noteOffInternal (message.getChannel(), i, 0.0f);
    }
}

void MidiKeyboardState::processNextUmpEvent (ump::View packet)
{
    const auto firstWord = packet[0];
    const auto messageType = ump::Utils::getMessageType (firstWord);
    const auto status = ump::Utils::getStatus (firstWord);
    const auto channel = static_cast<int> (ump::Utils::getChannel (firstWord)) + 1;

    if (messageType == ump::Utils::MessageKind::channelVoice1)
    {
        const auto byte2 = ump::Utils::U8<2>::get (firstWord);
        const auto byte3 = ump::Utils::U8<3>::get (firstWord);

        if (status == std::byte { 0x9 } && byte3 > 0)
            noteOnInternal (channel, static_cast<int> (byte2), byte3 / 127.0f);
        else if (status == std::byte { 0x8 } || (status == std::byte { 0x9 } && byte3 == 0))
            noteOffInternal (channel, static_cast<int> (byte2), byte3 / 127.0f);
        else if (status == std::byte { 0xb } && byte2 == 123) // CC 123 = All Notes Off
            for (int i = 0; i < 128; ++i)
                noteOffInternal (channel, i, 0.0f);
    }
    else if (messageType == ump::Utils::MessageKind::channelVoice2)
    {
        const auto byte2 = ump::Utils::U8<2>::get (firstWord);

        if (status == std::byte { 0x9 })
        {
            const auto velocity16bit = ump::Utils::U16<2>::get (packet[1]);

            if (velocity16bit > 0)
                noteOnInternal (channel, static_cast<int> (byte2), velocity16bit / 65535.0f);
            else
                noteOffInternal (channel, static_cast<int> (byte2), 0.0f);
        }
        else if (status == std::byte { 0x8 })
        {
            const auto velocity16bit = ump::Utils::U16<2>::get (packet[1]);
            noteOffInternal (channel, static_cast<int> (byte2), velocity16bit / 65535.0f);
        }
        else if (status == std::byte { 0xb } && byte2 == 123) // CC 123 = All Notes Off
        {
            for (int i = 0; i < 128; ++i)
                noteOffInternal (channel, i, 0.0f);
        }
    }
}

void MidiKeyboardState::processNextMidiBuffer (MidiBuffer& buffer,
                                               const int startSample,
                                               const int numSamples,
                                               const bool injectIndirectEvents)
{
    const ScopedLock sl (lock);

    for (const auto metadata : buffer)
        processNextMidiEvent (metadata.getMessage());

    if (injectIndirectEvents)
    {
        const int firstEventToAdd = eventsToAdd.getFirstEventTime();
        const double scaleFactor = numSamples / (double) (eventsToAdd.getLastEventTime() + 1 - firstEventToAdd);

        for (const auto metadata : eventsToAdd)
        {
            const auto pos = jlimit (0, numSamples - 1, roundToInt ((metadata.samplePosition - firstEventToAdd) * scaleFactor));
            buffer.addEvent (metadata.getMessage(), startSample + pos);
        }
    }

    eventsToAdd.clear();
}

void MidiKeyboardState::processNextUmpBuffer (UMPBuffer& buffer,
                                              const int startSample,
                                              const int numSamples,
                                              const bool injectIndirectEvents)
{
    const ScopedLock sl (lock);

    for (const auto& meta : buffer)
        processNextUmpEvent (meta.packet);

    if (injectIndirectEvents && ! eventsToAdd.isEmpty())
    {
        const int firstEventToAdd = eventsToAdd.getFirstEventTime();
        const double scaleFactor = numSamples / (double) (eventsToAdd.getLastEventTime() + 1 - firstEventToAdd);

        // Convert pending MIDI 1.0 events to UMP and inject
        ump::ToUMP1Converter converter;

        for (const auto metadata : eventsToAdd)
        {
            const auto pos = jlimit (0, numSamples - 1, roundToInt ((metadata.samplePosition - firstEventToAdd) * scaleFactor));
            const auto msg = metadata.getMessage();

            converter.convert ({ {}, msg.asSpan() }, [&buffer, samplePos = startSample + pos] (const ump::View& view)
            {
                buffer.addPacket (view, samplePos);
            });
        }
    }

    eventsToAdd.clear();
}

//==============================================================================
void MidiKeyboardState::addListener (Listener* listener)
{
    const ScopedLock sl (lock);
    listeners.add (listener);
}

void MidiKeyboardState::removeListener (Listener* listener)
{
    const ScopedLock sl (lock);
    listeners.remove (listener);
}

} // namespace juce
