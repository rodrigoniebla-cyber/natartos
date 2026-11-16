CC = aarch64-linux-gnu-gcc
CFLAGS = -shared -fPIC -O2 -fno-builtin -nostdlib
LDFLAGS = -Wl,-soname,natartos_loader.so

all: natartos_loader.so bootmenu

natartos_loader.so: natartos_loader.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	@echo "[natartos] Built $@ ($$(stat -c%s $@) bytes)"

# Boot menu ("Stock or Modified?") — standalone static binary, like fb_receiver.
bootmenu: bootmenu.c
	$(CC) -static -O2 -Wall -o $@ $<
	@echo "[natartos] Built $@ ($$(stat -c%s $@) bytes)"

clean:
	rm -f natartos_loader.so bootmenu

.PHONY: all clean
