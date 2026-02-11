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
    Metadata for a single Universal MIDI Packet stored in a UMPBuffer.

    Instances of this class do not own the memory they reference. They are
    valid only for the lifetime of the UMPBuffer that produced them.

    @see UMPBuffer, universal_midi_packets::View

    @tags{Audio}
*/
struct UMPPacketMetadata
{
    UMPPacketMetadata() noexcept = default;

    UMPPacketMetadata (ump::View packetView, int position) noexcept
        : packet (packetView), samplePosition (position)
    {
    }

    /** A non-owning view of the UMP packet data. */
    ump::View packet;

    /** The sample offset within the current audio buffer at which this
        packet should be applied.
    */
    int samplePosition = 0;
};

//==============================================================================
/**
    Holds a time-stamped sequence of Universal MIDI Packets.

    This is the UMP equivalent of MidiBuffer. Packets are stored with
    sample-accurate positions and kept sorted by time in a single flat
    vector of uint32_t words, matching the same interleaved encoding
    strategy used by MidiBuffer.

    Each event is stored as:
    @code
    [1 word : sample position (reinterpreted as uint32_t)]
    [1-4 words : UMP packet data]
    @endcode

    The packet word count is derived from the UMP message type bits in
    the first packet word, so no separate size field is needed.

    @code
    // In your audio callback:
    for (const auto& meta : umpBuffer)
    {
        if (auto cv = ump::asMIDI2ChannelVoice (meta.packet))
        {
            // Full 32-bit velocity, per-note controllers, etc.
        }
    }
    @endcode

    @see MidiBuffer, UMPPacketMetadata, UMPMessageCollector

    @tags{Audio}
*/
class JUCE_API  UMPBuffer
{
public:
    //==============================================================================
    /** Creates an empty buffer. */
    UMPBuffer() = default;

    //==============================================================================
    /** Adds a packet at a given sample position.

        The data pointed to by @p packet is copied into internal storage.
        Packets are kept sorted by sample position; packets at the same
        position maintain insertion order (FIFO).

        @param packet          a valid View of a UMP packet (1-4 words)
        @param samplePosition  the sample offset within the audio block
    */
    void addPacket (ump::View packet, int samplePosition)
    {
        jassert (samplePosition >= 0);
        jassert (packet.size() >= 1 && packet.size() <= 4);

        const auto numWords = packet.size();
        const auto totalSize = 1 + numWords;   // 1 word for timestamp + packet words

        // Find insertion point: past all events with timestamp <= samplePosition
        auto* d = findEventAfter (data.data(), data.data() + data.size(), samplePosition);
        auto offset = static_cast<size_t> (d - data.data());

        data.insert (data.begin() + static_cast<std::ptrdiff_t> (offset),
                     totalSize, uint32_t (0));

        auto* dest = data.data() + offset;
        *dest++ = static_cast<uint32_t> (samplePosition);
        std::copy_n (packet.begin(), numWords, dest);
    }

    /** Removes all packets without deallocating storage. */
    void clear() noexcept
    {
        data.clear();
    }

    /** Returns true if the buffer contains no packets. */
    bool isEmpty() const noexcept { return data.empty(); }

    /** Returns the number of packets in the buffer.
        Note: this is O(n) as it walks the interleaved stream.
    */
    int getNumPackets() const noexcept
    {
        int n = 0;
        auto* d   = data.data();
        auto* end = d + data.size();

        while (d < end)
        {
            d += getEventTotalSize (d);
            ++n;
        }

        return n;
    }

    /** Returns the sample position of the first packet, or 0 if empty. */
    int getFirstEventTime() const noexcept
    {
        return data.empty() ? 0 : getEventTime (data.data());
    }

    /** Returns the sample position of the last packet, or 0 if empty. */
    int getLastEventTime() const noexcept
    {
        if (data.empty())
            return 0;

        auto* d   = data.data();
        auto* end = d + data.size();

        for (;;)
        {
            auto* next = d + getEventTotalSize (d);

            if (next >= end)
                return getEventTime (d);

            d = next;
        }
    }

    //==============================================================================
    /** Exchanges the contents of this buffer with another.

        This is a fast pointer-swap suitable for real-time use in a
        double-buffering arrangement.
    */
    void swapWith (UMPBuffer& other) noexcept
    {
        data.swap (other.data);
    }

    /** Pre-allocates storage to avoid allocation during processing.

        @param numWords  the expected maximum number of uint32_t words
                         needed across all events (including timestamps)
    */
    void ensureSize (size_t numWords)
    {
        data.reserve (numWords);
    }

