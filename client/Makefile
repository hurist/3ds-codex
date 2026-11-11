ifeq ($(strip $(DEVKITARM)),)
$(error Set DEVKITARM to your devkitARM installation)
endif

include $(DEVKITARM)/3ds_rules

TARGET := 3ds-codex
BUILD := build

ifeq ($(notdir $(CURDIR)),$(BUILD))

VPATH := ../source
DEPSDIR := .
LD := $(CC)
OFILES := main.o composer.o pinyin.o display.o ui.o network.o pictures.o input_form.o selection.o voice.o pairing.o glyphs.o
CFLAGS := -g -Wall -Wextra -Werror -O2 -std=gnu11 -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -I$(CTRULIB)/include -I$(PORTLIBS)/include
LDFLAGS := -specs=3dsx.specs -g -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -L$(CTRULIB)/lib -L$(PORTLIBS)/lib
LIBS := -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lm

../$(TARGET).3dsx: ../$(TARGET).elf
../$(TARGET).elf: $(OFILES)

-include $(OFILES:.o=.d)

glyphs.o: ../assets/glyphs.bin ../assets/pinyin.bin

else

.PHONY: all cia clean
all:
	@mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f ../Makefile

cia: all
	@$(MAKE) --no-print-directory -C cia

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).3dsx $(TARGET).cia

endif
