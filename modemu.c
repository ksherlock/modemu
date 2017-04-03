#include <stdio.h>	/*(printf,fprintf)*/
#include <ctype.h>	/*isprint*/
#include <arpa/telnet.h>/*IAC,DO,DONT,...*/
#include <sys/time.h>	/*fd_set,FD_ZERO*/
#include <fcntl.h>	/*O_RDWR*/
#include <errno.h>	/*EINTR*/
#include <string.h>	/*strlen*/
#include <stdlib.h>	/*exit*/

#include "defs.h"	/*uchar*/
#include "sock.h"	/*sock*/
#include "sockbuf.h"	/*sockBufR,sockBufW*/
#include "ttybuf.h"	/*tty*/
#include "stty.h"	/*(setTty)*/
#include "cmdlex.h"	/*Cmdstat*/
#include "telopt.h"	/*telOptSummary*/
#include "atcmd.h"	/*CHAR_CR*/
#include "timeval.h"	/*(timeval...)*/
#include "commx.h"	/*(commxForkExec)*/
#include "cmdarg.h"	/*cmdarg*/


/* socket input processing loop */

static void
sockReadLoop(void)
{
    static enum {
	SRL_NORM, SRL_IAC, SRL_CMD,
	SRL_SB, SRL_SBC, SRL_SBS, SRL_SBI, SRL_CR
    } state /*= SRL_NORM*/;
    static int cmd;
    static int opt;
    int c;

    if (atcmd.pr) {
	while ((c = getSock1()) >= 0) putTty1(c);
    } else {
	while ((c = getSock1()) >= 0) {
	    switch (state) {
	    case SRL_IAC:
		switch (c) {
		case WILL:
		case WONT:
		case DO:
		case DONT:
		    cmd = c;
		    state = SRL_CMD;
		    break;
		case IAC:
		    /*if (telOpt.binrecv)*/ {
			putTty1(c);
			state = SRL_NORM;
		    }
		    break;
		case SB:
		    state = SRL_SB;
		    break;
		default:
		    state = SRL_NORM;
		    telOptPrintCmd("<", c);
		}
		break;
	    case SRL_CMD:
		if (telOptHandle(cmd, c)) sock.alive = 0;
		state = SRL_NORM;
		break;
	    case SRL_SB:
		opt = c;
		state = SRL_SBC;
		break;
	    case SRL_SBC:
		state = (c == TELQUAL_SEND)? SRL_SBS : SRL_NORM;
		break;
	    case SRL_SBS:
		state = (c == IAC)? SRL_SBI : SRL_NORM;
		break;
	    case SRL_SBI:
		telOptSBHandle(opt);
		state = SRL_NORM;
		break;
	    case SRL_CR:
		state = SRL_NORM;
		if (c != 0x00) putTty1(c);
		break;
	    default:
		if (c == IAC) {
		    state = SRL_IAC;
		} else {
		    /*putTty1(telOpt.binrecv? c : (c & 0x7f));*/
		    putTty1(c);
		}
		if (c == '\r') state = SRL_CR;
	    }
	}
    }
}


/* TTY input processing loop */

static struct {
    enum { ESH_NORM, ESH_P1, ESH_P2, ESH_P3 } state;
    struct timeval plus1T; /* the time 1st '+' input */
    int checkSilence; /* Recognized silence,"+++" sequence.
			 Now prepare for the 2nd silence.. */
    struct timeval expireT; /* keep silence until the time */
} escSeq;

#define escSeqReset() { escSeq.state = ESH_NORM; }
#define checkTtySilence() (escSeq.checkSilence)

/* t1 - t2 > S12? */
static int
s12timePassed(const struct timeval *t1p, const struct timeval *t2p)
{
    struct timeval t;

    timevalSet10ms(&t, atcmd.s[12] * 2);
    timevalAdd(&t, t2p);
    return (timevalCmp(t1p, &t) > 0);
}

static void
escSeqHandle(int c)
{
    switch (escSeq.state) {
    case ESH_P1:
	if (c == CHAR_ESC
	    && ! s12timePassed(&ttyBufR.newT, &escSeq.plus1T)) {
	    escSeq.state = ESH_P2;
	} else escSeq.state = ESH_NORM;
	break;
    case ESH_P2:
	if (c == CHAR_ESC
	    && ! s12timePassed(&ttyBufR.newT, &escSeq.plus1T)) {
	    escSeq.checkSilence = 1;
	    timevalSet10ms(&escSeq.expireT, atcmd.s[12] * 2);
	    timevalAdd(&escSeq.expireT, &ttyBufR.newT);
	    escSeq.state = ESH_P3;
	} else escSeq.state = ESH_NORM;
	break;
    case ESH_P3:
	escSeq.checkSilence = 0;
	escSeq.state = ESH_NORM;
	/*break;*/
    case ESH_NORM:
	if (c == CHAR_ESC
	    && s12timePassed(&ttyBufR.newT, &ttyBufR.prevT)) {
	    escSeq.plus1T = ttyBufR.newT;
	    escSeq.state = ESH_P1;
	}
    }
}


