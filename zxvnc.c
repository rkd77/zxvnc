/**
 * @example SDLvncviewer.c
 */

#include <signal.h>
#include <rfb/rfbclient.h>

#include <poll.h>
#include <pthread.h>
#include <fcntl.h>
#include <netdb.h>
#include <math.h>

static unsigned char display[12288 + 64];
static int to_refresh;

struct read_spectrum
{
	unsigned char ch;
	unsigned char but;
	unsigned short x;
	unsigned short y;
} read_spec;

#ifndef max
#define max(a,b)	((a)>(b)?(a):(b))
#endif

#ifndef min
#define min(a,b)	((a)>(b)?(b):(a))
#endif

static int render = 0;

#define RENDER_TIMEX	1 // Timex 8x1 attribute mode
#define RENDER_ULAPLUS	2 // ULA+ mode
#define RENDER_BW		4 // black & white mode, don't flip attribs

#define IS_RENDER_TIMEX(render)		((render) & RENDER_TIMEX)
#define IS_RENDER_ULAPLUS(render)	((render) & RENDER_ULAPLUS)
#define IS_RENDER_BW(render)		((render) & RENDER_BW)

typedef unsigned char rgb_t[3], hsv_t[3], yuv_t[3];
rfbBool npx[256][192];
static unsigned char pal[32][192][2];
static unsigned char palent[64];
static unsigned char tweaks[3][2]; // h/s/v, shift/scale

static int enableResizable = 1, viewOnly, listenLoop, buttonMask;
static int realWidth, realHeight, bytesPerPixel, rowStride;

static int rightAltKeyDown, leftAltKeyDown;


static yuv_t post[256][192];


static void
zxtorgb(unsigned char zx, rgb_t rgb)
{
	int r=zx&2;
	int g=zx&4;
	int b=zx&1;
	int br=(zx&8)?240:200; // figures based on http://www.worldofspectrum.org/forums/showpost.php?p=453665&postcount=2 (na_th_an)
	rgb[0]=r?br:0;
	rgb[1]=g?br:0;
	rgb[2]=b?br:0;
	if((zx%8)==1) rgb[2]+=15; // make blue and bright blue slightly brighter.  Based on http://www.worldofspectrum.org/forums/showpost.php?p=461355&postcount=27 (aowen)
}

static void
rgbtoyuv(const rgb_t rgb, yuv_t yuv)
{
	yuv[0]=(0.257 * rgb[0]) + (0.504 * rgb[1]) + (0.098 * rgb[2]) + 16.0;
	yuv[1]=-(0.148 * rgb[0]) - (0.291 * rgb[1]) + (0.439 * rgb[2]) + 128.0;
	yuv[2]=(0.439 * rgb[0]) - (0.368 * rgb[1]) - (0.071 * rgb[2]) + 128.0;
}

static void
yuvtorgb(const yuv_t yuv, rgb_t rgb)
{
	rgb[2]=min(max(1.164*(yuv[0]-16.0)+2.018*(yuv[1]-128.0), 0), 255);
	rgb[1]=min(max(1.164*(yuv[0]-16.0)-0.813*(yuv[2]-128.0)-0.391*(yuv[1]-128.0), 0), 255);
	rgb[0]=min(max(1.164*(yuv[0]-16.0)+1.596*(yuv[2]-128.0), 0), 255);
}

static double
yuvdist(const yuv_t pal, const yuv_t dat)
{
	return(fabs(pal[0]-dat[0])+fabs(pal[1]-dat[1])+fabs(pal[2]-dat[2]));
}

static void
rgbtohsv(const rgb_t rgb, hsv_t hsv)
{
	// based on https://en.wikipedia.org/wiki/HSL_and_HSV
	unsigned char M=0, m=255;
	unsigned int i;
	double hp; // H'
	unsigned char C; // Chroma

	for (i = 0; i < 3; i++)
	{
		M = max(M, rgb[i]);
		m = min(m, rgb[i]);
	}
	C=M-m; // Chroma
	if(!C)
		hp=0; // really undefined
	else if(M==rgb[0])
		hp=fmod((rgb[1]-rgb[2])/(double)C, 6);
	else if(M==rgb[1])
		hp=(rgb[2]-rgb[0])/(double)C+2;
	else
		hp=(rgb[0]-rgb[1])/(double)C+4;
	hsv[2]=floor(hp*255/6.0); // Hue
	hsv[0]=M; // Value
	if(M)
		hsv[1]=(C*255)/M; // Saturation
	else
		hsv[1]=0;
}

