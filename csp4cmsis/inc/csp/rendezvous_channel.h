#ifndef CSP4CMSIS_RENDEZVOUS_CHANNEL_H
#define CSP4CMSIS_RENDEZVOUS_CHANNEL_H

#include "channel_base.h"
#include "alt_channel_sync.h"
#include <type_traits>

namespace csp::internal {

/**
 * @brief Zero-capacity synchronous channel (rendezvous), OWRV protocol.
 * The logic lives in the non-template RendezvousCore (alt_channel_sync.h).
 */
template <typename T, csp::BufferPolicy P = csp::BufferPolicy::Block>
class RendezvousChannel : public BaseAltChan<T> {
    static_assert(std::is_trivially_copyable_v<T>,
                  "RendezvousChannel: T must be trivially copyable (elements are copied with memcpy)");
private:
    RendezvousCore core_{sizeof(T)};

public:
    RendezvousChannel() = default;
    ~RendezvousChannel() override = default;
    RendezvousChannel(const RendezvousChannel&) = delete;
    RendezvousChannel& operator=(const RendezvousChannel&) = delete;

    bool space_available() override {
        if constexpr (P != csp::BufferPolicy::Block) return true;
        return core_.space_available();
    }
    bool pending() override { return core_.pending(); }

    void input(T* const dest) override { core_.input(dest); }

    void output(const T* const source) override {
        if constexpr (P != csp::BufferPolicy::Block) {
            (void)core_.offer(source);        // sampling: only to a reader already waiting
        } else {
            core_.output(source);
        }
    }

    /// Delivers only to a plain reader that is already waiting.
    bool putFromISR(const T& data) override {
        bool ok = core_.offer(&data);
        if constexpr (P != csp::BufferPolicy::Block) return true;   // dropped counts as handled
        return ok;
    }

    internal::Guard* getInputGuard(GuardSlot& slot, T& dest) override {
        return slot.emplace<ChanInGuard>(&core_, static_cast<void*>(&dest));
    }
    internal::Guard* getOutputGuard(GuardSlot& slot, const T& source) override {
        return slot.emplace<ChanOutGuard>(&core_, static_cast<const void*>(&source));
    }

    void beginExtInput(T* const /*dest*/) override {}
    void endExtInput() override {}
};

} // namespace csp::internal

#endif
