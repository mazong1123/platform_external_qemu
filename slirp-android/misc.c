/*
 * Copyright (c) 1995 Danny Gasparovski.
 *
 * Please read the file COPYRIGHT for the
 * terms and conditions of the copyright.
 */

#define WANT_SYS_IOCTL_H
#include <slirp.h>

u_int curtime, time_fasttimo, last_slowtimo, detach_time;
u_int detach_wait = 600000;	/* 10 minutes */
struct emu_t *tcpemu;

int
inet_strtoip(const char*  str, uint32_t  *ip)
{
    int  comp[4];

    if (sscanf(str, "%d.%d.%d.%d", &comp[0], &comp[1], &comp[2], &comp[3]) != 4)
        return -1;

    if ((unsigned)comp[0] >= 256 ||
        (unsigned)comp[1] >= 256 ||
        (unsigned)comp[2] >= 256 ||
        (unsigned)comp[3] >= 256)
        return -1;

    *ip = (uint32_t)((comp[0] << 24) | (comp[1] << 16) |
                     (comp[2] << 8)  |  comp[3]);
    return 0;
}

char*  inet_iptostr(uint32_t  ip)
{
    static char  buff[32];

    snprintf(buff, sizeof(buff), "%d.%d.%d.%d",
             (ip >> 24) & 255,
             (ip >> 16) & 255,
             (ip >> 8) & 255,
             ip & 255);
    return buff;
}

/*
 * Get our IP address and put it in our_addr
 */
void
getouraddr()
{
    char*        hostname = host_name();
    SockAddress  hostaddr;

    our_addr_ip = loopback_addr_ip;

    if (sock_address_init_resolve( &hostaddr, hostname, 0, 0 ) < 0)
        return;

    our_addr_ip = sock_address_get_ip(&hostaddr);
    if (our_addr_ip == (uint32_t)-1)
        our_addr_ip = loopback_addr_ip;
}

struct quehead {
	struct quehead *qh_link;
	struct quehead *qh_rlink;
};

inline void
insque(void *a, void *b)
{
	register struct quehead *element = (struct quehead *) a;
	register struct quehead *head = (struct quehead *) b;
	element->qh_link = head->qh_link;
	head->qh_link = (struct quehead *)element;
	element->qh_rlink = (struct quehead *)head;
	((struct quehead *)(element->qh_link))->qh_rlink
	= (struct quehead *)element;
}

inline void
remque(void *a)
{
  register struct quehead *element = (struct quehead *) a;
  ((struct quehead *)(element->qh_link))->qh_rlink = element->qh_rlink;
  ((struct quehead *)(element->qh_rlink))->qh_link = element->qh_link;
  element->qh_rlink = NULL;
  /*  element->qh_link = NULL;  TCP FIN1 crashes if you do this.  Why ? */
}

/* #endif */


int
add_exec(struct ex_list **ex_ptr, int do_pty, char *exec, int addr, int port)
{
	struct ex_list *tmp_ptr;

	/* First, check if the port is "bound" */
	for (tmp_ptr = *ex_ptr; tmp_ptr; tmp_ptr = tmp_ptr->ex_next) {
		if (port == tmp_ptr->ex_fport && addr == tmp_ptr->ex_addr)
		   return -1;
	}

	tmp_ptr = *ex_ptr;
	*ex_ptr = (struct ex_list *)malloc(sizeof(struct ex_list));
	(*ex_ptr)->ex_fport = port;
	(*ex_ptr)->ex_addr = addr;
	(*ex_ptr)->ex_pty = do_pty;
	(*ex_ptr)->ex_exec = (do_pty == 3) ? exec : strdup(exec);
	(*ex_ptr)->ex_next = tmp_ptr;
	return 0;
}

#ifndef HAVE_STRERROR

/*
 * For systems with no strerror
 */

extern int sys_nerr;
extern char *sys_errlist[];

char *
strerror(error)
	int error;
{
	if (error < sys_nerr)
	   return sys_errlist[error];
	else
	   return "Unknown error.";
}