static unsigned char
clamp(int i)
{
	if(i<0) return(0);
	if(i>255) return(255);
	return(i);
}

static void
hsvtorgb(const hsv_t hsv, rgb_t rgb)
{
	double hp=hsv[2]*6/255.0; // H'
	double C=hsv[1]*hsv[0]/255;
	double x=C*(1-fabs(fmod(hp, 2)-1));
	int m=hsv[0]-C;
	int fh=floor(hp);
	switch(fh%6)
	{
		case 0:
			rgb[0]=clamp(C+m);
			rgb[1]=clamp(x+m);
			rgb[2]=clamp(m);
		break;
		case 1:
			rgb[0]=clamp(x+m);
			rgb[1]=clamp(C+m);
			rgb[2]=clamp(m);
		break;
		case 2:
			rgb[0]=clamp(m);
			rgb[1]=clamp(C+m);
			rgb[2]=clamp(x+m);
		break;
		case 3:
			rgb[0]=clamp(m);
			rgb[1]=clamp(x+m);
			rgb[2]=clamp(C+m);
		break;
		case 4:
			rgb[0]=clamp(x+m);
			rgb[1]=clamp(m);
			rgb[2]=clamp(C+m);
		break;
		case 5:
			rgb[0]=clamp(C+m);
			rgb[1]=clamp(m);
			rgb[2]=clamp(x+m);
		break;
	}
}

yuv_t palyuv[16];

static void
genpalyuv(void)
{
	int i;

	rgb_t rgb;
	for (i = 0; i < 16;i++)
	{
		zxtorgb(i, rgb);
		rgbtoyuv(rgb, palyuv[i]);
	}
}

static uint32_t get(rfbClient *cl, int x, int y)
{
	switch (bytesPerPixel) {
	case 1: return ((uint8_t *)cl->frameBuffer)[x + y * cl->width];
	case 2: return ((uint16_t *)cl->frameBuffer)[x + y * cl->width];
	case 4: return ((uint32_t *)cl->frameBuffer)[x + y * cl->width];
	default:
		rfbClientErr("Unknown bytes/pixel: %d", bytesPerPixel);
		exit(1);
	}
}

static int
vtweak(int value, int shift, int scale)
{
	return((int)min(max((value+(shift<<1)-255-127.5)*scale/(double)(256-scale)+127.5, 0), 255));
}

static int
ctweak(int value, int shift, int scale)
{
	return((int)min(max((value+(shift<<1)-255)*scale/(double)(256-scale), 0), 255));
}

static int
htweak(int value, int shift)
{
	return((value+shift+128)%256);
}

static void
apply_tweak(rfbClient *cl, unsigned int xs, unsigned int ys, unsigned int w, unsigned int  h)
//, uchar render, rgb_t pix[w][h], uchar tweaks[3][2], yuv_t post[w][h])
{
	unsigned int xk = min(xs + w, 256);
	unsigned int yk = min(ys + h, 192);
	unsigned int x, y;

	for (x = xs; x < xk; ++x)
		for (y = ys ;y < yk; ++y)
		{
			hsv_t hsv, hpost;
			rgb_t rgb;
			rgb_t rpost;
			unsigned int a = get(cl, x, y);

			rgb[0] = (a >> 16) & 255;
			rgb[1] = (a >> 8) & 255;
			rgb[2] = (a & 255);

			rgbtohsv(rgb, hsv);
			hpost[0]=vtweak(hsv[0], tweaks[0][0], tweaks[0][1]);
			hpost[1]=ctweak(hsv[1], tweaks[1][0], tweaks[1][1]);
			hpost[2]=htweak(hsv[2], tweaks[2][0]);
			if(IS_RENDER_BW(render))
				hpost[1]=0;
			hsvtorgb(hpost, rpost);
			rgbtoyuv(rpost, post[x][y]);
		}
}

