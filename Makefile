# Clawdmeter — build / flash / daemon / device control
#
# Overridable on the command line:
#   ENV   PlatformIO board env   (default: waveshare_amoled_216)
#   PORT  serial device          (default: first usbmodem on macOS, ttyACM0 on Linux)
#   OUT   screenshot output file  (default: screenshot.png)
#
# Examples:
#   make build ENV=waveshare_amoled_18
#   make flash PORT=/dev/cu.usbmodem101
#   make reset-device FACTORY=1
#   make buddy-seed SEED=sparky

ENV  ?= waveshare_amoled_216
OUT  ?= screenshot.png
SHELL := /bin/bash

# pio lives on PATH for brew installs, else inside the penv.
PIO := $(shell command -v pio 2>/dev/null || echo $(HOME)/.platformio/penv/bin/pio)

# Daemon python: prefer the daemon venv created by the installer.
DAEMON_PY := $(if $(wildcard daemon/.venv/bin/python),daemon/.venv/bin/python,python3)

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  PORT ?= $(shell ls /dev/cu.usbmodem* 2>/dev/null | head -1)
  STTY  = stty -f "$(PORT)" 115200
  PLIST = $(HOME)/Library/LaunchAgents/com.user.claude-usage-daemon.plist
else
  PORT ?= /dev/ttyACM0
  STTY  = stty -F "$(PORT)" 115200
endif

# Send a one-line command to the device's serial console.
define serial_send
	@test -n "$(PORT)" || { echo "No serial PORT found — set PORT=/dev/..."; exit 1; }
	@test -e "$(PORT)" || { echo "Port $(PORT) not found — is the device plugged in?"; exit 1; }
	@$(STTY) 2>/dev/null || true
	@printf '%s\n' "$(1)" > "$(PORT)"
	@echo "Sent '$(1)' -> $(PORT)"
endef

.DEFAULT_GOAL := help

.PHONY: help build flash monitor screenshot \
        daemon-install daemon-start daemon-stop daemon-restart daemon-logs daemon-uninstall \
        reset-device bonding-on bonding-off \
        buddy-show buddy-seed buddy-roll buddy-reset \
        hooks-install hooks-uninstall fonts

help:
	@echo "Clawdmeter targets (ENV=$(ENV)  PORT=$(PORT)):"
	@echo "  Firmware:  build  flash  monitor  screenshot [OUT=...]"
	@echo "  Daemon:    daemon-install  daemon-{start,stop,restart,logs,uninstall}"
	@echo "  Device:    reset-device [FACTORY=1]  bonding-on  bonding-off"
	@echo "  Buddy:     buddy-show  buddy-seed SEED=...  buddy-roll  buddy-reset"
	@echo "  Hooks:     hooks-install  hooks-uninstall   (Phase 2)"
	@echo "  Fonts:     fonts   (regenerate the Mono LVGL fonts)"

# ── Firmware ────────────────────────────────────────────────────────────────
build:
	$(PIO) run -d firmware -e $(ENV)

flash:
	$(PIO) run -d firmware -e $(ENV) -t upload --upload-port $(PORT)

monitor:
	$(PIO) device monitor -p $(PORT) -b 115200

screenshot:
	./screenshot.sh "$(OUT)" "$(PORT)"

# ── Daemon (OS-aware: launchd on macOS, systemd --user on Linux) ─────────────
daemon-install:
ifeq ($(UNAME),Darwin)
	./install-mac.sh
else
	./install.sh
endif

daemon-start:
ifeq ($(UNAME),Darwin)
	launchctl load -w "$(PLIST)"
else
	systemctl --user start claude-usage-daemon
endif

daemon-stop:
ifeq ($(UNAME),Darwin)
	launchctl unload "$(PLIST)"
else
	systemctl --user stop claude-usage-daemon
endif

daemon-restart:
ifeq ($(UNAME),Darwin)
	launchctl unload "$(PLIST)" 2>/dev/null || true; launchctl load -w "$(PLIST)"
else
	systemctl --user restart claude-usage-daemon
endif

