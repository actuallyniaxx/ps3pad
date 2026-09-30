# ps3pad - build with pkg-config + SDL2
#   make              -> normal build (dynamic SDL2)
#   make STATIC=1     -> link SDL2 statically (used by the Windows CI)

CC      ?= cc
PKG     ?= pkg-config
TARGET  ?= ps3pad
CFLAGS  ?= -O2
CFLAGS  += -std=gnu99 -Wall -Wextra -Wno-unused-parameter
# On Windows sdl2.pc adds -Dmain=SDL_main; we do not want it (SDL_MAIN_HANDLED)
CFLAGS  += $(filter-out -Dmain=SDL_main,$(shell $(PKG) --cflags sdl2))

ifeq ($(STATIC),1)
  # Drop -mwindows (we want a console) and SDL2main (we use SDL_MAIN_HANDLED)
  LIBS := $(filter-out -mwindows -lSDL2main,$(shell $(PKG) --static --libs sdl2))
  LDFLAGS += -static
else
  LIBS := $(shell $(PKG) --libs sdl2)
endif

ifeq ($(OS),Windows_NT)
  TARGET := $(TARGET).exe
  LIBS   += -lws2_32
endif

all: $(TARGET)

$(TARGET): src/main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -f ps3pad ps3pad.exe

.PHONY: all clean