#endif


#ifdef _WIN32

int
fork_exec(struct socket *so, const char *ex, int do_pty)
{
    /* not implemented */
    return 0;
}

#else

#ifndef CONFIG_QEMU
int
slirp_openpty(amaster, aslave)
     int *amaster, *aslave;
{
	register int master, slave;

#ifdef HAVE_GRANTPT
	char *ptr;

	if ((master = open("/dev/ptmx", O_RDWR)) < 0 ||
	    grantpt(master) < 0 ||
	    unlockpt(master) < 0 ||
	    (ptr = ptsname(master)) == NULL)  {
		close(master);
		return -1;
	}

	if ((slave = open(ptr, O_RDWR)) < 0 ||
	    ioctl(slave, I_PUSH, "ptem") < 0 ||
	    ioctl(slave, I_PUSH, "ldterm") < 0 ||
	    ioctl(slave, I_PUSH, "ttcompat") < 0) {
		close(master);
		close(slave);
		return -1;
	}

	*amaster = master;
	*aslave = slave;
	return 0;

#else

	static char line[] = "/dev/ptyXX";
	register const char *cp1, *cp2;

	for (cp1 = "pqrsPQRS"; *cp1; cp1++) {
		line[8] = *cp1;
		for (cp2 = "0123456789abcdefghijklmnopqrstuv"; *cp2; cp2++) {
			line[9] = *cp2;
			if ((master = open(line, O_RDWR, 0)) == -1) {
				if (errno == ENOENT)
				   return (-1);    /* out of ptys */
			} else {
				line[5] = 't';
				/* These will fail */
				(void) chown(line, getuid(), 0);
				(void) chmod(line, S_IRUSR|S_IWUSR|S_IWGRP);
#ifdef HAVE_REVOKE
				(void) revoke(line);
#endif
				if ((slave = open(line, O_RDWR, 0)) != -1) {
					*amaster = master;
					*aslave = slave;
					return 0;
				}
				(void) close(master);
				line[5] = 'p';
			}
		}
	}
	errno = ENOENT; /* out of ptys */
	return (-1);
#endif
}
#endif

/*
 * XXX This is ugly
 * We create and bind a socket, then fork off to another
 * process, which connects to this socket, after which we
 * exec the wanted program.  If something (strange) happens,
 * the accept() call could block us forever.
 *
 * do_pty = 0   Fork/exec inetd style
 * do_pty = 1   Fork/exec using slirp.telnetd
 * do_ptr = 2   Fork/exec using pty
 */
