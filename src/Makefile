# This Makefile is invoked by makefile after setting the working directory to
# the $(BUILD_DIR) directory.
ifndef TOP
$(error Do not call this make file directly)
endif

PROGRAM_PREFIX = fa-

SHELL = bash
INSTALL = install

all:    # Default target comes first!

CFLAGS += -std=gnu99
CFLAGS += -O2
# CFLAGS += -g

CFLAGS += -Werror
CFLAGS += -Wall -Wextra
CFLAGS += -Wno-unused-parameter
CFLAGS += -Wno-missing-field-initializers
CFLAGS += -Wundef
CFLAGS += -Wshadow
CFLAGS += -Wcast-align
CFLAGS += -Wwrite-strings
CFLAGS += -Wredundant-decls
CFLAGS += -Wmissing-prototypes
CFLAGS += -Wmissing-declarations
CFLAGS += -Wstrict-prototypes

CFLAGS += -msse
CFLAGS += -msse2
CFLAGS += -mfpmath=sse
CFLAGS += -ffast-math

CPPFLAGS += -D_GNU_SOURCE
CPPFLAGS += -D_FILE_OFFSET_BITS=64
CPPFLAGS += -D_FORTIFY_SOURCE=2

LDFLAGS += -lpthread -lrt -lm


BUILD = archiver prepare capture

COMMON_SRCS += error.c              # Core error handling framework
COMMON_SRCS += locking.c            # Simple abstraction of pthread locking
COMMON_SRCS += parse.c              # Parsing support
COMMON_SRCS += mask.c               # BPM capture mask support

# FA sniffer archiver
archiver_SRCS += archiver.c         # Command line interface to archiver
archiver_SRCS += buffer.c           # Ring buffer for data capture
archiver_SRCS += sniffer.c          # Interface to FA sniffer driver
archiver_SRCS += disk_writer.c      # Core disk writing access
archiver_SRCS += disk.c             # Disk header format definitions
archiver_SRCS += socket_server.c    # Socket server
archiver_SRCS += transform.c        # Data transformation and access
archiver_SRCS += reader.c           # Sniffer data readout
archiver_SRCS += decimate.c         # Continuous data reduction
archiver_SRCS += config_file.c      # Config file parsing

# FA archive preparation
prepare_SRCS += prepare.c           # Command line interface
prepare_SRCS += disk.c

# FA data capture
capture_SRCS += capture.c           # Command line interface
capture_SRCS += matlab.c            # Matlab header support


BUILD_NAMES = $(patsubst %,$(PROGRAM_PREFIX)%,$(BUILD))
all: $(BUILD_NAMES)

define expand_build
$(PROGRAM_PREFIX)$(target): $(COMMON_SRCS:.c=.o) $($(target)_SRCS:.c=.o)
	$$(LINK.o) $$^ $$(LOADLIBES) $$(LDLIBS) -o $$@
endef
$(foreach target,$(BUILD),$(eval $(expand_build)))

%.d: %.c
	set -o pipefail && $(CC) -M $(CPPFLAGS) $(CFLAGS) $< | \
            sed '1s/:/ $@:/' >$@
include $(patsubst %.c,%.d,$(foreach target,$(BUILD),$($(target)_SRCS)))

# Target for assembler build for code generation inspection.
%.s: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -S $<

# Installation: we install all the built executables in $(SCRIPT_DIR)
install: $(BUILD_NAMES)
ifndef SCRIPT_DIR
	echo >&2 Must define SCRIPT_DIR; false
endif
	$(INSTALL) $^ $(SCRIPT_DIR)

.PHONY: all install