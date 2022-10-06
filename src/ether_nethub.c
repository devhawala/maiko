/*******************************************************************************/
/*                                                                             */
/* Support for Dodo Nethub networking (see: https://github.com/devhawala/dodo) */
/*                                                                             */
/* Copyright (c) 2022, Dr. Hans-Walter Latz. All rights reserved.              */
/* Released to the Public Domain.                                              */
/*                                                                             */
/*******************************************************************************/

#if defined(MAIKO_ENABLE_NETHUB) && !defined(MAIKO_ENABLE_ETHERNET)

#include "version.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>

#include "commondefs.h"
#include "lispemul.h"
#include "lispmap.h"
#include "emlglob.h"
#include "lsptypes.h"
#include "lspglob.h"
#include "adr68k.h"
#include "ether.h"
#include "dbprint.h"
#include "etherdefs.h"
#include "ifpage.h"
#include "iopage.h"

/*
**  --- ether implementation common data -------------------------------------------
*/

extern int      ether_fd;      /* file descriptor for ether socket */
extern u_char   ether_host[6]; /* 48 bit address of this node */
extern u_char   broadcast[6];
extern int      ether_bsize;   /* if nonzero then a receive is pending */
extern u_char  *ether_buf;     /* address of receive buffer */
extern LispPTR *PENDINGINTERRUPT68k;
extern fd_set   LispReadFds;

extern int ETHEREventCount;

/*
**  --- nethub configuration data --------------------------------------------------
*/

static char* nethubHost = NULL;
static int   nethubPort = 3333;
static int   mac0 = 0xCA;
static int   mac1 = 0xFF;
static int   mac2 = 0xEE;
static int   mac3 = 0x12;
static int   mac4 = 0x34;
static int   mac5 = 0x56;

void setNethubHost(char* host) {
  if (host && *host) {
    nethubHost = host;
/*    printf("nh :: host now: '%s'\n", nethubHost); */
  }
}

void setNethubPort(int port) {
  if (port > 0 && port <= 65535) {
    nethubPort = port;
/*    printf("nh :: port now: %d\n", nethubPort); */
  }
}

void setNethubMac(int m0, int m1, int m2, int m3, int m4, int m5) {
  if (   m0 >= 0 && m0 <= 255
      && m1 >= 0 && m1 <= 255
      && m2 >= 0 && m2 <= 255
      && m3 >= 0 && m3 <= 255
      && m4 >= 0 && m4 <= 255
      && m5 >= 0 && m5 <= 255) {
    mac0 = m0;
    mac1 = m1;
    mac2 = m2;
    mac3 = m3;
    mac4 = m4;
    mac5 = m5;
/*    printf("nh :: mac now: %02X-%02X-%02X-%02X-%02X-%02X\n", mac0, mac1, mac2, mac3, mac4, mac5); */
  }
}

/*
**  --- very simple logging --------------------------------------------------------
*/

static int loglevel = 0;
static int logcnt = 0;

void setNethubLogLevel(int ll) {
  loglevel = ll;
  printf("nh :: leglevel now: %d\n", loglevel);
}

#define log_info(line) { if (loglevel == 1) \
	{ printf("nh-info[%d]: ", logcnt++); printf line; }}
#define log_debug(line) { if (loglevel >= 2) \
	{ printf("nh-debug[%d]: ", logcnt++); printf line; }}
#define log_all(line) { if (loglevel < 1) \
	{ printf("nh[%d]: ", logcnt++); printf line; }}

/*
**  --- utilities ------------------------------------------------------------------
*/

#define LONG_PTR_MASK ((long)(~((long)3)))

static void dblwordsSwap(u_char* basePtr, int forBytes) {
  u_char* wordsPtr = (u_char*)((long)basePtr & LONG_PTR_MASK);
  int neededBytes = forBytes + (basePtr - wordsPtr);
  int wordCount = (neededBytes + 3) / 4;
  while (wordCount > 0) {
    u_char b0 = wordsPtr[0];
    u_char b1 = wordsPtr[1];
    u_char b2 = wordsPtr[2];
    u_char b3 = wordsPtr[3];
    wordsPtr[0] = b3;
    wordsPtr[1] = b2;
    wordsPtr[2] = b1;
    wordsPtr[3] = b0;
    wordsPtr += 4;
    wordCount--;
  }
}

