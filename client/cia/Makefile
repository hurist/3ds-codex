MAKEROM ?= makerom
BANNERTOOL ?= bannertool
PYTHON ?= python3
BUILD := ../build

.PHONY: all
all: ../3ds-codex.cia

../3ds-codex.cia: ../3ds-codex.elf Makefile title.rsf $(BUILD)/cia-icon.smdh $(BUILD)/cia-banner.bnr
	"$(MAKEROM)" -f cia -target t -desc app:4 -rsf title.rsf \
	    -elf $< -icon $(BUILD)/cia-icon.smdh -banner $(BUILD)/cia-banner.bnr -o $@

$(BUILD)/cia-icon.smdh: icon.png | $(BUILD)
	"$(BANNERTOOL)" makesmdh -s "3DS Codex" -l "掌机 Codex 客户端" -p "3DS Codex" \
	    -i $< -o $@ -r regionfree -f visible

$(BUILD)/cia-banner.bnr: banner.png $(BUILD)/cia-silence.wav
	"$(BANNERTOOL)" makebanner -i $< -a $(BUILD)/cia-silence.wav -o $@

$(BUILD)/cia-silence.wav: | $(BUILD)
	$(PYTHON) -c 'import wave; f = wave.open("$@", "wb"); f.setparams((1, 2, 32000, 0, "NONE", "")); f.writeframes(bytes(6400)); f.close()'

$(BUILD):
	mkdir -p $@
