ENV ?= esp32-s3-standard
VERSION ?= $(shell git describe --tags --always --dirty)
PORT ?= 8000

BUILD_DIR := .pio/build/$(ENV)
SITE_DIR := $(BUILD_DIR)/web-installer
IMAGES := $(addprefix $(BUILD_DIR)/,bootloader.bin partitions.bin firmware.bin)
# Empty otadata, so a board left on app1 by OTA updates boots the new image.
BOOT_APP0 = $(firstword $(shell find $(or $(PLATFORMIO_CORE_DIR),$(HOME)/.platformio)/packages -path '*/tools/partitions/boot_app0.bin' 2>/dev/null))

.DEFAULT_GOAL := help
.PHONY: help build test site serve

help: ## Show this help
	@echo "Usage: make <target> [VAR=value]"
	@echo
	@echo "Targets:"
	@awk 'BEGIN { FS = ":.*## " } /^[a-z]+:.*## / { printf "  %-8s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@echo
	@echo "Variables:"
	@echo "  ENV      PlatformIO environment ($(ENV))"
	@echo "  VERSION  Firmware version ($(VERSION))"
	@echo "  PORT     Web installer port ($(PORT))"
	@echo
	@test -f config.json \
		&& echo "Builds include the settings in config.json." \
		|| echo "To build settings into the firmware, copy config.example.json to config.json."

build: ## Build the firmware (OTA image: .pio/build/<ENV>/firmware.bin)
	PLATFORMIO_BUILD_FLAGS='-DFIRMWARE_VERSION=\"$(VERSION)\"' pio run -e $(ENV)

# platformio.ini pulls in MinGW for Windows hosts; elsewhere the system compiler builds the tests.
test: ## Run the unit tests on this computer
	@mkdir -p .pio
	sed '/toolchain-gccmingw32/d' platformio.ini > .pio/native.ini
	pio test -e native -c .pio/native.ini

site: build ## Build the web installer into .pio/build/<ENV>/web-installer
	@test -n "$(BOOT_APP0)" || { echo "boot_app0.bin not found in the PlatformIO packages" >&2; exit 1; }
	mkdir -p $(SITE_DIR)
	cp web-installer/* $(IMAGES) "$(BOOT_APP0)" $(SITE_DIR)/
	sed -E 's/("version": *)"[^"]*"/\1"$(VERSION)"/' web-installer/manifest.json > $(SITE_DIR)/manifest.json

# Web Serial only works on localhost or HTTPS.
serve: site ## Serve the web installer on http://localhost:<PORT> to flash over USB
	@echo "Open http://localhost:$(PORT) in Chrome or Edge"
	python3 -m http.server $(PORT) --bind 127.0.0.1 --directory $(SITE_DIR)
