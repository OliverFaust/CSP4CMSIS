# library/csp4cmsis/csp4cmsis.mk

CSP4CMSIS_LIB_DIR = $(LIBRARIES_ROOT)/csp4cmsis

# 1. Provide the root inc for app-level #include "csp/csp4cmsis.h"
# 2. Provide the subfolder for internal #include "alt_channel_sync.h"
LIB_INCDIR += $(CSP4CMSIS_LIB_DIR)/inc
LIB_INCDIR += $(CSP4CMSIS_LIB_DIR)/inc/csp

# 3. Register the sources
LIB_CXXSRCDIR += $(CSP4CMSIS_LIB_DIR)/src
LIB_CXXSRCS += $(wildcard $(CSP4CMSIS_LIB_DIR)/src/*.cpp)

# 4. Inject into global paths to ensure the scenario app can see them
override INCLUDE_PATHS += $(CSP4CMSIS_LIB_DIR)/inc \
                          $(CSP4CMSIS_LIB_DIR)/inc/csp
