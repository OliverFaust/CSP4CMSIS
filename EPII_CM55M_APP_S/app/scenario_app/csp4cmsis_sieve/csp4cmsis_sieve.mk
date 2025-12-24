# -------------------------------------------------------------------------
# 1. APPLICATION IDENTITY & FOLDER PATHS
# -------------------------------------------------------------------------
override SCENARIO_APP_SUPPORT_LIST := $(APP_TYPE)

# Use the actual folder name (matching the rename to 'cmsis')
CURR_PROJ_DIR := ./app/scenario_app/csp4cmsis_sieve

# -------------------------------------------------------------------------
# 2. SYSTEM & ARCHITECTURE OVERRIDES (The "Linker Fixes")
# -------------------------------------------------------------------------
# Disable TrustZone
override TRUSTZONE      := n
override TRUSTZONE_TYPE := non-security
override TRUSTZONE_FW_TYPE := 0

# Disable MPU (Fixes the MPU_xTaskResumeAll error)
override MPU := n

# Force Non-TrustZone FreeRTOS
override OS_SEL := freertos
override EPII_USECASE_SEL := drv_user_defined

# -------------------------------------------------------------------------
# 3. GLOBAL INCLUDE PATHS (The "Fatal Error" Fix)
# -------------------------------------------------------------------------
# We override INCDIR to ensure core files like app/main.c see your headers
override INCDIR += $(CURR_PROJ_DIR) \
                   library/csp4cmsis/inc \
                   library/csp4cmsis/inc/csp \
                   os/freertos/NTZ/freertos_kernel/include \
                   os/freertos/NTZ/freertos_kernel/portable/GCC/ARM_CM55_NTZ/non_secure

# -------------------------------------------------------------------------
# 4. COMPILER DEFINES
# -------------------------------------------------------------------------
override APPL_DEFINES += -DCSP4CMSIS_SIEVE
override APPL_DEFINES += -DconfigENABLE_MPU=0
override APPL_DEFINES += -DconfigENABLE_TRUSTZONE=0

# -------------------------------------------------------------------------
# 5. SOURCE FILES (C and C++)
# -------------------------------------------------------------------------
# Collect all local C++ files and library C++ files
LOCAL_CXX_SOURCES = $(wildcard $(CURR_PROJ_DIR)/*.cpp)
LIB_CXX_SOURCES   = $(wildcard ./library/csp4cmsis/src/*.cpp)

# Register C++ files with the SDK build system
override SCENARIO_APP_CXXSRCS += $(LOCAL_CXX_SOURCES) $(LIB_CXX_SOURCES)

# Add FreeRTOS Kernel C sources (Non-TrustZone paths)
RTOS_PATH = ./os/freertos/NTZ/freertos_kernel
APPL_CSRCS += $(RTOS_PATH)/tasks.c \
              $(RTOS_PATH)/queue.c \
              $(RTOS_PATH)/timers.c \
              $(RTOS_PATH)/list.c \
              $(RTOS_PATH)/portable/MemMang/heap_4.c

# -------------------------------------------------------------------------
# 6. LINKER & LIBRARIES
# -------------------------------------------------------------------------
APPL_LIBS += -lm -lstdc++ -lc

ifeq ($(strip $(TOOLCHAIN)), arm)
override LINKER_SCRIPT_FILE := $(CURR_PROJ_DIR)/csp4cmsis_sieve.sct
else
override LINKER_SCRIPT_FILE := $(CURR_PROJ_DIR)/csp4cmsis_sieve.ld
endif