static void
pickattrs(void)
//uint w, uint h, uchar render, yuv_t yuv[w][h], uchar pal[w/8][h][2])
{
	const unsigned int w = 256;
	const unsigned int h = 192;
	unsigned int x, y;

	for (x=0; x < w/8; x++)
	{
		for (y=0; y < (IS_RENDER_TIMEX(render) ? h : h/8); y++)
		{
			if(IS_RENDER_BW(render))
				pal[x][y][0]=7;
			else
			{
				unsigned char ma, a;
				double mad;
				for (a = 1; a < 128; a++) // 1, not 0, because a=0 => i==p => continue
				{
					unsigned int dx, dy;
					int i = a & 7;
					int p = (a >> 3) & 7;
					int b;
					double d;

					if (i <= p)
						continue;
					b = a >> 6;
					d = 0;
					for (dx = 0; dx < 8; dx++)
					{
						for (dy = 0; dy < (IS_RENDER_TIMEX(render) ? 1 : 8); dy++)
						{
							const unsigned char *xyuv=post[(x*8)+dx][IS_RENDER_TIMEX(render)?y:(y*8)+dy];
							double di = yuvdist(palyuv[i+(b<<3)], xyuv);
							double dp = yuvdist(palyuv[p+(b<<3)], xyuv);
							d += min(di, dp);
						}
					}
					if((a==1)||(d<mad))
					{
						mad=d;
						ma=a;
					}
				}
				pal[x][y][0]=ma;
			}
		}
	}
}

static void
dither(void)
//(uint w, uint h, uchar render, yuv_t yuv[w][h], uchar pal[w/8][h][2], const palette palent, bool dithflow, bool npx[w][h])
{
	const unsigned int w = 256;
	const unsigned int h = 192;
	const rfbBool dithflow = TRUE;
	unsigned int x, y;
	yuv_t nyuv[w][h];

	memcpy(nyuv, post, sizeof(nyuv));
	for (x = 0; x < w; x++)
	{
		for (y = 0; y < h; y++)
		{
			int bx=floor(x/8), by=IS_RENDER_TIMEX(render)?y:(int)floor(y/8);
			const unsigned char *a=pal[bx][by], *ax=NULL, *ay=NULL;
			const unsigned char *iyuv, *pyuv;
			double di, dp;
			rfbBool flowx, flowy;
			unsigned int j;

			if (bx+1<(int)w/8) ax=pal[bx+1][by];
			if (by+1<IS_RENDER_TIMEX(render)?h:h/8) ay=pal[bx][by+1];
			if (IS_RENDER_ULAPLUS(render))
			{
				iyuv=palyuv[palent[a[0]]];
				pyuv=palyuv[palent[a[1]]];
			}
			else
			{
				unsigned int i=a[0]&7;
				unsigned int p=(a[0]>>3)&7;
				unsigned int b=a[0]>>6;
				iyuv=palyuv[i+(b<<3)];
				pyuv=palyuv[p+(b<<3)];
			}
			di = yuvdist(iyuv, nyuv[x][y]);
			dp = yuvdist(pyuv, nyuv[x][y]);
			npx[x][y] = (di<dp);
			flowx = (x + 1 <w) && (dithflow||((x+1)%8)||(ax&&(a[0]==ax[0])&&(a[1]==ax[1]))),
			flowy = (y + 1 <h) && (dithflow||(IS_RENDER_TIMEX(render)? TRUE : ((y+1)%8))||(ay&&(a[0]==ay[0])&&(a[1]==ay[1])));
			for (j=0; j < 3; j++)
			{
				int dy=(npx[x][y]?iyuv:pyuv)[j]-nyuv[x][y][j];
				if(flowx)
				{
					nyuv[x+1][y][j]=min(max(floor((y==0 ? post :nyuv)[x+1][y][j]-dy*7.0/16.0), 0), 255);
					if(flowy)
						nyuv[x+1][y+1][j]=min(max(floor(post[x+1][y+1][j]-dy/16.0), 0), 255);
				}
				if(flowy)
				{
					if(x>0)
						nyuv[x-1][y+1][j]=min(max(floor(nyuv[x-1][y+1][j]-dy*3.0/16.0), 0), 255);
					nyuv[x][y+1][j]=min(max(floor(nyuv[x][y+1][j]-dy*5.0/16.0), 0), 255);
				}
			}
		}
	}
}

