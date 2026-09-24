// --- glue.cpp ---
//
// Previously overrode global operator new/delete to route through
// FreeRTOS's pvPortMalloc/vPortFree. Removed: investigation found zero
// live allocation need anywhere in csp4cmsis itself or in the reference
// ALT/select test used during its own RTOS2/RTX5 validation -- the only
// real dependency found was in a downstream project's own inference
// pipeline (a std::vector<EValue> allocated once per cycle), which is
// that project's own implementation detail, not something shared
// library code should be dictating heap behavior for. A project with a
// genuine allocation need should supply its own local operator
// new/delete (or equivalent) rather than relying on this file.