int
fork_exec(struct socket *so, const char *ex, int do_pty)
{
	int s;
        int master = -1;
	const char *argv[256];
#if 0
	char buff[256];
#endif
	/* don't want to clobber the original */
	char *bptr;
	const char *curarg;
	int c, i;

	DEBUG_CALL("fork_exec");
	DEBUG_ARG("so = %lx", (long)so);
	DEBUG_ARG("ex = %lx", (long)ex);
	DEBUG_ARG("do_pty = %lx", (long)do_pty);

	if (do_pty == 2) {
#if 0
		if (slirp_openpty(&master, &s) == -1) {
			lprint("Error: openpty failed: %s\n", strerror(errno));
			return 0;
		}
#else
                return 0;
#endif
	} else {
		if ((s = socket_anyaddr_server(0, SOCKET_STREAM)) < 0) {
			lprint("Error: inet socket: %s\n", errno_str);
			return 0;
		}
	}

	switch(fork()) {
	 case -1:
		lprint("Error: fork failed: %s\n", strerror(errno));
		close(s);
		if (do_pty == 2)
		   close(master);
		return 0;

	 case 0:
		/* Set the DISPLAY */
		if (do_pty == 2) {
			(void) close(master);
#ifdef TIOCSCTTY /* XXXXX */
			(void) setsid();
			ioctl(s, TIOCSCTTY, (char *)NULL);
#endif
		} else {
			SockAddress  addr;
			socket_get_address(s, &addr);
			socket_close(s);
			/*
			 * Connect to the socket
			 * XXX If any of these fail, we're in trouble!
	 		 */
            s = socket_loopback_client(sock_address_get_port(&addr), SOCKET_STREAM);
		}

#if 0
		if (x_port >= 0) {
#ifdef HAVE_SETENV
			sprintf(buff, "%s:%d.%d", inet_ntoa(our_addr), x_port, x_screen);
			setenv("DISPLAY", buff, 1);
#else
			sprintf(buff, "DISPLAY=%s:%d.%d", inet_ntoa(our_addr), x_port, x_screen);
			putenv(buff);
#endif
		}
#endif
		dup2(s, 0);
		dup2(s, 1);
		dup2(s, 2);
		for (s = getdtablesize() - 1; s >= 3; s--)
		   close(s);

		i = 0;
		bptr = strdup(ex); /* No need to free() this */
		if (do_pty == 1) {
			/* Setup "slirp.telnetd -x" */
			argv[i++] = "slirp.telnetd";
			argv[i++] = "-x";
			argv[i++] = bptr;
		} else
		   do {
			/* Change the string into argv[] */
			curarg = bptr;
			while (*bptr != ' ' && *bptr != (char)0)
			   bptr++;
			c = *bptr;
			*bptr++ = (char)0;
			argv[i++] = strdup(curarg);
		   } while (c);

                argv[i] = NULL;
		execvp(argv[0], (char **)argv);

		/* Ooops, failed, let's tell the user why */
		  {
			  char buff[256];
                          int ret;

			  snprintf(buff, sizeof(buff),
                                   "Error: execvp of %s failed: %s\n",
                                   argv[0], strerror(errno));
			  do {
                                ret =write(2, buff, strlen(buff)+1);
                          } while (ret < 0 && errno == EINTR);
		  }
		close(0); close(1); close(2); /* XXX */
		exit(1);

	 default:
		if (do_pty == 2) {
			close(s);
			so->s = master;
		} else {
			/*
			 * XXX this could block us...
			 * XXX Should set a timer here, and if accept() doesn't
		 	 * return after X seconds, declare it a failure
		 	 * The only reason this will block forever is if socket()
		 	 * of connect() fail in the child process
		 	 */
             so->s = socket_accept(s, NULL);
			 socket_set_xreuseaddr(so->s);
             socket_set_oobinline(so->s);
		}
		socket_set_nonblock(so->s);

		/* Append the telnet options now */
                if (so->so_m != NULL && do_pty == 1)  {
			sbappend(so, so->so_m);
                        so->so_m = NULL;
		}

		return 1;
	}
}
#endif

#ifndef HAVE_STRDUP
char *
strdup(const char*  str)
{
	char *bptr;
	int   len = strlen(str);

	bptr = (char *)malloc(len+1);
	memcpy(bptr, str, len+1);

	return bptr;
}
#endif

#if 0
void
snooze_hup(num)
	int num;
{
	int s, ret;
#ifndef NO_UNIX_SOCKETS
	struct sockaddr_un sock_un;
#endif
	struct sockaddr_in sock_in;
	char buff[256];

	ret = -1;
	if (slirp_socket_passwd) {
		s = socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0)
		   slirp_exit(1);
		sock_in.sin_family = AF_INET;
		sock_in.sin_addr.s_addr = slirp_socket_addr;
		sock_in.sin_port = htons(slirp_socket_port);
		if (connect(s, (struct sockaddr *)&sock_in, sizeof(sock_in)) != 0)
		   slirp_exit(1); /* just exit...*/
		sprintf(buff, "kill %s:%d", slirp_socket_passwd, slirp_socket_unit);
		write(s, buff, strlen(buff)+1);
	}
