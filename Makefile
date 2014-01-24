all: zxvnc spectrum.tap

spectrum.tap: spectrum.c
	zcc +zx -vn  -lndos -llibsocket -lim2 -create-app -o spectrum.bin spectrum.c

zxvnc:	zxvnc.c
	$(CC) -o $@ $(CFLAGS) `pkg-config --cflags libvncclient` `pkg-config --libs libvncclient` -lm $<
