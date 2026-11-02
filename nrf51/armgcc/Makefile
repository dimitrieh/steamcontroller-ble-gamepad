# Steam Controller BLE gamepad -- nRF51822 radio application.
#
# Requires nRF5 SDK 10.0.0 (the last SDK supporting S110 8.0.0, which is the
# SoftDevice already on the controller) and arm-none-eabi-gcc.
#
#   make NRF_SDK=/path/to/nRF5_SDK_10.0.0

PROJECT_NAME := sc_ble_gamepad

NRF_SDK ?= $(abspath ../../../nRF5_SDK_10.0.0)

OUTPUT_DIR := _build
SRC_DIR    := ../src
CFG_DIR    := ../config

CC      := arm-none-eabi-gcc
OBJCOPY := arm-none-eabi-objcopy
SIZE    := arm-none-eabi-size

SDK_COMPONENTS := $(NRF_SDK)/components

# ---- Sources ---------------------------------------------------------------

C_SRC := \
  $(SRC_DIR)/main.c \
  $(SRC_DIR)/gamepad_hid.c \
  $(SRC_DIR)/sc_state.c \
  $(SRC_DIR)/vframe.c \
  $(SRC_DIR)/uart_link.c \
  $(SRC_DIR)/diag.c \
  $(SDK_COMPONENTS)/libraries/util/app_error.c \
  $(SDK_COMPONENTS)/libraries/util/nrf_assert.c \
  $(SDK_COMPONENTS)/libraries/timer/app_timer.c \
  $(SDK_COMPONENTS)/drivers_nrf/pstorage/pstorage.c \
  $(SDK_COMPONENTS)/ble/device_manager/device_manager_peripheral.c \
  $(SDK_COMPONENTS)/ble/common/ble_advdata.c \
  $(SDK_COMPONENTS)/ble/common/ble_conn_params.c \
  $(SDK_COMPONENTS)/ble/common/ble_srv_common.c \
  $(SDK_COMPONENTS)/ble/ble_services/ble_bas/ble_bas.c \
  $(SDK_COMPONENTS)/ble/ble_services/ble_dis/ble_dis.c \
  $(SDK_COMPONENTS)/ble/ble_services/ble_hids/ble_hids.c \
  $(SDK_COMPONENTS)/toolchain/system_nrf51.c \
  $(SDK_COMPONENTS)/softdevice/common/softdevice_handler/softdevice_handler.c

ASM_SRC := $(SDK_COMPONENTS)/toolchain/gcc/gcc_startup_nrf51.s

# ---- Includes --------------------------------------------------------------

INC := \
  -I$(CFG_DIR) \
  -I$(SRC_DIR) \
  -I$(SDK_COMPONENTS)/device \
  -I$(SDK_COMPONENTS)/toolchain \
  -I$(SDK_COMPONENTS)/toolchain/gcc \
  -I$(SDK_COMPONENTS)/libraries/util \
  -I$(SDK_COMPONENTS)/libraries/timer \
  -I$(SDK_COMPONENTS)/libraries/trace \
  -I$(SDK_COMPONENTS)/drivers_nrf/hal \
  -I$(SDK_COMPONENTS)/drivers_nrf/delay \
  -I$(SDK_COMPONENTS)/drivers_nrf/common \
  -I$(SDK_COMPONENTS)/drivers_nrf/config \
  -I$(SDK_COMPONENTS)/drivers_nrf/pstorage \
  -I$(SDK_COMPONENTS)/ble/device_manager \
  -I$(SDK_COMPONENTS)/ble/common \
  -I$(SDK_COMPONENTS)/ble/ble_services/ble_bas \
  -I$(SDK_COMPONENTS)/ble/ble_services/ble_dis \
  -I$(SDK_COMPONENTS)/ble/ble_services/ble_hids \
  -I$(SDK_COMPONENTS)/softdevice/common/softdevice_handler \
  -I$(SDK_COMPONENTS)/softdevice/s110/headers

# ---- Flags -----------------------------------------------------------------

# BLE_STACK_SUPPORT_REQD routes app_timer and friends through the SoftDevice.
# S110 / SOFTDEVICE_PRESENT pick the right SoftDevice headers.
# Answered to the LPC's version query, which is how the flash tool's "Radio"
# line identifies the running image. Fixed per make run; override to reproduce.
# Simply expanded, and only once: a recursive ?= would call date again at each
# use, and build.json could then disagree with the time compiled into main.o.
ifndef BUILD_UNIX_TIME
BUILD_UNIX_TIME := $(shell date +%s)
endif

