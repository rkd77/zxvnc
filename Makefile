all: zxvnc spectrum.tap timex.tap

spectrum.tap: spectrum.c
	zcc +zx -vn  -lndos -llibsocket -lim2 -create-app -o spectrum.bin spectrum.c

timex.tap: timex.c
	zcc +zx -vn  -lndos -llibsocket -lim2 -create-app -o timex.bin timex.c

zxvnc:	zxvnc.c
	$(CC) -o $@ -O6 $(CFLAGS) `pkg-config --cflags libvncclient` `pkg-config --libs libvncclient` -lm $<
