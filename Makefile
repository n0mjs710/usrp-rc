# Configurable install paths
PREFIX     ?= /usr/local
SYSCONFDIR ?= /etc
BINDIR      = $(PREFIX)/bin
UNITDIR    ?= $(PREFIX)/lib/systemd/system
# vocab_8k/ and user_8k/ are read-only-to-the-program data assets (usrp-rc
# never writes to either at runtime), not configuration -- they belong under
# share/, not /etc.
DATADIR    ?= $(PREFIX)/share/usrp-rc
STATEDIR   ?= /var/local/usrp-rc

CC      ?= gcc
CFLAGS  += -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE -fPIE \
            $(shell pkg-config --cflags libsystemd opus)
LDFLAGS += -pie
LDLIBS  += $(shell pkg-config --libs libsystemd opus) -lm -lpthread

BUILDDIR  = build

SRCS = src/main.c \
       src/config.c \
       src/usrp_protocol.c \
       src/fm_protocol.c \
       src/opus_codec.c \
       src/jitter_buffer.c \
       src/tone.c \
       src/morse.c \
       src/vocab.c \
       src/voice_filter.c \
       src/message.c \
       src/ste.c \
       src/port.c \
       src/dtmf.c \
       src/command.c \
       src/schedule.c \
       src/stats.c \
       src/log.c

OBJS     = $(patsubst %.c,$(BUILDDIR)/%.o,$(SRCS))
DEPFILES = $(OBJS:.o=.d)
TARGET   = $(BUILDDIR)/usrp-rc

# tests/port_harness.c drives src/port.c's state machine directly with a
# fake clock (see the file for why) -- it needs everything port.c pulls
# in, but none of main.c's socket/pacing machinery.
HARNESS_SRCS = src/port.c \
               src/config.c \
               src/vocab.c \
               src/tone.c \
               src/morse.c \
               src/voice_filter.c \
               src/message.c \
               src/schedule.c \
               src/log.c
HARNESS_OBJS = $(patsubst %.c,$(BUILDDIR)/%.o,$(HARNESS_SRCS))
HARNESS      = $(BUILDDIR)/tests/port_harness

# vocab_8k/ is pre-built and committed to the repo; VOCAB_SRC/`make vocab`
# are only for regenerating it from the 48 kHz reference source (maintainers
# only — not part of the normal build).
VOCAB_SRC ?= /home/cort/rc/vocab_pcm