static void asyncFd(int fd) {
#ifdef O_ASYNC
  if (fcntl(ether_fd, F_SETOWN, getpid()) == -1) perror("fcntl F_SETOWN error");
  if (fcntl(ether_fd, F_SETFL, fcntl(ether_fd, F_GETFL, 0) | O_ASYNC) == -1) perror("fcntl F_SETFL on error");
  log_debug(("  async io enabled for ether_fd\n"));
#endif
}

/*
**  --- connect, disconnect and transmit packets -----------------------------------
*/

void connectToHub() {
  if (nethubHost == NULL) {
    return;
  }
  if (ether_fd > -1) {
    return;
  }

  log_debug(("connectToHub() - begin\n"));

  ether_host[0] = mac0;
  ether_host[1] = mac1;
  ether_host[2] = mac2;
  ether_host[3] = mac3;
  ether_host[4] = mac4;
  ether_host[5] = mac5;

  log_debug(("  resolving nethub hostname\n"));

  struct hostent *host = gethostbyname(nethubHost);
  if (host == NULL || host->h_addr_list == NULL) {
    log_debug(("connectToHub(): hostname '%s' cannot be resolved\n", nethubHost));
    log_info(("connectToHub(): hostname '%s' cannot be resolved\n", nethubHost));
    log_all(("connectToHub(): hostname '%s' cannot be resolved\n", nethubHost));
    return;
  }

  log_debug(("  connecting to nethub\n"));

  struct sockaddr_in hubaddr;
  hubaddr.sin_family = AF_INET;
  hubaddr.sin_addr.s_addr = *((unsigned long *)host->h_addr);
  hubaddr.sin_port = htons(nethubPort);

  ether_fd = socket(AF_INET, SOCK_STREAM, 0);

  if (connect(ether_fd, (struct sockaddr*)&hubaddr, sizeof(hubaddr)) < 0) {
    perror("connectToHub() - connect failed");
    close(ether_fd);
    ether_fd = -1;
    log_info(("connectToHub(): FAILED to connect to nethub\n"));
    return;
  }
  log_debug(("  connected to nethub, ether_fd = %d\n", ether_fd));

  FD_SET(ether_fd, &LispReadFds);
  log_debug(("  added ether_fd to LispReadFds\n"));

  log_debug(("connectToHub() - end\n\n"));
  log_info(("connectToHub() - connected to nethub\n"));
  log_all(("connectToHub() - connected to nethub\n"));
}

static void disconnectFromHub() {
  if (ether_fd > -1) {
    close(ether_fd);
    ether_fd = -1;
    log_debug(("disconnectFromHub() - disconnected from nethub\n"));
    log_info(("disconnectFromHub() - disconnected from nethub\n"));
    log_all(("disconnectFromHub() - disconnected from nethub\n"));
  }
}

/* buffering for multipe packets:
** if interrupts are'nt processed fast enough, recv() may deliver >1 packets at once
*/
static u_char multiBuffer[65536]; /* should be enough for > 100 IDP packets with ~ 600 bytes */
static u_char* nextPacket = NULL;
static u_char* rcvBuffer = multiBuffer;
static int rcvLen = 0;