#ifndef NO_UNIX_SOCKETS
	  else {
		s = socket(AF_UNIX, SOCK_STREAM, 0);
		if (s < 0)
		   slirp_exit(1);
		sock_un.sun_family = AF_UNIX;
		strcpy(sock_un.sun_path, socket_path);
		if (connect(s, (struct sockaddr *)&sock_un,
			      sizeof(sock_un.sun_family) + sizeof(sock_un.sun_path)) != 0)
		   slirp_exit(1);
		sprintf(buff, "kill none:%d", slirp_socket_unit);
		write(s, buff, strlen(buff)+1);
	}
#endif
	slirp_exit(0);
}


void
snooze()
{
	sigset_t s;
	int i;

	/* Don't need our data anymore */
	/* XXX This makes SunOS barf */
/*	brk(0); */

	/* Close all fd's */
	for (i = 255; i >= 0; i--)
	   close(i);

	signal(SIGQUIT, slirp_exit);
	signal(SIGHUP, snooze_hup);
	sigemptyset(&s);

	/* Wait for any signal */
	sigsuspend(&s);

	/* Just in case ... */
	exit(255);
}

void
relay(s)
	int s;
{
	char buf[8192];
	int n;
	fd_set readfds;
	struct ttys *ttyp;

	/* Don't need our data anymore */
	/* XXX This makes SunOS barf */
/*	brk(0); */

	signal(SIGQUIT, slirp_exit);
	signal(SIGHUP, slirp_exit);
        signal(SIGINT, slirp_exit);
	signal(SIGTERM, slirp_exit);

	/* Fudge to get term_raw and term_restore to work */
	if (NULL == (ttyp = tty_attach (0, slirp_tty))) {
         lprint ("Error: tty_attach failed in misc.c:relay()\r\n");
         slirp_exit (1);
    }
	ttyp->fd = 0;
	ttyp->flags |= TTY_CTTY;
	term_raw(ttyp);

	while (1) {
		FD_ZERO(&readfds);

		FD_SET(0, &readfds);
		FD_SET(s, &readfds);

		n = select(s+1, &readfds, (fd_set *)0, (fd_set *)0, (struct timeval *)0);

		if (n <= 0)
		   slirp_exit(0);

		if (FD_ISSET(0, &readfds)) {
			n = read(0, buf, 8192);
			if (n <= 0)
			   slirp_exit(0);
			n = writen(s, buf, n);
			if (n <= 0)
			   slirp_exit(0);
		}

		if (FD_ISSET(s, &readfds)) {
			n = read(s, buf, 8192);
			if (n <= 0)
			   slirp_exit(0);
			n = writen(0, buf, n);
			if (n <= 0)
			   slirp_exit(0);
		}
	}

	/* Just in case.... */
	exit(1);
}
#endif

#ifdef CONFIG_QEMU
#include "monitor/monitor.h"

void lprint(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    monitor_vprintf(cur_mon, format, args);
    va_end(args);
}
#else
int (*lprint_print) _P((void *, const char *, va_list));
char *lprint_ptr, *lprint_ptr2, **lprint_arg;

void
#ifdef __STDC__
lprint(const char *format, ...)
#else
lprint(va_alist) va_dcl
#endif
{
	va_list args;

#ifdef __STDC__
        va_start(args, format);
#else
        char *format;
        va_start(args);
        format = va_arg(args, char *);
#endif
#if 0
	/* If we're printing to an sbuf, make sure there's enough room */
	/* XXX +100? */
	if (lprint_sb) {
		if ((lprint_ptr - lprint_sb->sb_wptr) >=
		    (lprint_sb->sb_datalen - (strlen(format) + 100))) {
			int deltaw = lprint_sb->sb_wptr - lprint_sb->sb_data;
			int deltar = lprint_sb->sb_rptr - lprint_sb->sb_data;
			int deltap = lprint_ptr -         lprint_sb->sb_data;

			lprint_sb->sb_data = (char *)realloc(lprint_sb->sb_data,
							     lprint_sb->sb_datalen + TCP_SNDSPACE);

			/* Adjust all values */
			lprint_sb->sb_wptr = lprint_sb->sb_data + deltaw;
			lprint_sb->sb_rptr = lprint_sb->sb_data + deltar;
			lprint_ptr =         lprint_sb->sb_data + deltap;

			lprint_sb->sb_datalen += TCP_SNDSPACE;
		}
	}
#endif
	if (lprint_print)
	   lprint_ptr += (*lprint_print)(*lprint_arg, format, args);

	/* Check if they want output to be logged to file as well */
	if (lfd) {
		/*
		 * Remove \r's
		 * otherwise you'll get ^M all over the file
		 */
		int len = strlen(format);
		char *bptr1, *bptr2;

		bptr1 = bptr2 = strdup(format);

		while (len--) {
			if (*bptr1 == '\r')
			   memcpy(bptr1, bptr1+1, len+1);
			else
			   bptr1++;
		}
		vfprintf(lfd, bptr2, args);
		free(bptr2);
	}
	va_end(args);
}

