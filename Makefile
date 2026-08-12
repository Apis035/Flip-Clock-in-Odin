CFLAGS ?= -O2 -Wall -Wextra
LDLIBS_SYS := -lm -lpthread -ldl -lrt

RAYLIB_VERSION := 5.5
VENDOR := vendor/raylib-$(RAYLIB_VERSION)_linux_amd64
TARBALL := raylib-$(RAYLIB_VERSION)_linux_amd64.tar.gz
VENDOR_URL := https://github.com/raysan5/raylib/releases/download/$(RAYLIB_VERSION)/$(TARBALL)

# Prefer a system raylib; fall back to a vendored copy from `make vendor`.
PC_CFLAGS := $(shell pkg-config --cflags raylib 2>/dev/null)
PC_LIBS := $(shell pkg-config --libs raylib 2>/dev/null)

ifneq ($(strip $(PC_LIBS)),)
  RL_CFLAGS := $(PC_CFLAGS)
  RL_LIBS := $(PC_LIBS)
else ifneq ($(wildcard $(VENDOR)/include/raylib.h),)
  RL_CFLAGS := -I$(VENDOR)/include
  RL_LIBS := -L$(VENDOR)/lib -lraylib -Wl,-rpath,$(abspath $(VENDOR)/lib)
else ifneq ($(wildcard /usr/include/raylib.h),)
  RL_CFLAGS :=
  RL_LIBS := -lraylib
else
  RL_MISSING := 1
endif

flipclock: main.c
ifdef RL_MISSING
	@echo "raylib not found. Install it (sudo pacman -S raylib) or run: make vendor"
	@false
endif
	$(CC) $(CFLAGS) $(RL_CFLAGS) -o $@ $< $(RL_LIBS) $(LDLIBS_SYS)

# No-sudo option: drop the official prebuilt raylib into ./vendor.
vendor:
	mkdir -p vendor
	curl -sSL -o vendor/$(TARBALL) $(VENDOR_URL)
	tar xzf vendor/$(TARBALL) -C vendor
	rm -f vendor/$(TARBALL)
	@echo "raylib vendored in $(VENDOR) -- run: make"

run: flipclock
	./flipclock

clean:
	rm -f flipclock

distclean: clean
	rm -rf vendor

.PHONY: run clean distclean vendor
