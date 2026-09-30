# ps3pad - build con pkg-config + SDL2
#   make              -> build normal (SDL2 dinámica)
#   make STATIC=1     -> intenta enlazar SDL2 estática (lo usa el CI de Windows)

CC      ?= cc
PKG     ?= pkg-config
TARGET  ?= ps3pad
CFLAGS  ?= -O2
CFLAGS  += -std=gnu99 -Wall -Wextra -Wno-unused-parameter
# En Windows sdl2.pc mete -Dmain=SDL_main; no lo queremos (SDL_MAIN_HANDLED)
CFLAGS  += $(filter-out -Dmain=SDL_main,$(shell $(PKG) --cflags sdl2))

ifeq ($(STATIC),1)
  # Quitamos -mwindows (queremos consola) y SDL2main (usamos SDL_MAIN_HANDLED)
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