DEFINES := \
  -DNRF51 \
  -DNRF51822 \
  -DBOARD_CUSTOM \
  -DS110 \
  -DSOFTDEVICE_PRESENT \
  -DBLE_STACK_SUPPORT_REQD \
  -DBUILD_UNIX_TIME=$(BUILD_UNIX_TIME)u


CFLAGS := -mcpu=cortex-m0 -mthumb -mabi=aapcs -mfloat-abi=soft \
          -Os -g3 -ffunction-sections -fdata-sections -fno-strict-aliasing \
          -fno-builtin --short-enums -std=gnu99 \
          -Wall -MMD -MP $(DEFINES) $(INC)

# No heap: nothing here calls malloc, and the 8 kB of application RAM is
# better spent elsewhere. 2 kB of stack, the SDK default.
ASMFLAGS := -mcpu=cortex-m0 -mthumb -x assembler-with-cpp $(DEFINES) \
            -D__HEAP_SIZE=0 -D__STACK_SIZE=2048

LDFLAGS := -mcpu=cortex-m0 -mthumb -mabi=aapcs \
           -L$(SDK_COMPONENTS)/toolchain/gcc \
           -T$(PROJECT_NAME).ld \
           -Wl,--gc-sections -Wl,-Map=$(OUTPUT_DIR)/$(PROJECT_NAME).map \
           --specs=nano.specs

# ---- Rules -----------------------------------------------------------------

OBJ := $(addprefix $(OUTPUT_DIR)/,$(notdir $(C_SRC:.c=.o))) \
       $(addprefix $(OUTPUT_DIR)/,$(notdir $(ASM_SRC:.s=.o)))

vpath %.c $(sort $(dir $(C_SRC)))
vpath %.s $(sort $(dir $(ASM_SRC)))

.PHONY: all clean size

all: $(OUTPUT_DIR)/$(PROJECT_NAME).bin size

$(OUTPUT_DIR):
	@mkdir -p $@

$(OUTPUT_DIR)/%.o: %.c | $(OUTPUT_DIR)
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) -c -o $@ $<

$(OUTPUT_DIR)/%.o: %.s | $(OUTPUT_DIR)
	@echo "  AS   $<"
	@$(CC) $(ASMFLAGS) -c -o $@ $<

$(OUTPUT_DIR)/$(PROJECT_NAME).elf: $(OBJ)
	@echo "  LD   $@"
	@$(CC) $(LDFLAGS) $(OBJ) -lm -o $@

# The flash tool writes a raw image at a base address, so emit a .bin rather
# than a .hex: it is flashed at 0x18000 with the SoftDevice left in place.
#
# Pad to a whole number of words. nRF51 flash is written a word at a time, and
# the SWD bridge in the LPC firmware splits the image into 56-byte chunks, so a
# trailing partial word in the image would produce a partial-word write.
$(OUTPUT_DIR)/$(PROJECT_NAME).bin: $(OUTPUT_DIR)/$(PROJECT_NAME).elf
	@echo "  BIN  $@"
	@$(OBJCOPY) -O binary $< $@
	@pad=$$(( (4 - $$(wc -c < $@) % 4) % 4 )); \
	if [ $$pad -ne 0 ]; then \
	    printf '\377%.0s' $$(seq $$pad) >> $@; \
	    echo "  PAD  $@ (+$$pad bytes to a word boundary)"; \
	fi
	@printf '{"build":"0x%x","unix":%s,"bytes":%s}\n' $(BUILD_UNIX_TIME) $(BUILD_UNIX_TIME) \
	    $$(wc -c < $@) > $(OUTPUT_DIR)/build.json
	@echo "  INFO $(OUTPUT_DIR)/build.json"

size: $(OUTPUT_DIR)/$(PROJECT_NAME).elf
	@$(SIZE) $<

clean:
	@rm -rf $(OUTPUT_DIR)

# main.o carries BUILD_UNIX_TIME, so rebuild it every time: an image whose
# reported build time is older than its code would defeat the point.
$(OUTPUT_DIR)/main.o: FORCE
.PHONY: FORCE
FORCE:

-include $(OBJ:.o=.d)
