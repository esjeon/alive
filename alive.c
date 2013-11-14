#include <assert.h>
#include <errno.h>
#include <pty.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "arg.h"
char *argv0;

/* macros */

#define SERRNO strerror(errno)
#define MAX3(a,b,c) ((a>b && a>c)? a : ((b>a && b>c)? b : c))

#ifndef UNIX_PATH_MAX
#	define UNIX_PATH_MAX sizeof(addr.sun_path)
#endif


/* structs */

typedef enum {
	true = 1,
	false = 0
} bool;

struct options
{
	bool serv;
	char *name;
	char **cmd;
};

enum {
	PKT_SNATCH,
	PKT_INPUT,
	PKT_OUTPUT,
	PKT_WINCH
};

struct packet {
	unsigned char size;
	unsigned char type;
	union {
		struct winsize wsz;
		char bytes[0];
	} load;
};


/* globals */

struct options opt;
struct sockaddr_un addr;


/* prototypes */

static void die(const char*, ...);

static void server_start();
static pid_t server_exec(int*);
static void server_main(int);

static void client_rawterm(bool);
static bool client_signals(int, bool);
static int client_main();



void
die(const char *str, ...)
{
	va_list ap;

	va_start(ap, str);
	vfprintf(stderr, str, ap);
	va_end(ap);

	exit(EXIT_FAILURE);
}


void
server_start()
{
	int lsock;
	pid_t pid;

	if((lsock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		die("Can't create a socket: %s\n", SERRNO);
	}

	if((bind(lsock, (struct sockaddr*)&addr, sizeof(addr))) < 0) {
		die("Can't bind a server address: %s\n", SERRNO);
	}

	if(listen(lsock, 8) < 0) { /* TODO: check this constant */
		die("Can't listen on the server socket: %s\n", SERRNO);
	}

	switch(pid = fork()) {
	case -1:
		die("Can't fork a server process: %s\n", SERRNO);
	case 0:
		server_main(lsock);
	}
}

pid_t
server_exec(int *cmdfd_ret)
{
	struct winsize wsz;
	pid_t pid;
	int fd;

	ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsz);

	switch(pid = forkpty(&fd, NULL, NULL, &wsz)) {
	case -1:
		die("forkpty failed: %s\n", SERRNO);
	case 0:
		/* TODO: exec user-specified command */
		if(opt.cmd) {
			fprintf(stderr, "running %s...\n", opt.cmd[0]);
			execvp(opt.cmd[0], opt.cmd);
		} else {
			fprintf(stderr, "running shell...\n");
			execl("/bin/sh", "-i", NULL);
		}
		die("exec failed: %s\n", SERRNO);
	}

	*cmdfd_ret = fd;
	return pid;
}

void
server_cleanup()
{
	unlink(addr.sun_path);
}

void
server_main(int lsock)
{
	struct packet pkt;
	fd_set rfds;
	pid_t cmdpid;
	int cmdfd;
	int maxfd;
	int sock = -1;
	int ret;

	atexit(server_cleanup);

	cmdpid = server_exec(&cmdfd);

	setsid();

	while(1) {
		FD_ZERO(&rfds);
		FD_SET(lsock, &rfds);
		FD_SET(cmdfd, &rfds);
		if(sock) {
			FD_SET(sock, &rfds);
		}
		maxfd = MAX3(lsock, cmdfd, sock);

		ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
		if(ret < 0) {
			if(errno == EINTR) continue;
			die("server_main: select: %s\n", SERRNO);
		}

		if(FD_ISSET(cmdfd, &rfds)) {
			ret = read(cmdfd, pkt.load.bytes, sizeof(pkt.load));
			if(ret <= 0) {
				goto EXIT;
			}
			/* TODO: buffer */
			if(sock) {
				pkt.type = PKT_OUTPUT;
				pkt.size = ret;
				ret = write(sock, &pkt, sizeof(pkt));
				/* TODO: here */
				if(ret < 0) {
					close(sock);
					sock = -1;
				}
				assert(ret == sizeof(pkt));
			}
		}

		if(FD_ISSET(lsock, &rfds)) {
			if(sock) close(sock);
			sock = accept(lsock, NULL, NULL);
			if(sock < 0) {
				die("server_main: accept: %s\n", SERRNO);
			}
			/* TODO: dump buffer to client */
		}

		/* check client message */
		if(sock && FD_ISSET(sock, &rfds)) do {
			ret = read(sock, &pkt, sizeof(pkt));
			if(ret <= 0) {
				close(sock);
				sock = -1;
				break;
			}
			assert(ret == sizeof(pkt));

			if(pkt.type == PKT_INPUT) {
				write(cmdfd, pkt.load.bytes, pkt.size);
			} else if(pkt.type == PKT_WINCH) {
				ioctl(cmdfd, TIOCSWINSZ, &pkt.load.wsz);
			}
		} while(0);
	}

EXIT:
	close(lsock);
	if(sock)
		close(sock);

	exit(EXIT_SUCCESS);
}