void
add_emu(buff)
	char *buff;
{
	u_int lport, fport;
	u_int8_t tos = 0, emu = 0;
	char buff1[256], buff2[256], buff4[128];
	char *buff3 = buff4;
	struct emu_t *emup;
	struct socket *so;

	if (sscanf(buff, "%256s %256s", buff2, buff1) != 2) {
		lprint("Error: Bad arguments\r\n");
		return;
	}

	if (sscanf(buff1, "%d:%d", &lport, &fport) != 2) {
		lport = 0;
		if (sscanf(buff1, "%d", &fport) != 1) {
			lprint("Error: Bad first argument\r\n");
			return;
		}
	}

	if (sscanf(buff2, "%128[^:]:%128s", buff1, buff3) != 2) {
		buff3 = 0;
		if (sscanf(buff2, "%256s", buff1) != 1) {
			lprint("Error: Bad second argument\r\n");
			return;
		}
	}

	if (buff3) {
		if (strcmp(buff3, "lowdelay") == 0)
		   tos = IPTOS_LOWDELAY;
		else if (strcmp(buff3, "throughput") == 0)
		   tos = IPTOS_THROUGHPUT;
		else {
			lprint("Error: Expecting \"lowdelay\"/\"throughput\"\r\n");
			return;
		}
	}

	if (strcmp(buff1, "ftp") == 0)
	   emu = EMU_FTP;
	else if (strcmp(buff1, "irc") == 0)
	   emu = EMU_IRC;
	else if (strcmp(buff1, "none") == 0)
	   emu = EMU_NONE; /* ie: no emulation */
	else {
		lprint("Error: Unknown service\r\n");
		return;
	}

	/* First, check that it isn't already emulated */
	for (emup = tcpemu; emup; emup = emup->next) {
		if (emup->lport == lport && emup->fport == fport) {
			lprint("Error: port already emulated\r\n");
			return;
		}
	}

	/* link it */
	emup = (struct emu_t *)malloc(sizeof (struct emu_t));
	emup->lport = (u_int16_t)lport;
	emup->fport = (u_int16_t)fport;
	emup->tos = tos;
	emup->emu = emu;
	emup->next = tcpemu;
	tcpemu = emup;

	/* And finally, mark all current sessions, if any, as being emulated */
	for (so = tcb.so_next; so != &tcb; so = so->so_next) {
		if ((lport && lport == so->so_laddr_port) ||
		    (fport && fport == so->so_faddr_port)) {
			if (emu)
			   so->so_emu = emu;
			if (tos)
			   so->so_iptos = tos;
		}
	}

	lprint("Adding emulation for %s to port %d/%d\r\n", buff1, emup->lport, emup->fport);
}
#endif

#ifdef BAD_SPRINTF

#undef vsprintf
#undef sprintf

/*
 * Some BSD-derived systems have a sprintf which returns char *
 */

int
vsprintf_len(string, format, args)
	char *string;
	const char *format;
	va_list args;
{
	vsprintf(string, format, args);
	return strlen(string);
}

