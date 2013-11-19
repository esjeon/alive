#define _POSIX_C_SOURCE 200112L
#define _BSD_SOURCE
#define __BSD_VISIBLE 1

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#if   defined(__linux)
#	include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#	include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#	include <libutil.h>
#endif

#include "arg.h"
char *argv0;

/* macros */

#define NAMELEN 15
#define SERRNO strerror(errno)
#define MAX3(a,b,c) ((a>b && a>c)? a : ((b>a && b>c)? b : c))

#ifndef UNIX_PATH_MAX
#	define UNIX_PATH_MAX sizeof(addr.sun_path)
#endif


/* structs */

struct options
{
	bool serv;
	char name[NAMELEN+1];
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
		char bytes[sizeof(struct winsize)];
	} load;
};

/* config */
#include "config.h"

/* globals */

struct options opt;
struct sockaddr_un addr;


/* prototypes */

static void die(const char*, ...);

static void server_start();
static void server_cleanup();
static pid_t server_exec(int*);
static void server_main(int);

static void client_rawterm(bool);
static void client_onsignal(int);
static bool client_signals(int, bool);
static int client_main();

static void usage();


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
	char *dir;

	if((lsock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		die("Can't create a socket: %s\n", SERRNO);
	}

	dir = malloc(256 * sizeof(char));
	snprintf(dir, 256, "/tmp/alive-%d", getuid());
	mkdir(dir, S_IRUSR | S_IWUSR | S_IXUSR);
	free(dir);

	if((bind(lsock, (struct sockaddr*)&addr, sizeof(addr))) < 0) {
		die("Can't bind a server address: %s\n", SERRNO);
	}

	if(listen(lsock, 8) < 0) {
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
	char *oldlst;
	char *lst;

	ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsz);

	switch(pid = forkpty(&fd, NULL, NULL, &wsz)) {
	case -1:
		die("forkpty failed: %s\n", SERRNO);
	case 0:
		if((oldlst = getenv(envvar)) == NULL) {
			lst = opt.name;
		} else {
			lst = calloc(strlen(oldlst) + NAMELEN + 2, sizeof(char));
			strcpy(lst, oldlst);
			strcat(lst, envsep);
			strncat(lst, opt.name, NAMELEN);
		}
		setenv(envvar, lst, 1);

		if(opt.cmd) {
			execvp(opt.cmd[0], opt.cmd);
		} else {
			execl("/bin/sh", "-i", NULL);
		}
		setenv(envvar, oldlst, 1);
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
	int sock = 0;
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
				if(ret < 0) {
					close(sock);
					sock = 0;
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
				sock = 0;
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
	char *str;
	char *tok;

	opt.serv = true;
	snprintf(opt.name, NAMELEN+1, "%d", getpid());
	opt.cmd = NULL;

	ARGBEGIN {
	case 'x':
	case 'a':
		opt.serv = false;
		strncpy(opt.name, EARGF(usage()), NAMELEN);
		break;
	case 'n':
		opt.serv = true;
		strncpy(opt.name, EARGF(usage()), NAMELEN);
		break;
	default:
		usage();
	} ARGEND;

	if(argc > 0) {
		opt.cmd = &argv[0];
	}

RUN:
	for(str = opt.name; *str != '\0' && isalnum(*str); str++);
	if(*str != '\0') {
		die("session name must be alphanumeric\n");
	}

	if( (str = getenv(envvar)) != NULL ) {
		str = strdup(str);
		tok = strtok(str, envsep);
		while(tok != NULL) {
			if(strncmp(opt.name, tok, NAMELEN) == 0) {
				die("cannot attach to a session recursively\n");
			}
			tok = strtok(NULL, envsep);
		}
		free(str);
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, UNIX_PATH_MAX,
			"/tmp/alive-%d/%s", getuid(), opt.name);

	if(opt.serv) {
		server_start();
	}

	client_main();

	return EXIT_FAILURE;
}