daemon-logs:
ifeq ($(UNAME),Darwin)
	tail -F "$(HOME)/Library/Logs/claude-usage-daemon.out.log"
else
	journalctl --user -u claude-usage-daemon -f
endif

daemon-uninstall:
ifeq ($(UNAME),Darwin)
	launchctl unload "$(PLIST)" 2>/dev/null || true; rm -f "$(PLIST)"
	@echo "Removed $(PLIST)"
else
	systemctl --user disable --now claude-usage-daemon 2>/dev/null || true
	rm -f "$(HOME)/.config/systemd/user/claude-usage-daemon.service"
	systemctl --user daemon-reload
endif

# ── Device control (serial commands; see main.cpp handle_serial_cmd) ─────────
reset-device:
	$(call serial_send,$(if $(FACTORY),reset --factory,reset))

bonding-on:
	$(call serial_send,bonding on)

bonding-off:
	$(call serial_send,bonding off)

# ── Buddy identity (host-side; see daemon/buddy.py) ──────────────────────────
BUDDY_TOML := $(HOME)/.config/claude-usage-monitor/buddy.toml
BUDDY_JSON := $(HOME)/.config/claude-usage-monitor/buddy.json

buddy-show:
	@$(DAEMON_PY) daemon/buddy.py

# Pin a deterministic seed (preserves any [overrides] already in the file).
buddy-seed:
	@test -n "$(SEED)" || { echo "Usage: make buddy-seed SEED=<string>"; exit 1; }
	@mkdir -p "$(dir $(BUDDY_TOML))"
	@$(DAEMON_PY) -c "import re,pathlib; p=pathlib.Path('$(BUDDY_TOML)'); t=(p.read_text() if p.exists() else ''); t=re.sub(r'(?m)^\s*seed\s*=.*\n','',t); p.write_text('seed = \"$(SEED)\"\n'+t); print('seed set to $(SEED) in '+str(p))"
	@echo "Run 'make buddy-show' to preview, then 'make daemon-restart'."

# Re-roll from a fresh random seed.
buddy-roll:
	@$(MAKE) buddy-seed SEED=$$($(DAEMON_PY) -c "import secrets;print(secrets.token_hex(8))")

# Back to the account-derived buddy: drop the seed and the cached state.
buddy-reset:
	@if [ -f "$(BUDDY_TOML)" ]; then $(DAEMON_PY) -c "import re,pathlib;p=pathlib.Path('$(BUDDY_TOML)');p.write_text(re.sub(r'(?m)^\s*seed\s*=.*\n','',p.read_text()))"; fi
	@rm -f "$(BUDDY_JSON)"
	@echo "Buddy reset to account identity (removed seed + $(BUDDY_JSON))."
	@echo "Run 'make daemon-restart' to apply."

# ── Hooks (Phase 2) ──────────────────────────────────────────────────────────
hooks-install hooks-uninstall:
	@echo "Hooks are a Phase 2 feature and not yet wired."
	@echo "See .ai/plans/claude-buddy-on-device.md (Feature 6)."

# ── Fonts ────────────────────────────────────────────────────────────────────
# Regenerate the Mono LVGL fonts from assets/DejaVuSansMono.ttf, including the
# buddy glyphs (eyes/stars/blocks). Patches lv_font_conv output for LVGL 9.
MONO_RANGES := 0x20-0x7E,0xB0,0xB7,0xD7,0x3C9,0x2026,0x2248,0x2588,0x2591,0x2605,0x2722,0x2726,0x2733,0x2736,0x273B,0x273D,0x25C9,0x25B2,0x25BC
fonts:
	@for sz in 18 32; do \
	  npx -y lv_font_conv@1.5.3 --font assets/DejaVuSansMono.ttf -r "$(MONO_RANGES)" \
	    --size $$sz --format lvgl --bpp 4 --no-compress -o /tmp/font_mono_$$sz.c --lv-include lvgl.h && \
	  python3 tools/lv_font_patch.py /tmp/font_mono_$$sz.c firmware/src/font_mono_$$sz.c && \
	  echo "regenerated firmware/src/font_mono_$$sz.c"; \
	done