int
#ifdef __STDC__
sprintf_len(char *string, const char *format, ...)
#else
sprintf_len(va_alist) va_dcl
#endif
{
	va_list args;
#ifdef __STDC__
	va_start(args, format);
#else
	char *string;
	char *format;
	va_start(args);
	string = va_arg(args, char *);
	format = va_arg(args, char *);
#endif
	vsprintf(string, format, args);
	return strlen(string);
}

#endif

#if 0
void
u_sleep(int usec)
{
	struct timeval t;
	fd_set fdset;

	FD_ZERO(&fdset);

	t.tv_sec = 0;
	t.tv_usec = usec * 1000;

	select(0, &fdset, &fdset, &fdset, &t);
}
#endif

/*
 * Set fd blocking and non-blocking
 */

void
fd_nonblock(int fd)
{
#ifdef FIONBIO
#ifdef _WIN32
        unsigned long opt = 1;
#else
        int opt = 1;
#endif

	ioctlsocket(fd, FIONBIO, &opt);
#else
	int opt;

	opt = fcntl(fd, F_GETFL, 0);
	opt |= O_NONBLOCK;
	fcntl(fd, F_SETFL, opt);
#endif
}

void
fd_block(int fd)
{
#ifdef FIONBIO
#ifdef _WIN32
        unsigned long opt = 0;
#else
	int opt = 0;
#endif

	ioctlsocket(fd, FIONBIO, &opt);
#else
	int opt;

	opt = fcntl(fd, F_GETFL, 0);
	opt &= ~O_NONBLOCK;
	fcntl(fd, F_SETFL, opt);
#endif
}


#if 0
/*
 * invoke RSH
 */
int
rsh_exec(so,ns, user, host, args)
	struct socket *so;
	struct socket *ns;
	char *user;
	char *host;
	char *args;
{
	int fd[2];
	int fd0[2];
	int s;
	char buff[256];

	DEBUG_CALL("rsh_exec");
	DEBUG_ARG("so = %lx", (long)so);

	if (pipe(fd)<0) {
          lprint("Error: pipe failed: %s\n", strerror(errno));
          return 0;
	}
/* #ifdef HAVE_SOCKETPAIR */
#if 1
        if (socketpair(PF_UNIX,SOCK_STREAM,0, fd0) == -1) {
          close(fd[0]);
          close(fd[1]);
          lprint("Error: openpty failed: %s\n", strerror(errno));
          return 0;
        }
#else
        if (slirp_openpty(&fd0[0], &fd0[1]) == -1) {
          close(fd[0]);
          close(fd[1]);
          lprint("Error: openpty failed: %s\n", strerror(errno));
          return 0;
        }
#endif

	switch(fork()) {
	 case -1:
           lprint("Error: fork failed: %s\n", strerror(errno));
           close(fd[0]);
           close(fd[1]);
           close(fd0[0]);
           close(fd0[1]);
           return 0;

	 case 0:
           close(fd[0]);
           close(fd0[0]);

		/* Set the DISPLAY */
           if (x_port >= 0) {
#ifdef HAVE_SETENV
             sprintf(buff, "%s:%d.%d", inet_ntoa(our_addr), x_port, x_screen);
             setenv("DISPLAY", buff, 1);
#else
             sprintf(buff, "DISPLAY=%s:%d.%d", inet_ntoa(our_addr), x_port, x_screen);
             putenv(buff);
#endif
           }

           dup2(fd0[1], 0);
           dup2(fd0[1], 1);
           dup2(fd[1], 2);
           for (s = 3; s <= 255; s++)
             close(s);

           execlp("rsh","rsh","-l", user, host, args, NULL);

           /* Ooops, failed, let's tell the user why */

           sprintf(buff, "Error: execlp of %s failed: %s\n",
                   "rsh", strerror(errno));
           write(2, buff, strlen(buff)+1);
           close(0); close(1); close(2); /* XXX */
           exit(1);

        default:
          close(fd[1]);
          close(fd0[1]);
          ns->s=fd[0];
          so->s=fd0[0];

          return 1;
	}
}
#endif