/* -1: failed/not connected, 0: no packet available, > 0: received byte count */
static int recvPacket() {
  if (ether_fd < 0) {
    log_debug(("  recvPacket() :: not connected to hub !!!\n"));
    return -1;
  }

  if (ether_bsize < 1) {
    log_debug(("  recvPacket() :: no receive buffer present -> return 0\n"));
    return 0;
  }

  if (nextPacket == NULL) {

    /* try to get the next packet(s) form network */
    rcvLen = recv(ether_fd, multiBuffer, sizeof(multiBuffer), MSG_DONTWAIT);
    if (rcvLen < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      perror("recvPacket() :: recv(ether_fd, rcvBuffer, sizeof(rcvBuffer), MSG_DONTWAIT)");
      disconnectFromHub();
      return -1;
    }

    if (rcvLen <= 0) {
      log_debug(("  recvPacket() :: no network packet available -> return 0\n"));
      return 0;
    }

    rcvBuffer = multiBuffer;

  } else {
    
    /* move to next packet in multi packet buffer */
    rcvBuffer = nextPacket;

  }
  nextPacket = NULL;

  int bLen = ((rcvBuffer[0] << 8) & 0xFF00) | (rcvBuffer[1] & 0x00FF);
  log_debug(("  recvPacket() :: received %d bytes, logical packet length = %d bytes\n", rcvLen, bLen));
  if (bLen & 0x0001) {
    log_debug(("recvPacket() ERROR :: odd byte byte length of logical packet -> disconnecting from nethub\n"));
    log_info(("recvPacket() ERROR :: odd byte byte length of logical packet -> disconnecting from nethub\n"));
    log_all(("recvPacket() ERROR :: odd byte byte length of logical packet -> disconnecting from nethub\n"));
    disconnectFromHub();
    return -1;
  }
  if (bLen > (rcvLen - 2)) {
    log_debug(("  recvPacket() ERROR :: logical packet larger than packet -> disconnecting from nethub\n"));
    log_info(("recvPacket() ERROR :: logical packet larger than packet -> disconnecting from nethub\n"));
    log_all(("recvPacket() ERROR :: logical packet larger than packet -> disconnecting from nethub\n"));
    disconnectFromHub();
    return -1;
  }

  if (bLen < 14) {
    /* not even enough bytes for the ethernet header */
    /* -> discard packet */
    log_debug(("  recvPacket() WARNING :: packet too small -> discard packet -> return 0\n"));
    log_info(("recvPacket() WARNING :: packet too small -> discard packet -> return 0\n"));
    log_all(("recvPacket() WARNING :: packet too small -> discard packet -> return 0\n"));
    return 0;
  }

  if ((bLen + 2) != rcvLen) {
    log_debug(("  recvPacket() ***** :: logical/physical length differ -> preserving next packet in multiBuffer\n"));
    log_info(("recvPacket() ***** :: logical/physical length differ -> preserving next packet in multiBuffer\n"));
    nextPacket = &rcvBuffer[bLen + 2];
    rcvLen -= bLen + 2;
    log_debug(("  recvPacket() ***** :: next packet in multiBuffer at +%d , remaining length = %d\n", 
	       (int)(nextPacket - multiBuffer), rcvLen));
    log_info(("recvPacket() ***** :: next packet in multiBuffer at +%d , remaining length = %d\n", 
	       (int)(nextPacket - multiBuffer), rcvLen));
  }

  if (   memcmp(&rcvBuffer[2], ether_host, 6) != 0
      && memcmp(&rcvBuffer[2], broadcast, 6) != 0) {
    log_debug(("  recvPacket() :: packet is not for us and not a broadcast, ignoring packet\n"));
    return 0;
  }

  DLword copyLen = (bLen < ether_bsize) ? bLen : ether_bsize;

#if BYTESWAP
  log_debug(("  recvPacket() :: byte-swap copying %d bytes to 0x%016lX\n", copyLen, (unsigned long)ether_buf));

  dblwordsSwap(ether_buf, copyLen);
  memcpy(ether_buf, &rcvBuffer[2], copyLen);
  dblwordsSwap(ether_buf, copyLen);

#else
  log_debug(("  recvPacket() :: copying %d bytes to 0x%016X\n", copyLen, (unsigned long)ether_buf));

  memcpy(ether_buf, &rcvBuffer[2], copyLen);

#endif

  ether_bsize = 0;

#if BYTESWAP
log_debug(("  recvPacket() :: IOPage->dlethernet[2] = 0x%04X (before)\n", IOPage->dlethernet[2]));

#if 1 /* byte-order in OPage->dlethernet[2] does not seem to matter ... boolean ? */
  IOPage->dlethernet[2] = copyLen;
#else
  int b0 = copyLen & 0x000F;
  int b1 = (copyLen >> 4) & 0x000F;
  int b2 = (copyLen >> 8) & 0x000F;
  int b3 = (copyLen >> 12) & 0x000F;
  IOPage->dlethernet[2] = (b0 << 12) | (b1 << 8) | (b2 << 4) | b3;
#endif

log_debug(("  recvPacket() :: IOPage->dlethernet[2] = 0x%04X (after)\n", IOPage->dlethernet[2]));
#else

log_debug(("  recvPacket() :: IOPage->dlethernet[3] = 0x%04X (before)\n", IOPage->dlethernet[3]));
  IOPage->dlethernet[3] = copyLen;
log_debug(("  recvPacket() :: IOPage->dlethernet[3] = 0x%04X (after)\n", IOPage->dlethernet[3]));

#endif

  return copyLen;
}