static void update(rfbClient* cl,int x,int y,int w,int h) {
//fprintf(stderr, "update: x=%d y=%d w=%d h=%d\n", x,y,w,h);
	apply_tweak(cl, x, y, w, h);
	to_refresh = 1;
}


#ifdef __MINGW32__
#define LOG_TO_FILE
#endif

#ifdef LOG_TO_FILE
#include <stdarg.h>
static void
log_to_file(const char *format, ...)
{
    FILE* logfile;
    static char* logfile_str=0;
    va_list args;
    char buf[256];
    time_t log_clock;

    if(!rfbEnableClientLogging)
      return;

    if(logfile_str==0) {
	logfile_str=getenv("VNCLOG");
	if(logfile_str==0)
	    logfile_str="vnc.log";
    }

    logfile=fopen(logfile_str,"a");

    va_start(args, format);

    time(&log_clock);
    strftime(buf, 255, "%d/%m/%Y %X ", localtime(&log_clock));
    fprintf(logfile,buf);

    vfprintf(logfile, format, args);
    fflush(logfile);

    va_end(args);
    fclose(logfile);
}
#endif


static void cleanup(rfbClient* cl)
{
  /*
    just in case we're running in listenLoop:
    close viewer window by restarting SDL video subsystem
  */
  if(cl)
    rfbClientCleanup(cl);
}

static void
handleSpectrumEvent(rfbClient *cl, struct read_spectrum *e)
{
	static struct read_spectrum old = {0};

	if (e->ch != old.ch)
	{
		if (e->ch == 0)
		{
			SendKeyEvent(cl, old.ch, FALSE);
		}
		else
		{
			SendKeyEvent(cl, e->ch, TRUE);
		}
	}
	if (e->x != old.x || e->y != old.y || e->but != old.but)
	{
		int b = ((e->but & 1) ? rfbButton1Mask : 0) | ((e->but & 2) ? rfbButton2Mask : 0) | ((e->but & 4) ? rfbButton3Mask : 0);

		SendPointerEvent(cl, e->x, e->y, b);
	}
	old = *e;
}


static void
writescr(void)
//FILE *fp, uint w, uint h, uchar render, uchar pal[w/8][h][2], const palette palent, bool npx[w][h])
{
	unsigned int i;
	unsigned char *address = display;
	const unsigned int w = 256;
	const unsigned int h = 192;

	for (i=0;i<0x1800;i++)
	{
		unsigned int sx=(i&0x1f)<<3, sy=((i&0x1800)>>5)|((i&0xe0)>>2)|((i&0x700)>>8);
		unsigned int j;
		unsigned char b=0; // construct the byte
		for (j=0;j<8;j++)
		{
			rfbBool px = FALSE;
			if ((sx+j<w)&&(sy<h)) px=npx[sx+j][sy];
			b+=px?(1<<(7-j)):0;
		}
		*address++ = b;
	}
	// Dump: END OF DISPLAY FILE; START OF ATTRIBUTE FILE
	for (i=0; i < (IS_RENDER_TIMEX(render) ? 0x1800 : 0x300); i++)
	{
		unsigned char blank[2]={0, 0};
		const unsigned char *palp=blank;
		unsigned int bx=(i&0x1f), by=(i>>5);
		if((bx<w/8)&&(by<(IS_RENDER_TIMEX(render)?h:h/8))) palp=pal[bx][by];
		if(IS_RENDER_ULAPLUS(render))
		{
			unsigned char b=(palp[0]&0x7)|((palp[1]&0x7)<<3)|((palp[0]&0x30)<<2); // construct attribute byte
			*address++ = b;
		}
		else
		{
			*address++ = palp[0];
		}
	}
	// Dump: END OF ATTRIBUTE FILE
	if(IS_RENDER_ULAPLUS(render))
	{
		unsigned int p;
		// Dump: START OF PALETTE
		for (p=0; p < 64; p++)
			*address++ = palent[p];
		// Dump: END OF PALETTE
	}
}


