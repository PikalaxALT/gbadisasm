CAPSTONE_VERMAJ := 4
CAPSTONE_VERMIN := 0
CAPSTONE_REVISN := 1
CAPSTONE_VERTAG :=

CAPSTONE_VERSION := $(CAPSTONE_VERMAJ).$(CAPSTONE_VERMIN).$(CAPSTONE_REVISN)
ifneq (,$(CAPSTONE_VERTAG))
	CAPSTONE_VERSION += -$(CAPSTONE_VERTAG)
endif

CAPSTONE_DIR := capstone-$(CAPSTONE_VERSION)
CAPSTONE_ARCHIVE := $(CAPSTONE_DIR).tar.gz
CAPSTONE_LIB := -L$(CAPSTONE_DIR) -lcapstone

DEBUG ?= 0

CC := gcc
CFLAGS := -isystem $(CAPSTONE_DIR)/include -Wall -Wextra -Wpedantic -DCAPSTONE_VERSION="$(CAPSTONE_VERSION)" -DCAPSTONE_VERMAJ=$(CAPSTONE_VERMAJ) -DCAPSTONE_VERMIN=$(CAPSTONE_VERMIN) -DCAPSTONE_REVISN=$(CAPSTONE_REVISN)
ifeq ($(DEBUG),1)
CFLAGS += -O0 -g
else
CFLAGS += -O3
endif
#CFLAGS += -fsanitize=address
PROGRAM := ndsdisasm
SOURCES := main.c disasm.c
LIBS := $(CAPSTONE_LIB)

MAKEFLAGS += --no-print-dir

# Compile the program
$(PROGRAM): $(SOURCES) $(CAPSTONE_LIB)
	$(CC) $(CFLAGS) $^ -o $@

# Build libcapstone
$(CAPSTONE_LIB): $(CAPSTONE_DIR)
	@$(MAKE) -C $(CAPSTONE_DIR) CAPSTONE_STATIC=yes CAPSTONE_SHARED=no CAPSTONE_ARCHS="arm" CAPSTONE_BUILD_CORE_ONLY=yes

# Extract the archive
$(CAPSTONE_DIR): $(CAPSTONE_ARCHIVE)
	tar -xvf $(CAPSTONE_ARCHIVE)

clean:
	$(RM) $(PROGRAM) $(PROGRAM).exe
	@$(MAKE) -C $(CAPSTONE_DIR) clean

distclean: clean
	rm -rf $(CAPSTONE_DIR)
