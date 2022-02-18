CC=gcc
CFLAGS=-Wall -std=c11 -Werror
BUILDDIR=build
BIN=$(BUILDDIR)/bin/service_runner
OBJ=$(BUILDDIR)/obj/service_runner.o
RELEASE=OFF
PREFIX=/usr/local/bin

ifeq ($(RELEASE),ON)
    CFLAGS += -O2
else
    CFLAGS += -g
endif

.PHONY: all clean install uninstall

all: $(BIN)

install: $(BIN)
	@mkdir -p $(PREFIX)
	cp $(BIN) $(PREFIX)

uninstall:
	rm $(PREFIX)/service_runner

$(BIN): $(OBJ)
	@mkdir -p $(BUILDDIR)/bin
	$(CC) $(CFLAGS) $< -o $@

$(BUILDDIR)/obj/%.o: src/%.c
	@mkdir -p $(BUILDDIR)/obj
	$(CC) $(CFLAGS) $< -c -o $@

clean:
	rm -r $(BIN) $(OBJ)
