# Card Attendance System — standalone firmware build.
#
# STM32CubeIDE can build this project directly; this Makefile exists so the
# same tree builds from a terminal or in CI without the IDE. It uses the
# toolchain CubeIDE ships, falling back to one on PATH.

TARGET  := card-attendance

CUBEIDE_GCC := $(firstword $(wildcard \
  /opt/st/stm32cubeide_*/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/tools/bin))
ifneq ($(CUBEIDE_GCC),)
PREFIX := $(CUBEIDE_GCC)/arm-none-eabi-
else
PREFIX := arm-none-eabi-
endif

CC      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
SIZE    := $(PREFIX)size

BUILD := build

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
C_SOURCES := \
  $(wildcard App/Src/*.c) \
  $(wildcard Bsp/Src/*.c) \
  $(wildcard Core/Src/*.c) \
  $(wildcard Drivers/STM32L4xx_HAL_Driver/Src/*.c) \
  $(wildcard Middlewares/ST/STM32_USB_Device_Library/Core/Src/*.c) \
  $(wildcard Middlewares/ST/STM32_USB_Device_Library/Class/MSC/Src/*.c)

ASM_SOURCES := Core/Startup/startup_stm32l432kcux.s

# ---------------------------------------------------------------------------
# Flags
# ---------------------------------------------------------------------------
CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

DEFS := -DUSE_HAL_DRIVER -DSTM32L432xx

INCS := \
  -IApp/Inc \
  -IBsp/Inc \
  -ICore/Inc \
  -IDrivers/STM32L4xx_HAL_Driver/Inc \
  -IDrivers/STM32L4xx_HAL_Driver/Inc/Legacy \
  -IDrivers/CMSIS/Device/ST/STM32L4xx/Include \
  -IDrivers/CMSIS/Include \
  -IMiddlewares/ST/STM32_USB_Device_Library/Core/Inc \
  -IMiddlewares/ST/STM32_USB_Device_Library/Class/MSC/Inc

# -Og keeps the debugger usable; the design is nowhere near cycle bound.
OPT := -Og

CFLAGS := $(CPU) $(DEFS) $(INCS) $(OPT) -std=gnu11 \
          -Wall -Wextra -ffunction-sections -fdata-sections \
          -g3 -gdwarf-2 -MMD -MP

# App/ is held to a stricter standard than the vendor code it links against:
# it is the part we wrote, and it is the part the host tests also compile.
APP_CFLAGS := -Wconversion -Wsign-conversion -Wshadow -Wundef

ASFLAGS := $(CPU) -x assembler-with-cpp -g3 -gdwarf-2 -MMD -MP

LDSCRIPT := STM32L432KCUX_FLASH.ld
LDFLAGS  := $(CPU) -T$(LDSCRIPT) --specs=nano.specs \
            -Wl,-Map=$(BUILD)/$(TARGET).map,--cref -Wl,--gc-sections \
            -Wl,--print-memory-usage

# ---------------------------------------------------------------------------
# Objects
# ---------------------------------------------------------------------------
OBJECTS := $(addprefix $(BUILD)/,$(C_SOURCES:.c=.o)) \
           $(addprefix $(BUILD)/,$(ASM_SOURCES:.s=.o))

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin

$(BUILD)/App/Src/%.o: App/Src/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $(APP_CFLAGS) $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) -c $(ASFLAGS) $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJECTS) $(LDSCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	@$(SIZE) $@

$(BUILD)/%.hex: $(BUILD)/%.elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OBJCOPY) -O binary -S $< $@

# Run the Level 2 host tests. No cross compiler involved.
test:
	$(MAKE) -C Tests

clean:
	rm -rf $(BUILD)
	$(MAKE) -C Tests clean

.PHONY: all clean test

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
