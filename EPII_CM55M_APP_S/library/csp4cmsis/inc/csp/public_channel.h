#ifndef CSP4CMSIS_PUBLIC_CHANNEL_H
#define CSP4CMSIS_PUBLIC_CHANNEL_H

#include "rendezvous_channel.h"
#include "buffered_channel.h"
#include "sync_channel.h" // Added for Signal support

namespace csp {

// Forward declarations
template <typename T> class Chanin;
template <typename T> class Chanout;

/**
 * @brief Pipe Operators for Alternative Syntax.
 */
template <typename T>
ChannelBinding<T, Chanin<T>> operator|(Chanin<T>& chan, T& dest) {
    return ChannelBinding<T, Chanin<T>>(chan, dest);
}

template <typename T>
ChannelBinding<const T, Chanout<T>> operator|(Chanout<T>& chan, const T& source) {
    return ChannelBinding<const T, Chanout<T>>(chan, source);
}

// =============================================================
// Channel End Wrappers (Chanout / Chanin)
// =============================================================

template <typename T>
class Chanout {
private:
    internal::BaseAltChan<T>* internal_ptr;
public:
    Chanout(internal::BaseAltChan<T>* ptr) : internal_ptr(ptr) {}
    
    // Write (Blocks if policy is Block, Samples if policy is KeepNewest/Oldest)
    void operator<<(const T& data) { internal_ptr->output(&data); }
    void write(const T& data) { internal_ptr->output(&data); }
    
    /**
     * @brief Non-blocking write from an Interrupt Service Routine.
     */
    bool putFromISR(const T& data) { 
        return internal_ptr->putFromISR(data); 
    }
    
    internal::Guard* getGuard(const T& source) { 
        return internal_ptr->getOutputGuard(source); 
    }
};

template <typename T>
class Chanin {
private:
    internal::BaseAltChan<T>* internal_ptr;
public:
    Chanin(internal::BaseAltChan<T>* ptr) : internal_ptr(ptr) {}
    
    // Read (Always blocks until data is available)
    void operator>>(T& dest) { internal_ptr->input(&dest); }
    void read(T& dest) { internal_ptr->input(&dest); }
    
    internal::Guard* getGuard(T& dest) { 
        return internal_ptr->getInputGuard(dest); 
    }
};

// =============================================================
// Static Channel Containers
// =============================================================

/**
 * @brief Zero-capacity Rendezvous Channel.
 * Defaults to Blocking for backward compatibility.
 */
template <typename T, BufferPolicy P = BufferPolicy::Block>
class One2OneChannel {
private:
    internal::RendezvousChannel<T, P> internal_chan;
public:
    One2OneChannel() = default;
    
    Chanout<T> writer() { return Chanout<T>(&internal_chan); }
    Chanin<T> reader() { return Chanin<T>(&internal_chan); }
};

/**
 * @brief Default Alias. One2OneChannel<int> will block by default.
 */
template <typename T>
using Channel = One2OneChannel<T, BufferPolicy::Block>;

/**
 * @brief Buffered Channel with Static Capacity.
 * Defaults to Blocking (standard FIFO behavior).
 */
template <typename T, size_t SIZE, BufferPolicy P = BufferPolicy::Block>
class BufferedOne2OneChannel {
private:
    internal::BufferedChannel<T, P> internal_chan;
public:
    BufferedOne2OneChannel() : internal_chan(SIZE) {}
    
    Chanout<T> writer() { return Chanout<T>(&internal_chan); }
    Chanin<T> reader() { return Chanin<T>(&internal_chan); }
};

/**
 * @brief Synchronous Signal Channel (void data).
 */
template <BufferPolicy P = BufferPolicy::Block>
class SignalChannel {
private:
    internal::SyncChannel<P> internal_chan;
public:
    SignalChannel() = default;
    // Signal channels use specialized wrappers or direct internal access
    internal::SyncChannel<P>* getInternal() { return &internal_chan; }
};

// --- Standard CSP Aliases ---
template <typename T, BufferPolicy P = BufferPolicy::Block> 
using Any2OneChannel = One2OneChannel<T, P>;

template <typename T, size_t S, BufferPolicy P = BufferPolicy::Block> 
using BufferedAny2OneChannel = BufferedOne2OneChannel<T, S, P>;

} // namespace csp

#endif // CSP4CMSIS_PUBLIC_CHANNEL_H