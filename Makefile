PREFIX = $(HOME)/.local/bin

WEBKIT_FLAGS = `pkg-config --cflags webkit2gtk-4.0` `pkg-config --libs webkit2gtk-4.0`

all: webwidget

webwidget: webwidget.c
	$(CC) --std=c99 -Wall $< -o $@ `pkg-config --cflags webkit2gtk-4.0 gio-unix-2.0` `pkg-config --libs webkit2gtk-4.0`

install: webwidget
	cp webwidget $(PREFIX)