    //==============================================================================
    /** Appends all packets to a MidiBuffer, converting them to MIDI 1.0 messages.

        This is a lossy conversion: 32-bit resolution, per-note controllers,
        and MIDI 2.0-only message types are lost. Sample positions are preserved.
    */
    void addToMidiBuffer (MidiBuffer& dest) const
    {
        ump::ToBytestreamConverter converter { 2048 };

        for (const auto& meta : *this)
        {
            const auto samplePos = meta.samplePosition;

            converter.convert (meta.packet, static_cast<double> (samplePos),
                               [&dest, samplePos] (ump::BytesOnGroup v, double)
                               {
                                   dest.addEvent (v.bytes.data(),
                                                  (int) v.bytes.size(),
                                                  samplePos);
                               });
        }
    }

    /** Appends the contents of a MidiBuffer, converting each message to UMP.

        Messages are converted to UMP Channel Voice 1 format.
    */
    void addFromMidiBuffer (const MidiBuffer& input)
    {
        ump::ToUMP1Converter converter;

        for (const auto item : input)
        {
            auto msg = item.getMessage();

            converter.convert ({ {}, msg.asSpan() }, [this, time = item.samplePosition] (const ump::View& view)
            {
                addPacket (view, time);
            });
        }
    }

    //==============================================================================
    /** Forward iterator over the packets in a UMPBuffer. */
    class Iterator
    {
    public:
        using difference_type   = std::ptrdiff_t;
        using value_type        = UMPPacketMetadata;
        using reference         = UMPPacketMetadata;
        using pointer           = const UMPPacketMetadata*;
        using iterator_category = std::forward_iterator_tag;

        Iterator() noexcept = default;

        UMPPacketMetadata operator*() const noexcept
        {
            jassert (ptr < endPtr);
            return { ump::View { ptr + 1 }, getEventTime (ptr) };
        }

        Iterator& operator++() noexcept
        {
            ptr += getEventTotalSize (ptr);
            return *this;
        }

        Iterator operator++ (int) noexcept
        {
            auto copy = *this;
            ++(*this);
            return copy;
        }

        bool operator== (const Iterator& other) const noexcept { return ptr == other.ptr; }
        bool operator!= (const Iterator& other) const noexcept { return ptr != other.ptr; }

    private:
        friend class UMPBuffer;

        Iterator (const uint32_t* p, const uint32_t* e) noexcept
            : ptr (p), endPtr (e) {}

        const uint32_t* ptr    = nullptr;
        const uint32_t* endPtr = nullptr;
    };

    /** Returns a const iterator to the first packet. */
    Iterator begin() const noexcept { return cbegin(); }

    /** Returns a const iterator past the last packet. */
    Iterator end() const noexcept   { return cend(); }
    /** Returns an iterator to the first packet. */
    Iterator cbegin() const noexcept { return { data.data(), data.data() + data.size() }; }

    /** Returns an iterator past the last packet. */
    Iterator cend() const noexcept   { auto* e = data.data() + data.size(); return { e, e }; }
    
    /** Returns the first iterator whose sample position is >= @p samplePosition. */
    Iterator findNextSamplePosition (int samplePosition) const noexcept
    {
        auto* d   = data.data();
        auto* end = d + data.size();

        while (d < end && getEventTime (d) < samplePosition)
            d += getEventTotalSize (d);

        return { d, end };
    }

private:
    //==============================================================================
    std::vector<uint32_t> data;

    /** Reads the sample position from the first word of an event. */
    static int getEventTime (const uint32_t* d) noexcept
    {
        return static_cast<int> (*d);
    }

    /** Returns the number of uint32 words for the UMP packet at (d + 1). */
    static uint32_t getEventPacketSize (const uint32_t* d) noexcept
    {
        return ump::Utils::getNumWordsForMessageType (d[1]);
    }

    /** Returns the total event size: 1 (timestamp) + packet words. */
    static uint32_t getEventTotalSize (const uint32_t* d) noexcept
    {
        return 1 + getEventPacketSize (d);
    }

    /** Walks forward past all events with timestamp <= samplePosition. */
    static uint32_t* findEventAfter (uint32_t* d, uint32_t* endData, int samplePosition) noexcept
    {
        while (d < endData && getEventTime (d) <= samplePosition)
            d += getEventTotalSize (d);

        return d;
    }

    JUCE_LEAK_DETECTOR (UMPBuffer)
};

} // namespace juce
