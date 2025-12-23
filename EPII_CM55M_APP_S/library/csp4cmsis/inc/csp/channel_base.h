// --- channel_base.h (HARD SYNCHRONIZATION ONLY) ---
#ifndef CSP4CMSIS_CHANNEL_BASE_H
#define CSP4CMSIS_CHANNEL_BASE_H

#include <stddef.h> 
// Note: TickType_t is no longer strictly required here, but harmless.

namespace csp {
    // These forward declarations are REQUIRED for friend class declarations below.
    template <typename T> class Chanin;
    template <typename T> class Chanout;
    class Alternative; 
}

namespace csp::internal {

    // FORWARD DECLARATION for the ALT Guard object
    class Guard; 

    /**
     * @brief The core contract for a CSP communication channel in the SPN model.
     * It defines the pure virtual methods for BLOCKING I/O ONLY.
     */
    template <typename DATA_TYPE>
    class BaseChan 
    {
    public:
        // Friend declarations allow the user-facing wrappers access to protected I/O.
        template <typename U>
        friend class csp::Chanin; 

        template <typename U>
        friend class csp::Chanout;
        
    protected:
        inline virtual ~BaseChan() = default;

        // Core Blocking I/O (The only required I/O contract)
        virtual void input(DATA_TYPE* const dest) = 0;
        virtual void output(const DATA_TYPE* const source) = 0;
        
        // Extended I/O (Required stubs, e.g., for direct DMA/Buffer access)
        virtual void beginExtInput(DATA_TYPE* const dest) = 0;
        virtual void endExtInput() = 0;
        
    // --- TIMED I/O REMOVED ---
    /*
    public:
        virtual bool output_with_timeout(const DATA_TYPE* const source, TickType_t timeout) = 0;
        virtual bool input_with_timeout(DATA_TYPE* const dest, TickType_t timeout) = 0;
    */
    }; 
    
    /**
     * @brief Extends BaseChan with methods required for the Alternative (ALT) primitive.
     */
    template <typename DATA_TYPE>
    class BaseAltChan : public BaseChan<DATA_TYPE>
    {
    public:
        /**
         * @brief Configures and returns the resident Input Guard.
         * Instead of 'new', this returns a pointer to a member variable inside the channel.
         */
        virtual internal::Guard* getInputGuard(DATA_TYPE& dest) = 0;
        
        /**
         * @brief Configures and returns the resident Output Guard.
         */
        virtual internal::Guard* getOutputGuard(const DATA_TYPE& source) = 0;
        
    public:
        inline virtual ~BaseAltChan() = default;

        // Alternative needs access to call these to set up the selection process.
        friend class ::csp::Alternative; 
    };

    // =============================================================
    // BaseAltChan (FULL SPECIALIZATION for void - Synchronization Only)
    // =============================================================
    template <>
    class BaseAltChan<void> : public BaseChan<void> {
    public:
        // No data destination needed for void/sync signals.
        virtual Guard* getInputGuard() = 0;
        virtual Guard* getOutputGuard() = 0;
    };

} // namespace csp::internal

#endif // CSP4CMSIS_CHANNEL_BASE_H