/*#define LINEBUF_SIZE 256 =>defs.h*/

static struct {
    uchar buf[LINEBUF_SIZE];
    uchar *ptr;
    /*int eol;*/
} lineBuf;

#define lineBufReset() { lineBuf.ptr = lineBuf.buf; /*lineBuf.eol = 0;*/ }
#define putLine1(c) \
{ \
    if (lineBuf.ptr < lineBuf.buf + LINEBUF_SIZE) *lineBuf.ptr++ = (c); \
}
#define lineBufBS() \
{ \
    if (lineBuf.ptr > lineBuf.buf) lineBuf.ptr--; \
}

static void
ttyReadLoop(void)
{
    int c;
    static int crhack=0;

    if (atcmd.pr) {
	while ((c = getTty1()) >= 0) {
	    putSock1(c);
	    escSeqHandle(c);
	}
    } else if (telOpt.sgasend) {
	while ((c = getTty1()) >= 0) {
	    /*if (telOpt.binsend)*/ {
		if (crhack && c != CHAR_LF) putSock1(0);
		if (c == IAC) putSock1(IAC);
		putSock1(c);
		crhack = (c == CHAR_CR);
	    } /*else putSock1(c & 0x7f);*/
	    escSeqHandle(c);
	}
    } else {
	/* !sgasend == local echo mode, which cannot be true binmode */
	while ((c = getTty1()) >= 0) {
	    putTty1(c);
	    if (c == CHAR_CR) {
		putTty1(CHAR_LF);
		putSockN(lineBuf.buf, lineBuf.ptr - lineBuf.buf);
		putSock1('\r'); /* EOL = CRLF */
		putSock1('\n');
		lineBufReset();
	    } else if (c == CHAR_LF) {
		/* ignore LFs. CR is the EOL char for modems */
	    } else if (c == CHAR_BS) {
		lineBufBS();
	    } else {
		/*if (telOpt.binsend)*/ {
		    if (c == IAC) putLine1(IAC);
		    putLine1(c);
		} /*else putLine1(c & 0x7f);*/
	    }
	    escSeqHandle(c);
	}
    }
}


/* online mode main loop */

static int
onlineMode(void)
{
    fd_set rfds,wfds;
    struct timeval t;

    sockBufRReset();
    sockBufWReset();
    ttyBufRReset();
    /*ttyBufWReset();*/
    lineBufReset();
    escSeqReset();

    if (!telOpt.sentReqs && !atcmd.pr) telOptSendReqs();

    t.tv_sec = 0;
    while (sockIsAlive()) {
	struct timeval *tp;

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);

	if (ttyBufWReady()) FD_SET(sock.fd, &rfds); /*flow control*/
	if (sockBufWHasData()) FD_SET(sock.fd, &wfds);
	if (sockBufWReady()) FD_SET(tty.rfd, &rfds); /*flow control*/
	if (ttyBufWHasData()) FD_SET(tty.wfd, &wfds);

	if (escSeq.checkSilence) {
	    struct timeval tt;
	    gettimeofday(&tt, NULL);
	    if (timevalCmp(&tt, &escSeq.expireT) >= 0) {
		escSeq.checkSilence = 0;
		return 1;
	    }
	    t = escSeq.expireT;
	    timevalSub(&t, &tt);
	    tp = &t;
	} else {
	    tp = NULL; /* infinite */
	}

	if (select(sock.fd+1, &rfds, &wfds, NULL, tp) < 0) {
	    if (errno != EINTR) perror("select()");
	    continue;
	}

	if (FD_ISSET(sock.fd, &wfds)) {
	    sockBufWrite();
	}
	if (FD_ISSET(tty.wfd, &wfds)) {
	    ttyBufWrite();
	}
	if (FD_ISSET(sock.fd, &rfds)) {
	    sockBufRead();
	    sockReadLoop();
	}
	if (FD_ISSET(tty.rfd, &rfds)) {
	    ttyBufRead();
	    ttyReadLoop();
	}
    }
    sockShutdown();
    return 0;
}


/* command mode input processing loop */

/*#define CMDBUF_MAX 255 =>defs.h*/

static struct {
    uchar buf[CMDBUF_MAX+1];
    uchar *ptr;
    int eol;
} cmdBuf;

#define cmdBufReset() { cmdBuf.ptr = cmdBuf.buf; cmdBuf.eol = 0; }
#define putCmd1(c) \
{ \
    if (cmdBuf.ptr < cmdBuf.buf + CMDBUF_MAX) *cmdBuf.ptr++ = (c); \
}
#define cmdBufBS() \
{ \
    if (cmdBuf.ptr > cmdBuf.buf) cmdBuf.ptr--; \
}