void
client_rawterm(bool raw)
{
	static struct termios bak;
	struct termios cfg;
	
	if(raw) {
		tcgetattr(STDIN_FILENO, &cfg);

		bak = cfg;
		cfmakeraw(&cfg);
		cfg.c_lflag &= ~ECHO;

		tcsetattr(STDIN_FILENO, TCSADRAIN, &cfg);
	} else {
		tcsetattr(STDIN_FILENO, TCSADRAIN, &bak);
	}
}

void
client_onsignal(int sig)
{
	client_signals(sig, true);
}

bool
client_signals(int sig, bool val)
{
	static bool winch = false;
	static bool tstp = false;
	bool *p = NULL;
	bool old;

	switch(sig) {
	case SIGWINCH: p = &winch; break;
	case SIGTSTP : p = &tstp ; break;
	default: return false;
	}

	old = *p;
	*p = val;
	return old;
}

int
client_main()
{
	struct packet pkt;
	struct sigaction sa;
	sigset_t sigs;
	fd_set rfds;
	int sock;
	int ret;

	if((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		die("Can't create a socket: %s\n", SERRNO);
	}
	if((connect(sock, (struct sockaddr*)&addr, sizeof(addr))) < 0) {
		die("Can't connect to the address: %s\n", SERRNO);
	}

	client_rawterm(true);

	sigprocmask(0, NULL, &sigs);
	sigaddset(&sigs, SIGWINCH);
	sigprocmask(SIG_SETMASK, &sigs, NULL);

	sa.sa_handler = client_onsignal;
	sa.sa_mask = sigs;
	sigaction(SIGWINCH, &sa, NULL);

	sigprocmask(0, NULL, &sigs);
	sigdelset(&sigs, SIGWINCH);

	while(1) {
		FD_ZERO(&rfds);
		FD_SET(STDIN_FILENO, &rfds);
		FD_SET(sock, &rfds);

		ret = pselect(sock + 1, &rfds, NULL, NULL, NULL, &sigs);
		if(ret < 0 && errno != EINTR) {
			die("select failed: %s\n", SERRNO);
		} else if(ret < 0 && errno == EINTR) {
			if(client_signals(SIGWINCH, false)) {
				ret = ioctl(STDIN_FILENO, TIOCGWINSZ, &pkt.load.wsz);
				assert(ret == 0);
				pkt.type = PKT_WINCH;
				ret = write(sock, &pkt, sizeof(pkt));
				assert(ret == sizeof(pkt));
			}
			continue;
		}

		if(FD_ISSET(STDIN_FILENO, &rfds)) {
			ret = read(STDIN_FILENO, pkt.load.bytes, sizeof(pkt.load));
			if(ret < 0) {
				die("Can't read the standard input: %s\n", SERRNO);
			} else if(ret == 0) {
				goto EXIT;
			}

			pkt.type = PKT_INPUT;
			pkt.size = ret;
			ret = write(sock, &pkt, sizeof(pkt));
			if(ret < 0) {
				die("Can't write to the socket: %s\n", SERRNO);
			}
		}

		if(FD_ISSET(sock, &rfds)) {
			ret = read(sock, &pkt, sizeof(pkt));
			if(ret < 0) {
				die("Can't read the socket: %s\n", SERRNO);
			} else if(ret == 0) {
				goto EXIT;
			}

			if(pkt.type == PKT_OUTPUT) {
				write(STDOUT_FILENO, pkt.load.bytes, pkt.size);
			}
		}
	}

EXIT:
	client_rawterm(false);
	close(sock);

	exit(EXIT_SUCCESS);
}

void
usage()
{
	die("TODO: usage");
}

int
main(int argc, char *argv[])
{
	opt.serv = true;
	opt.name = NULL;

	ARGBEGIN {
	case 'x':
	case 'a':
		opt.serv = false;
		opt.name = EARGF(usage());
		break;
	case 'n':
		opt.serv = true;
		opt.name = EARGF(usage());
		break;
	} ARGEND;

	if(argc > 0) {
		opt.cmd = &argv[0];
	}

RUN:
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if(opt.name) {
		snprintf(addr.sun_path, UNIX_PATH_MAX,
				"/tmp/alive-%d-%s.sock", getuid(), opt.name);
	} else {
		snprintf(addr.sun_path, UNIX_PATH_MAX,
				"/tmp/alive-%d-%d.sock", getuid(), getpid());
	}

	if(opt.serv) {
		server_start();
	}

	client_main();

	return EXIT_FAILURE;
}

