override SCENARIO_APP_SUPPORT_LIST := $(APP_TYPE)

# -------------------------------------------------------------------------
# 1. APPLICATION DEFINES & INCLUDE PATHS
# -------------------------------------------------------------------------
APPL_DEFINES += -DHELLO_WORLD_FREERTOS_TZ_S_ONLY

# Using -I in APPL_DEFINES is a "forced injection" to ensure it bypasses 
# any SDK variable resets for both C and C++ compilers.
APPL_DEFINES += -I./library/csp4cmsis/inc
APPL_DEFINES += -I$(SCENARIO_APP_ROOT)/$(APP_TYPE)

# Path to the FreeRTOS Kernel
RTOS_INC_DIR = ./os/freertos/NTZ/freertos_kernel

# -------------------------------------------------------------------------
# 2. SOURCE FILES (C and C++)
# -------------------------------------------------------------------------

# Collect all local C++ files (app_init.cpp, tests.cpp)
LOCAL_CXX_SOURCES = $(wildcard $(SCENARIO_APP_ROOT)/$(APP_TYPE)/*.cpp)

# Collect all Library C++ files
LIB_CXX_SOURCES = $(wildcard ./library/csp4cmsis/src/*.cpp)

# CRITICAL: SCENARIO_APP_CXXSRCS is what the Himax SDK uses to trigger 
# the compilation of C++ files into .o files.
override SCENARIO_APP_CXXSRCS += $(LOCAL_CXX_SOURCES) $(LIB_CXX_SOURCES)

# Backup: Also add to generic source variables
override CXX_SOURCES += $(LOCAL_CXX_SOURCES) $(LIB_CXX_SOURCES)
APPL_CXXSRCS         += $(LOCAL_CXX_SOURCES) $(LIB_CXX_SOURCES)

# Standard FreeRTOS C sources
APPL_CSRCS += $(RTOS_INC_DIR)/Source/tasks.c \
              $(RTOS_INC_DIR)/Source/queue.c \
              $(RTOS_INC_DIR)/Source/timers.c \
              $(RTOS_INC_DIR)/Source/list.c

# -------------------------------------------------------------------------
# 3. COMPILER FLAGS
# -------------------------------------------------------------------------
APPL_CFLAGS += -I$(RTOS_INC_DIR)/include \
               -I$(RTOS_INC_DIR)/portable \
               -I$(RTOS_INC_DIR)/portable/GCC/ARM_CM55_NTZ/non_secure \
               -I$(RTOS_INC_DIR)/portable/MemMang

# Ensure the C++ compiler sees the local directory
APPL_CXXFLAGS += -I$(SCENARIO_APP_ROOT)/$(APP_TYPE)

# -------------------------------------------------------------------------
# 4. LINKER CONFIGURATION
# -------------------------------------------------------------------------
OUT_DIR := obj_epii_evb_icv30_bdv10/gnu_epii_evb_WLCSP65
CMSIS_RTOS2_OBJ := $(OUT_DIR)/os/rtos2_freertos/10.5.1/CMSIS/RTOS2/FreeRTOS/Source/cmsis_os2.o

# Force the inclusion of the CMSIS-RTOS2 object and C++ runtime
APPL_LDFLAGS += $(CMSIS_RTOS2_OBJ)
APPL_LIBS += -lm -lstdc++ -lc

# -------------------------------------------------------------------------
# 5. SYSTEM OVERRIDES
# -------------------------------------------------------------------------
override OS_SEL := freertos
override OS_HAL := n
override MPU := n
override TRUSTZONE := n
override TRUSTZONE_TYPE := security
override TRUSTZONE_FW_TYPE := 0
override EPII_USECASE_SEL := drv_user_defined

CIS_SUPPORT_INAPP = cis_sensor
CIS_SUPPORT_INAPP_MODEL = cis_ov5647

ifeq ($(strip $(TOOLCHAIN)), arm)
override LINKER_SCRIPT_FILE := $(SCENARIO_APP_ROOT)/$(APP_TYPE)/hello_world_freertos_tz_s_only.sct
else
override LINKER_SCRIPT_FILE := $(SCENARIO_APP_ROOT)/$(APP_TYPE)/hello_world_freertos_tz_s_only.ld
endif