static void
cmdReadLoop(void)
{
    int c;

    while ((c = getTty1()) >= 0) {
	putTty1(c);
	if (c == CHAR_CR) {
	    cmdBuf.eol = 1;
	    *cmdBuf.ptr = '\0';
	    return; /* may discard some chars in ttyBufR */
	} else if (c == CHAR_BS) {
	    cmdBufBS();
#if 0
	} else if (c <= ' ' || c == 127) {
	    /* side effect: "a  t" is recognized as "at" */
#else
	} else if (c < ' ' || c == 127) {
#endif
	    /* just ignore them */
	} else {
	    putCmd1(c);
	}
    }
}


/* command mode main loop */

static void
putTtyCmdstat(Cmdstat s)
{
    static const char *cmdstatStr[] = {
	"OK",
	"ERROR",
	"CONNECT",
	"NO CARRIER",
	"",
	"",
	"",
    };

    putTty1(CHAR_CR);
    putTty1(CHAR_LF);
    putTtyN(cmdstatStr[s], strlen(cmdstatStr[s]));
    putTty1(CHAR_CR);
    putTty1(CHAR_LF);
}

static Cmdstat
cmdMode(void)
{
    fd_set rfds,wfds;
    Cmdstat stat;

    cmdBufReset();
    ttyBufRReset();
    /*ttyBufWReset();*/
    
    for (;;) {
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);

	if (ttyBufWReady()) FD_SET(tty.rfd, &rfds); /*flow control*/
	if (ttyBufWHasData()) FD_SET(tty.wfd, &wfds);

	if (select(tty.wfd+1, &rfds, &wfds, NULL, NULL) < 0) {
	    if (errno != EINTR) perror("select()");
	    continue;
	}

	if (FD_ISSET(tty.wfd, &wfds)) {
	    ttyBufWrite(); /* put CR before dialup */
	    if (cmdBuf.eol) {
		stat = cmdLex((char *)cmdBuf.buf);
		cmdBufReset();
		switch (stat) {
		case CMDST_ATD:
		case CMDST_ATO:
		    return stat;
		case CMDST_OK:
		case CMDST_ERROR:
		    putTtyCmdstat(stat);
		    break;
		default:; /*CMDST_NOCMD*/
		}		
	    }
	}
	if (FD_ISSET(tty.rfd, &rfds)) {
	    ttyBufRead();
	    cmdReadLoop();
	}
    }
}


/* open a pty */

static int
openPtyMaster(const char *dev)
{
    int fd;

    if (dev) {
    	fd = open(dev, O_RDWR);
    	if (fd < 0) {
	    perror("open");
	    exit(1);
    	}
    	return fd;
    }

    fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0) {
	perror("posix_openpt");
	exit(1);
    }
    grantpt(fd);
    unlockpt(fd);
    return fd;
}



int
main(int argc, const char *argv[])
{
#ifdef SOCKS
    SOCKSinit(argv[0]);
#endif
    cmdargParse(argv);
    switch (cmdarg.ttymode) {
    case CA_STDINOUT:
	tty.rfd = 0;
	tty.wfd = 1;
	setTty();
	break;
    case CA_SHOWDEV:
	tty.rfd = tty.wfd = openPtyMaster(NULL);
	printf("%s\n", ptsname(tty.rfd));
	break;
    case CA_COMMX:
	tty.rfd = tty.wfd = openPtyMaster(NULL);
	commxForkExec(cmdarg.commx, ptsname(tty.rfd));
	break;
    case CA_DEVGIVEN:
	tty.rfd = tty.wfd = openPtyMaster(cmdarg.dev);
	break;
    }

    ttyBufWReset();
    telOptInit();
    atcmdInit(); /* initialize atcmd */

 CMDMODE:
    switch (cmdMode()) {
    case CMDST_ATD:
	if (sockIsAlive()) {
	    putTtyCmdstat(CMDST_ERROR);
	    goto CMDMODE;
	}
	goto DIAL;
    case CMDST_ATO:
	if (!sockIsAlive()) {
	    putTtyCmdstat(CMDST_NOCARRIER);
	    goto CMDMODE;
	}
	goto ONLINE;
    default:;
    }

    return 1;

 DIAL:
    telOptReset(); /* before sockDial(), which may change telOpt.xx */
    switch (sockDial()) {
    case 0: /* connect */
	goto ONLINE;
    case 1: /* error */
	putTtyCmdstat(CMDST_NOCARRIER);
	goto CMDMODE;
    default:;
    }

    return 1;
    
 ONLINE:
    putTtyCmdstat(CMDST_CONNECT);
    switch (onlineMode()) {
    case 0: /* connection lost */
	putTtyCmdstat(CMDST_NOCARRIER);
	goto CMDMODE;
    case 1: /* +++ */
	putTtyCmdstat(CMDST_OK);
	goto CMDMODE;
    default:;
    }	

    return 1;
}