static void *
send_loop(void *arg)
{
	int sockfd = *(int *)arg;
	int counter = 0;

	while (TRUE)
	{
		if (to_refresh)
		{
			int pos, to_write;

			pickattrs();
			dither();
			writescr();
			to_refresh = counter = 0;

			for (pos = 0, to_write = 6912; to_write;)
			{
				int sent = write(sockfd, display + pos, to_write);

				if (sent < 0)
				{
					fprintf(stderr, "sent = %d\n",sent);
					break;
				};
				pos += sent;
				to_write -= sent;
			}
		}
		else
		{
			static struct timespec t = {
				.tv_sec = 0,
				.tv_nsec = 10000000
			};
			nanosleep(&t, NULL);
			++counter;
		}
	}
	return NULL;
}

int
main(int argc,char** argv)
{
	rfbClient* cl;
	int i, j;

	struct sockaddr_in remoteaddr;
	struct hostent *he;
	int sockfd;
	pthread_t fred;

#ifdef LOG_TO_FILE
	rfbClientLog=rfbClientErr=log_to_file;
#endif

	for (i = 1, j = 1; i < argc; i++)
		if (!strcmp(argv[i], "-viewonly"))
			viewOnly = 1;
		else if (!strcmp(argv[i], "-resizable"))
			enableResizable = 1;
		else if (!strcmp(argv[i], "-no-resizable"))
			enableResizable = 0;
		else if (!strcmp(argv[i], "-listen")) {
		        listenLoop = 1;
			argv[i] = "-listennofork";
                        ++j;
		}
		else {
			if (i != j)
				argv[j] = argv[i];
			j++;
		}
	argc = j;

	signal(SIGINT, exit);

	he = gethostbyname("127.0.0.2");
	if (!he) {
		perror("gethostbyname");
		exit(255);
	}

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("socket");
		exit(255);
	}

	printf("Connecting...\n");
	memset(&remoteaddr, 0, sizeof(remoteaddr));
	remoteaddr.sin_family = AF_INET;
	remoteaddr.sin_port = htons(2000);
	memcpy(&(remoteaddr.sin_addr), he->h_addr, he->h_length);
	if(connect(sockfd, (struct sockaddr *)&remoteaddr, sizeof(remoteaddr)) < 0) {
		perror("connect");
		exit(255);
	}

	for (i = 0; i < 3; i++) // set tweak defaults
	{
		tweaks[i][0]=128;
		tweaks[i][1]=128;
	}

	genpalyuv();

	//fcntl(sockfd, F_SETFL, O_NONBLOCK);
	pthread_create(&fred, NULL, send_loop, &sockfd);

	do {
	  /* 16-bit: cl=rfbGetClient(5,3,2); */
	  cl=rfbGetClient(8,3,4);
	  //cl->MallocFrameBuffer=resize;
	  cl->canHandleNewFBSize = TRUE;
	  cl->GotFrameBufferUpdate=update;
	  //cl->HandleKeyboardLedState=kbd_leds;
	  //cl->HandleTextChat=text_chat;
	  //cl->GotXCutText = got_selection;
	  cl->listenPort = LISTEN_PORT_OFFSET;
	  cl->listen6Port = LISTEN_PORT_OFFSET;
	  if(!rfbInitClient(cl,&argc,argv))
	    {
	      cl = NULL; /* rfbInitClient has already freed the client struct */
	      cleanup(cl);
	      break;
	    }
	bytesPerPixel = cl->format.bitsPerPixel / 8;

		update(cl, 0, 0, 256, 192);

		while(1)
		{
			struct pollfd fd_array;
			int retval;

			fd_array.fd = sockfd;
			fd_array.events = POLLIN;
			// wait 10 millisecond for data on pty file descriptor.
			retval = poll(&fd_array, 1, 10);
			// no data or poll() error.
			if (retval > 0)
			{
				unsigned char *pos;
				int count;

				for (pos = (unsigned char *)&read_spec, count = sizeof(read_spec); count > 0;)
				{ 
					int ile = read(sockfd, pos, sizeof(read_spec));

					if (ile < 0) break;
					pos += ile;
					count -= ile;
					if (count == 0)
					{
						handleSpectrumEvent(cl, &read_spec);
					}
				}
			}
			i=WaitForMessage(cl,500);
			if (i<0)
			{
				cleanup(cl);
				break;
			}
			if (i)
				if(!HandleRFBServerMessage(cl))
				{
					cleanup(cl);
					break;
				}
		}
	} while(listenLoop);

	return 0;
}