.PHONY: all clean install uninstall vocab check

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/src/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(HARNESS): $(BUILDDIR)/tests/port_harness.o $(HARNESS_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

JITTER_TEST = $(BUILDDIR)/tests/jitter_buffer_test
JITTER_TEST_OBJS = $(BUILDDIR)/src/jitter_buffer.o

$(JITTER_TEST): $(BUILDDIR)/tests/jitter_buffer_test.o $(JITTER_TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lpthread

DTMF_TEST = $(BUILDDIR)/tests/dtmf_test
DTMF_TEST_OBJS = $(BUILDDIR)/src/dtmf.o $(BUILDDIR)/src/command.o \
                 $(BUILDDIR)/src/tone.o $(BUILDDIR)/src/config.o $(BUILDDIR)/src/vocab.o \
                 $(BUILDDIR)/src/log.o

$(DTMF_TEST): $(BUILDDIR)/tests/dtmf_test.o $(DTMF_TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

STATS_TEST = $(BUILDDIR)/tests/stats_test
STATS_TEST_OBJS = $(BUILDDIR)/src/stats.o $(BUILDDIR)/src/log.o

$(STATS_TEST): $(BUILDDIR)/tests/stats_test.o $(STATS_TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

CONFIG_TEST = $(BUILDDIR)/tests/config_test
CONFIG_TEST_OBJS = $(filter-out $(BUILDDIR)/src/port.o,$(HARNESS_OBJS))

$(CONFIG_TEST): $(BUILDDIR)/tests/config_test.o $(CONFIG_TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm

# Run from the repo root -- tests/harness.conf, tones.conf and vocab_8k/ are relative.
check: $(TARGET) $(HARNESS) $(JITTER_TEST) $(DTMF_TEST) $(STATS_TEST) $(CONFIG_TEST)
	./$(HARNESS)
	./$(JITTER_TEST)
	./$(DTMF_TEST)
	./$(STATS_TEST)
	./$(CONFIG_TEST)
	python3 tools/toml2conf.py tests/legacy.toml | diff - tests/legacy.conf
	./$(TARGET) --check tests/legacy.conf
	./$(TARGET) --check usrp-rc.conf.sample

-include $(DEPFILES)
-include $(wildcard $(BUILDDIR)/tests/*.d)

vocab:
	@mkdir -p vocab_8k
	@for f in $(VOCAB_SRC)/*.wav; do \
	    sox "$$f" -r 8000 -c 1 -e signed-integer -b 16 \
	        "vocab_8k/$$(basename $$f)"; \
	done
	@echo "vocab_8k/: $$(ls vocab_8k/*.wav | wc -l) files"

clean:
	rm -rf $(BUILDDIR)

install: $(TARGET)
	install -Dm755 $(TARGET)                  $(DESTDIR)$(BINDIR)/usrp-rc
	install -Dm644 usrp-rc.conf.sample         $(DESTDIR)$(SYSCONFDIR)/usrp-rc/usrp-rc.conf.sample
	install -Dm644 tones.conf                  $(DESTDIR)$(DATADIR)/tones.conf
	install -Dm755 tools/toml2conf.py          $(DESTDIR)$(DATADIR)/toml2conf.py
	install -Dm644 systemd/usrp-rc.service     $(DESTDIR)$(UNITDIR)/usrp-rc.service
	install -dm755 $(DESTDIR)$(STATEDIR)
	@mkdir -p $(DESTDIR)$(DATADIR)/vocab_8k
	@cp -n vocab_8k/*.wav $(DESTDIR)$(DATADIR)/vocab_8k/ 2>/dev/null || true
	install -Dm644 user_8k/README.md          $(DESTDIR)$(DATADIR)/user_8k/README.md
	@if [ -z "$(DESTDIR)" ]; then \
		if [ ! -f $(SYSCONFDIR)/usrp-rc/usrp-rc.conf ]; then \
			if [ -f $(SYSCONFDIR)/usrp-rc/usrp-rc.toml ]; then \
				python3 tools/toml2conf.py $(SYSCONFDIR)/usrp-rc/usrp-rc.toml $(SYSCONFDIR)/usrp-rc/usrp-rc.conf \
					&& echo "usrp-rc: converted usrp-rc.toml to $(SYSCONFDIR)/usrp-rc/usrp-rc.conf -- run usrp-rc --check on it" \
					|| echo "usrp-rc: could not convert usrp-rc.toml -- see tools/toml2conf.py"; \
			else \
				cp $(SYSCONFDIR)/usrp-rc/usrp-rc.conf.sample $(SYSCONFDIR)/usrp-rc/usrp-rc.conf; \
				echo "usrp-rc: installed default config at $(SYSCONFDIR)/usrp-rc/usrp-rc.conf"; \
			fi; \
		else \
			echo "usrp-rc: existing config preserved at $(SYSCONFDIR)/usrp-rc/usrp-rc.conf"; \
		fi; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/usrp-rc
	rm -f $(DESTDIR)$(UNITDIR)/usrp-rc.service
	rm -rf $(DESTDIR)$(DATADIR)/vocab_8k
	rm -f $(DESTDIR)$(DATADIR)/tones.conf $(DESTDIR)$(DATADIR)/toml2conf.py
	rm -f $(DESTDIR)$(DATADIR)/user_8k/README.md
	@echo "usrp-rc: config at $(SYSCONFDIR)/usrp-rc/ preserved — remove manually if desired"
	@echo "usrp-rc: usage figures in $(STATEDIR)/ preserved — remove manually if desired"
	@echo "usrp-rc: any of your own clips in $(DATADIR)/user_8k/ preserved — remove manually if desired"
