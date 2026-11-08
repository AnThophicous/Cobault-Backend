# Cobault: build
#
#   make            compila build/cobault
#   make test       testes unitários do núcleo (JSON, arena, seletor, HLS, URLs)
#   make check      testes unitários + integração local (HTTP com Range, HLS AES-128)
#   make install    instala em $(PREFIX)/bin (padrão /usr/local)

CC      ?= cc
OPT     ?= -O3
PREFIX  ?= /usr/local

CSTD    := -std=c11
WARN    := -Wall -Wextra -Wformat=2 -Wshadow -Wpointer-arith -Wstrict-prototypes \
           -Wno-unused-parameter
DEFS    := -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
INC     := -Isrc
PKGS    := libcurl libcrypto
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS)) -lpthread

CFLAGS  ?= $(CSTD) $(OPT) $(WARN) $(DEFS) $(INC) $(PKG_CFLAGS)
LDFLAGS ?=
LIBS    := $(PKG_LIBS)

BUILD   := build
LIB_SRC := $(filter-out src/main.c,$(shell find src -name '*.c' | sort))
LIB_OBJ := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(LIB_SRC))
MAIN_OBJ := $(BUILD)/obj/main.o
BIN     := $(BUILD)/cobault
TEST_BIN := $(BUILD)/test_core

.PHONY: all test check install clean

all: $(BIN)

$(BIN): $(MAIN_OBJ) $(LIB_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BIN): tests/test_core.c $(LIB_OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_core.c $(LIB_OBJ) $(LIBS)

test: $(TEST_BIN)
	./$(TEST_BIN)

check: test $(BIN)
	sh tests/integration.sh $(abspath $(BIN))

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/cobault

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJ:.o=.d) $(MAIN_OBJ:.o=.d)