/* -1: failed/not connected; >= 0: packet bytes sent */
static int sendPacket(u_char *source, int sourceLen) {
  if (ether_fd < 0) {
    log_debug(("  sendPacket() :: not connected to hub !!!\n"));
    return -1;
  }

  u_char sndBuffer[2050];
  
  if (sourceLen < 14 || sourceLen > (sizeof(sndBuffer) - 2)) {
    log_debug(("  sendPacket() :: invalid packet length: %d !!!\n", sourceLen));
    return -1;
  }

  if (sourceLen & 0x0001) {
    log_debug(("  sendPacket() :: packet length not even (mutiple of 2) !!!\n"));
    return -1;
  }

  sndBuffer[0] = (sourceLen >> 8) & 0x00FF;
  sndBuffer[1] = sourceLen & 0x00FF;
  int resLen = 0;
  u_char *sPos = source;
  u_char *dPos = &sndBuffer[2];
  while(resLen < sourceLen) {
    *dPos++ = *sPos++;
    *dPos++ = *sPos++;
    resLen += 2;
  }

  int result = send(ether_fd, sndBuffer, sourceLen + 2, 0);
  return result;
}

/*
**  --- SUBR implementations -------------------------------------------------------
*/

/************************************************************************/
/*									*/
/*			e t h e r _ s u s p e n d			*/
/*									*/
/*	Suspend receiving packets.                                      */
/*	175/70/0							*/
/*									*/
/************************************************************************/
LispPTR ether_suspend(LispPTR args[])
{
  if (nethubHost == NULL) {
    return (ATOM_T);
  }
  log_debug(("ether_suspend() -- ignored!\n\n"));
  log_info(("ether_suspend()\n"));
  return (ATOM_T);
}


/************************************************************************/
/*									*/
/*			e t h e r _ r e s u m e				*/
/*									*/
/*	resume nit socket to receive all types of packets 175/71/0	*/
/*									*/
/************************************************************************/
LispPTR ether_resume(LispPTR args[])
{
  if (nethubHost == NULL) {
    return (ATOM_T);
  }
  log_debug(("ether_resume() - begin\n"));
  asyncFd(ether_fd);
  log_debug(("ether_resume() - end\n\n"));
  log_info(("ether_resume()\n"));
  return (ATOM_T);
}


/************************************************************************/
/*									*/
/*			e t h e r _ c t r l r				*/
/*									*/
/*	return T if ether controller is available 175/72/0		*/
/*									*/
/************************************************************************/
LispPTR ether_ctrlr(LispPTR args[])
{
  if (nethubHost == NULL) {
    return (NIL);
  }
  log_debug(("ether_ctrlr() - begin\n"));

  connectToHub();

  if (ether_fd < 0) {
    log_debug(("ether_ctrlr() - end -> NIL\n\n"));
    log_info(("ether_ctrlr() -> NIL\n"));
    return (NIL);
  } else {
    asyncFd(ether_fd);
    log_debug(("ether_ctrlr() - end -> ATOM_T\n\n"));
    log_info(("ether_ctrlr() -> ATOM_T\n"));
    return (ATOM_T);
  }
}


/**********************************************************************
 *	ether_reset(args) 175/73/0
 *	reset ether controller and disable receipt of packets
 **********************************************************************/
LispPTR ether_reset(LispPTR args[])
{
  if (nethubHost == NULL) {
    return (NIL);
  }
  log_debug(("ether_reset() - begin\n"));
  
  if (ether_fd < 0) {
    log_debug(("ether_reset() - end -> NIL\n\n"));
    log_info(("ether_reset() -> NIL\n"));
    return (NIL);
  }
  ether_bsize = 0; /* deactivate receiver */
  log_debug(("ether_reset() - end -> ATOM_T\n\n"));
  log_info(("ether_reset() -> ATOM_T\n"));
  return (ATOM_T);
}


