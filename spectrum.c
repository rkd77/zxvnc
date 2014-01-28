
/* A demonstration of the use of poll to avoid blocking. This allows
 * multiple sockets to be read and accepted, and interleaved with keypresses.
 *
 * Compile with:
 * zcc +zx -vn -O2 -o nonblockserv.bin nonblockserv.c -lndos -llibsocket */

#include <stdio.h>
#include <stdlib.h>
#include <im2.h>
#include <input.h>		/* for in_Inkey() */
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sockpoll.h>

#define KBUFSZ 8

unsigned char bufor[1500];

char kbuf[KBUFSZ];	/* Circular keyboard buffer */
int bufoffset;		/* buffer offset */
int readoffset;		/* where we've got to reading the buffer */

uchar in_KeyDebounce = 1;       // no debouncing
uchar in_KeyStartRepeat = 20;   // wait 20/50s before key starts repeating
uchar in_KeyRepeatPeriod = 5;  // repeat every 10/50s
uint in_KbdState;               // reserved

uchar in_KempcoordX, in_KempcoordY, in_KemprawX, in_KemprawY;

char
getSingleKeypress()
{
	uchar k=0;
	if (readoffset != bufoffset)
	{
#asm
		di
#endasm
		k = *(kbuf + readoffset);
		++readoffset;
		if (readoffset == KBUFSZ)
			readoffset = 0;

#asm
		ei
#endasm
	}
	return k;
}

/* The ISR handles filling the keyboard buffer, which is a circular
 * buffer. The keyboard handler in the 'main thread' should pick characters
 * off this buffer till it catches up */
M_BEGIN_ISR(isr)
{
	uchar k = in_GetKey();

	if (k)
	{
		*(kbuf+bufoffset) = k;

		bufoffset++;
		if (bufoffset == KBUFSZ)
			bufoffset=0;
	}
}
M_END_ISR

/* Initialization routine that should be called when the client starts */
void
inputinit()
{
	/* IM2 keyboard polling routine setup - this from the
 	   example in the z88dk wiki documentation */
	#asm
	di
	#endasm
/*
	im2_Init(0xd300);
	memset(0xd300, 0xd4, 257);
	bpoke(0xd4d4, 195);
	wpoke(0xd4d5, isr);*/

	im2_Init(0xfd00);
	memset(0xfd00, 0xfe, 257);
	bpoke(0xfefe, 195);
	wpoke(0xfeff, isr);

	/* initialize the keyboard buffer */
	memset(kbuf, 0, sizeof(kbuf));
	bufoffset = 0;
	readoffset = 0;

	#asm
	ei
	#endasm
	in_MouseKempInit();
	in_MouseKempSetPos(0, 0);
}

/* De-initialize IM2 etc. to return to BASIC. */
void
inputexit()
{
	#asm
	di
	im 1
	ei
	#endasm
}

struct send
{
	unsigned char ch;
	unsigned char but;
	unsigned int x;
	unsigned int y;
} send_buffer;

main()
{
	struct sockaddr_in my_addr;
	struct pollfd p;	/* the poll information structure */
	unsigned char *start;
	unsigned int offset, to_send;
	int sockfd, connfd, rc;
	int pos = 0;
	unsigned char ch, but;
	unsigned int x, y;

	/* 0x0C clears the screen in the z88dk default console driver */
	putchar(0x0C);

	/* Create the socket */
	/* The first argument, AF_INET is the family. The Spectranet only
	   supports AF_INET at present. SOCK_STREAM in this context is
           for a TCP connection. */
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
	{
		printk("Could not open the socket - rc=%d\n", sockfd);
		return;
	}

	/* Now set up the sockaddr_in structure. */
	/* Zero it out so that any fields we don't set are set to
	   NULL (the structure also contains the local address to bind to). 
	   We will listne to port 2000. */
	memset(&my_addr, 0, sizeof(my_addr));	/* zero out all fields */
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(2000);		/* Port 2000 */

	if (bind(sockfd, &my_addr, sizeof(my_addr)) < 0)
	{
		printk("Bind failed.\n");
		sockclose(sockfd);
		return;
	}

	/* The socket should now listen. The Spectranet hardware in
	   its present form doesn't support changing the backlog, but
           the second argument to listen should still be a sensible value */
	if (listen(sockfd, 1) < 0)
	{
		printk("listen failed.\n");
		sockclose(sockfd);
		return;
	}

	printk("Listening on port 2000.\n");

	/* Now wait for things to happen. Contrast this with the server
	   code example in tutorial 2. Note that instead of calling accept()
	   we start a loop, and use the pollall() function to poll all open
	   sockets. When pollall() finds some data ready, it returns
	   status telling us why the socket was ready, so we can act
	   appropriately */

	connfd = accept(sockfd, NULL, NULL);
	if (connfd == 0)
	{
		printk("accept failed\n");
		return;
	}

#ifdef TIMEX
#asm
	ld a,2
	out (255),a
#endasm
#endif
	inputinit();
	while(1)
	{
		rc = poll_fd(connfd);
		if (rc & POLLIN)
		{
			unsigned char *s;
			int r = recv(connfd, bufor + pos, 1497, 0);

			if (r < 0)
			{
				printk("recv failed!\n");
				break;
			}

			r += pos;
			for (s = bufor; r > 0; r -= 3)
			{
				unsigned char *where = (unsigned char *)*(void **)s;

				s += 2;
				*where = *s++;
			}

			pos = 0;
			if (r < 0)
			{
				pos = r + 3;
				*bufor = *s;
				*(bufor+1) = *(s+1);
			}
		}
		if (rc & POLLHUP)
		{
			break;
		}

		ch = getSingleKeypress();
		in_MouseKemp(&but, &x, &y);

		if (x == send_buffer.x && y == send_buffer.y && ch == send_buffer.ch && but == send_buffer.but) continue;

		send_buffer.ch = ch;
		send_buffer.but = but;
		send_buffer.x = x;
		send_buffer.y = y;

		for (start = (unsigned char *)&send_buffer, offset = 0, to_send = sizeof(send_buffer); to_send;)
		{
			int r = send(connfd, start + offset, to_send, 0);
		//printf("rc = %d\n",rc);
			if (r < 0)
			{
				printk("send failed!\n");
				goto wypad;
			}
			to_send -= r;
			offset += r;
		}
	}
wypad:
	/* Close the listening socket and exit. */
	sockclose(sockfd);
	printk("Finished.\n");
	inputexit();
}
