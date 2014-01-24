zxvnc
=====

VNC viewer for ZX Spectrum, Spectranet and Kempston Mouse

Requirements:
z88dk
socklib (from Spectranet)
libvncclient from libvncserver.

To build type:
make

On the Spectrum load the spectrum.tap. It will be listen on port 2000 for connections.
Run somewhere a vncserver with geometry 256x192 and finally run zxvnc. For example:
./zxvnc :2

zxvnc will connect to both the Zx Spectrum and the vncserver. It will be some kind of proxy.
The 127.0.0.2 address of Spectrum is hardcoded in zxvnc.
Unfortunately, performance is poor.

This program contains code from the scrplus (http://scrplus.sourceforge.net/)
and is based on SDLVncviewer from libvncserver.

The license is GPL v3.