/************************************************************************/
/*									*/
/*			  e t h e r _ g e t (175/74/2)			*/
/*									*/
/*	Set up the Ethernet driver to receive a packet.  The driver	*/
/*	first tries to read any pending packet from the net, and if	*/
/*	there is one, ether_get returns T.  If there is no pending	*/
/*	packet, the failing read sets us up to get an interrupt when	*/
/*	a packet DOES arrive, and ether_get returns NIL.		*/
/*									*/
/*		args[0] Length of the buffer we're passed		*/
/*		args[1] LISP address of a packet buffer			*/
/*									*/
/*	sets ether_buf to the buffer address, for check_ether's use	*/
/*	sets ether_bsize to the buffer size.  ether_bsize>0 means	*/
/*	it's OK to read packets from the network on interrupt.		*/
/*									*/
/************************************************************************/
LispPTR ether_get(LispPTR args[])
{
  if (nethubHost == NULL) {
    return (NIL);
  }
  log_debug(("ether_get() - begin\n"));

  u_char *target = (u_char *)NativeAligned2FromLAddr(args[1]);
  int maxByteCount = 2 * (0xFFFF & args[0]); /* words to bytes */
  log_debug(("  target = 0x%016lX maxBytecount: %d bytes\n", (unsigned long)target, maxByteCount));

  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGIO);
  sigprocmask(SIG_BLOCK, &signals, NULL);

  ether_buf = target;
  ether_bsize = maxByteCount;

  int receivedBytes = recvPacket();

  sigprocmask(SIG_UNBLOCK, &signals, NULL);

  log_debug(("ether_get() ::  receivedBytes: %d bytes\n", receivedBytes));
  if (receivedBytes <= 0) {
    log_debug(("ether_get() - end -> NIL\n\n"));
    return (NIL);
  }

  log_debug(("ether_get() - end -> ATOM_T\n\n"));
  log_info(("ether_get(): received packet, len = %d bytes\n", receivedBytes));
  return (ATOM_T);
}



/**********************************************************************
 *	ether_send(args) 175/75/2 max_words,buffer_addr
 *	send a packet
 **********************************************************************/
LispPTR ether_send(LispPTR args[])
{
  if (nethubHost == NULL) {
    return (NIL);
  }
  log_debug(("ether_send() - begin\n"));

  u_char *source = (u_char *)NativeAligned2FromLAddr(args[1]);
  int byteCount = 2 * (0xFFFF & args[0]); /* words to bytes */

  log_debug(("   source = 0x%08X , bytecount: %d bytes\n", (unsigned int)source, byteCount));

#ifdef BYTESWAP
  dblwordsSwap(source, byteCount);
  int bytesSent = sendPacket(source, byteCount);
  dblwordsSwap(source, byteCount);
#else
  int bytesSent = sendPacket(source, byteCount);
#endif

  if (bytesSent < 0) {
    log_debug(("ether_send() - failed (bytesSent: %d bytes) -> NIL\n\n", bytesSent));
    log_info(("ether_send(): FAILED to send packet, len = %d\n", byteCount));
    return (NIL);
  }
  
  log_debug(("ether_send() - end -> ATOM_T\n\n"));
  log_info(("ether_send(): sent packet, len = %d\n", bytesSent));
  return (ATOM_T);
}

/**********************************************************************
 *	ether_setfilter(args) 175/76/1 filterbits
 *	check whether a packet has come. if does, notify iocb
 **********************************************************************/
LispPTR ether_setfilter(LispPTR args[])
{
  log_debug(("ether_setfilter() - ????\n\n"));
  return (NIL);
}


/**********************************************************************
 *	check_ether() 175/77/0
 *	checks an incoming packet
 **********************************************************************/
LispPTR check_ether()
{
  LispPTR result = (NIL);

  if (ether_fd < 0) {
    return result;
  }

  struct pollfd pfds[1];
  pfds[0].fd = ether_fd;
  pfds[0].events = POLLIN;
  pfds[0].revents = 0;
  poll(pfds, 1, 0);
  if (pfds[0].revents & POLLIN) {
    log_debug(("check_ether() - begin\n"));
    int receivedBytes = recvPacket();
    if (receivedBytes > 0) {
      ((INTSTAT *)NativeAligned4FromLAddr(*INTERRUPTSTATE_word))->ETHERInterrupt = 1;
      ((INTSTAT *)NativeAligned4FromLAddr(*INTERRUPTSTATE_word))->IOInterrupt = 1;
      ETHEREventCount++;
      Irq_Stk_Check = Irq_Stk_End = 0; /* ??? */
      *PENDINGINTERRUPT68k = (ATOM_T);
      log_debug(("check_ether() :: raised LISP interrupt\n"));
      log_debug(("check_ether() :: INTERRUPTSTATE_word   = 0x%08X\n",
		 *((int*)NativeAligned4FromLAddr(*INTERRUPTSTATE_word))));
      log_debug(("check_ether() :: PENDINGINTERRUPT_word = 0x%08X\n",
		 *((int*)PENDINGINTERRUPT68k)));
      result = (ATOM_T);
      log_debug(("check_ether() - end -> ATOM_T\n\n"));
      log_info(("check_ether(): received packet, len = %d bytes\n", receivedBytes));
    } else {
      log_debug(("check_ether() - end -> NIL\n\n"));
    }
  }

  return result;
}

#endif /* MAIKO_ENABLE_NETHUB */

