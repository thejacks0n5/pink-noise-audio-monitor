#pragma once
/*
  ==============================================================================
    MeasurementQueue.h
    Pink Noise Audio Monitor (pnam) — lock-free SPSC queues.

    A single-producer / single-consumer wait-free queue built on
    juce::AbstractFifo over a preallocated array of trivially-copyable records.

    Contract:
      - Exactly ONE producer thread (the audio thread) calls push().
      - Exactly ONE consumer thread (the message thread, from a juce::Timer)
        calls popAll() / pop() / getNumReady().
      - All storage is allocated once in the constructor. push() and pop never
        allocate, lock, or block — safe for real-time use.
      - push() returns false when the queue is full; the producer must simply
        drop the record (the consumer's TotalPushFailures counter lets the
        message thread log a LoggingError event if drops occur).

    This header is fully implemented inline (template): no .cpp required.
  ==============================================================================
*/

#include <atomic>
#include <cstdint>
#include <type_traits>
#include <vector>
#include <juce_core/juce_core.h>

#include "../Model/DataModel.h"

namespace pnam
{

//==============================================================================
/**
    Lock-free single-producer/single-consumer FIFO of POD records.

    @tparam T  a trivially-copyable record type (pnam::Measurement, pnam::Event).
*/
template <typename T>
class SpscQueue
{
public:
    static_assert (std::is_trivially_copyable<T>::value,
                   "SpscQueue<T> requires a trivially copyable T");

    /** Preallocates storage for `capacity` records. Choose a capacity large
        enough to absorb the worst-case UI-timer stall (e.g. 256 for
        measurements at 1 Hz, 1024 for events). Call from a non-real-time
        thread (construction allocates). */
    explicit SpscQueue (int capacity)
        : fifo (juce::jmax (2, capacity)),
          storage ((size_t) juce::jmax (2, capacity))
    {
    }

    SpscQueue (const SpscQueue&) = delete;
    SpscQueue& operator= (const SpscQueue&) = delete;

    //==========================================================================
    /** AUDIO THREAD. Copies one record into the queue.
        @returns false (and increments the failure counter) if the queue was
                 full; the record is dropped. Never blocks or allocates. */
    bool push (const T& item) noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite (1, start1, size1, start2, size2);

        if (size1 + size2 < 1)
        {
            pushFailures.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        storage[(size_t) start1] = item;
        fifo.finishedWrite (1);
        return true;
    }

    //==========================================================================
    /** MESSAGE THREAD. Pops every currently-ready record, invoking
        `callback (const T&)` for each, in FIFO order.
        @returns the number of records consumed. */
    template <typename Callback>
    int popAll (Callback&& callback)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (fifo.getNumReady(), start1, size1, start2, size2);

        for (int i = 0; i < size1; ++i)
            callback (static_cast<const T&> (storage[(size_t) (start1 + i)]));

        for (int i = 0; i < size2; ++i)
            callback (static_cast<const T&> (storage[(size_t) (start2 + i)]));

        const int consumed = size1 + size2;
        fifo.finishedRead (consumed);
        return consumed;
    }

    /** MESSAGE THREAD. Pops a single record into `out`.
        @returns false if the queue was empty. */
    bool pop (T& out)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (1, start1, size1, start2, size2);

        if (size1 + size2 < 1)
            return false;

        out = storage[(size_t) (size1 > 0 ? start1 : start2)];
        fifo.finishedRead (1);
        return true;
    }

    //==========================================================================
    /** Number of records ready to read (approximate across threads). */
    int getNumReady() const noexcept        { return fifo.getNumReady(); }

    /** Total capacity in records. */
    int getCapacity() const noexcept        { return fifo.getTotalSize(); }

    /** Running count of push() calls dropped because the queue was full.
        Read on the message thread to detect overload/stalls. */
    uint64_t getTotalPushFailures() const noexcept
    {
        return pushFailures.load (std::memory_order_relaxed);
    }

    /** MESSAGE THREAD ONLY, and only while the producer is stopped. */
    void reset()
    {
        fifo.reset();
        pushFailures.store (0, std::memory_order_relaxed);
    }

private:
    juce::AbstractFifo    fifo;
    std::vector<T>        storage;
    std::atomic<uint64_t> pushFailures { 0 };
};

//==============================================================================
/** Audio thread -> message thread queue of measurement snapshots. */
using MeasurementQueue = SpscQueue<Measurement>;

/** Audio thread -> message thread queue of detection events. */
using EventQueue = SpscQueue<Event>;

/** Recommended default capacities (records). */
constexpr int defaultMeasurementQueueCapacity = 256;
constexpr int defaultEventQueueCapacity       = 1024;

} // namespace pnam
