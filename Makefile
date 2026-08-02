# Configurable install paths
PREFIX     ?= /usr/local
SYSCONFDIR ?= /etc
BINDIR      = $(PREFIX)/bin
UNITDIR    ?= $(PREFIX)/lib/systemd/system
# vocab_8k/ and user_8k/ are read-only-to-the-program data assets (usrp-rc
# never writes to either at runtime), not configuration -- they belong under
# share/, not /etc.
DATADIR    ?= $(PREFIX)/share/usrp-rc

CC      ?= gcc
CFLAGS  += -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE -fPIE \
            $(shell pkg-config --cflags libsystemd opus)
LDFLAGS += -pie
LDLIBS  += $(shell pkg-config --libs libsystemd opus) -lm -lpthread

BUILDDIR  = build
VENDORDIR = vendor/tomlc99

SRCS = src/main.c \
       src/config.c \
       src/usrp_protocol.c \
       src/opus_codec.c \
       src/jitter_buffer.c \
       src/tone.c \
       src/morse.c \
       src/vocab.c \
       src/voice_filter.c \
       src/message.c \
       src/ste.c \
       src/port.c \
       $(VENDORDIR)/toml.c

OBJS     = $(patsubst %.c,$(BUILDDIR)/%.o,$(SRCS))
DEPFILES = $(OBJS:.o=.d)
TARGET   = $(BUILDDIR)/usrp-rc

# vocab_8k/ is pre-built and committed to the repo; VOCAB_SRC/`make vocab`
# are only for regenerating it from the 48 kHz reference source (maintainers
# only — not part of the normal build).
VOCAB_SRC ?= /home/cort/rc/vocab_pcm

.PHONY: all clean install uninstall vocab

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILDDIR)/src/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(VENDORDIR) -MMD -MP -c -o $@ $<

$(BUILDDIR)/$(VENDORDIR)/%.o: $(VENDORDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Wno-unused-result -MMD -MP -c -o $@ $<

-include $(DEPFILES)

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
	install -Dm644 usrp-rc.toml.sample         $(DESTDIR)$(SYSCONFDIR)/usrp-rc/usrp-rc.toml.sample
	install -Dm644 systemd/usrp-rc.service     $(DESTDIR)$(UNITDIR)/usrp-rc.service
	@mkdir -p $(DESTDIR)$(DATADIR)/vocab_8k
	@cp -n vocab_8k/*.wav $(DESTDIR)$(DATADIR)/vocab_8k/ 2>/dev/null || true
	install -Dm644 user_8k/README.md          $(DESTDIR)$(DATADIR)/user_8k/README.md
	@if [ -z "$(DESTDIR)" ]; then \
		if [ ! -f $(SYSCONFDIR)/usrp-rc/usrp-rc.toml ]; then \
			cp $(SYSCONFDIR)/usrp-rc/usrp-rc.toml.sample $(SYSCONFDIR)/usrp-rc/usrp-rc.toml; \
			echo "usrp-rc: installed default config at $(SYSCONFDIR)/usrp-rc/usrp-rc.toml"; \
		else \
			echo "usrp-rc: existing config preserved at $(SYSCONFDIR)/usrp-rc/usrp-rc.toml"; \
		fi; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/usrp-rc
	rm -f $(DESTDIR)$(UNITDIR)/usrp-rc.service
	rm -rf $(DESTDIR)$(DATADIR)/vocab_8k
	rm -f $(DESTDIR)$(DATADIR)/user_8k/README.md
	@echo "usrp-rc: config at $(SYSCONFDIR)/usrp-rc/ preserved — remove manually if desired"
	@echo "usrp-rc: any of your own clips in $(DATADIR)/user_8k/ preserved — remove manually if desired"
