ENV ?= esp32-s3-standard
VERSION ?= $(shell git describe --tags --always --dirty)
PORT ?= 8000

BUILD_DIR := .pio/build/$(ENV)
SITE_DIR := $(BUILD_DIR)/web-installer
IMAGES := $(addprefix $(BUILD_DIR)/,bootloader.bin partitions.bin firmware.bin)
# Empty otadata, so a board left on app1 by OTA updates boots the new image.
BOOT_APP0 = $(firstword $(shell find $(or $(PLATFORMIO_CORE_DIR),$(HOME)/.platformio)/packages -path '*/tools/partitions/boot_app0.bin' 2>/dev/null))

.DEFAULT_GOAL := serve
.PHONY: build site serve

build:
	PLATFORMIO_BUILD_FLAGS='-DFIRMWARE_VERSION=\"$(VERSION)\"' pio run -e $(ENV)

site: build
	@test -n "$(BOOT_APP0)" || { echo "boot_app0.bin not found in the PlatformIO packages" >&2; exit 1; }
	mkdir -p $(SITE_DIR)
	cp web-installer/* $(IMAGES) "$(BOOT_APP0)" $(SITE_DIR)/
	sed -E 's/("version": *)"[^"]*"/\1"$(VERSION)"/' web-installer/manifest.json > $(SITE_DIR)/manifest.json

# Web Serial only works on localhost or HTTPS.
serve: site
	@echo "Open http://localhost:$(PORT) in Chrome or Edge"
	python3 -m http.server $(PORT) --bind 127.0.0.1 --directory $(SITE_DIR)
