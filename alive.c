#include <assert.h>
#include <errno.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* macros */

#define SOCKNAME "/tmp/alive.sock"


/* structs */

typedef enum {
	true = 1,
	false = 0
} bool;

struct config
{
	bool spawn_server;
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

struct config cfg;
struct sockaddr_un addr;


/* prototypes */

static int server_start();
static pid_t server_exec(int*);
static int server_main(int);

static int client_connect();
static int client_rawterm(bool);
static bool client_signals(int, bool);
static int client_main();

static void parse_args(int, char*[]);



static inline
int
max(a, b)
	int a, b;
{
	return (a > b)? a : b;
}
static inline
int
max3(a, b, c)
	int a, b, c;
{
	return max(max(a,b),c);
}


int
server_start()
{
	int lsock;
	pid_t pid;
	int ret;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCKNAME, 108);

	if((lsock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		perror(__func__": socket");
		return -1;
	}

	if((bind(lsock, (struct sockaddr*)&addr, sizeof(addr))) < 0) {
		perror(__func__": bind");
		return -1;
	}

	if(listen(lsock, 8) < 0) { /* TODO: check this constant */
		perror(__func__": listen");
		return -1;
	}

	if(pid = fork() < 0) {
		perror(__func__": fork");
		return -1;
	}

	if (pid == 0) {
		ret = server_main(lsock);
		exit(ret);
	}

	return 0;
}

pid_t
server_exec(cmdfd_ret)
	int *cmdfd_ret;
{
	struct winsize wsz;
	pid_t pid;
	int fd;

	ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsz);

	if((pid = forkpty(&fd, NULL, NULL, &wsz)) < 0) {
		perror(__func__": forkpty");
		return -1;
	}
	if(pid == 0) {
		/* TODO: exec user-specified command */
		execl("/bin/sh", "-i", NULL);
		perror("server_exec: execl");
		exit(EXIT_FAILURE);
	}

	*cmdfd_ret = fd;
	return pid;
}

int
server_main(lsock)
	int lsock;
{
	struct packet pkt;
	fd_set rfds;
	pid_t cmdpid;
	int cmdfd;
	int maxfd;
	int sock = -1;
	int ret;

	if((cmdpid = server_exec(&cmdfd)) < 0) {
		return EXIT_FAILURE;
	}

	while(1) {
		FD_ZERO(&rfds);
		FD_SET(lsock, &rfds);
		FD_SET(cmdfd, &rfds);
		FD_SET(sock, &rfds);
		maxfd = max3(lsock, cmdfd, sock);

		ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
		if(ret < 0) {
			if(errno == EINTR) continue;
			perror("server_main: select");
			goto FAIL;
		}

		if(FD_ISSET(cmdfd, &rfds)) {
			ret = read(cmdfd, pkt.load.bytes, sizeof(pkt.load));
			if(ret < 0) {
				perror("server_main: read");
				goto FAIL;
			} else if(ret == 0) {
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
				perror("server_main: accept");
				goto FAIL;
			}
			/* TODO: dump buffer to client */
		}

		/* check client message */
		if(FD_ISSET(sock, &rfds)) do {
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
	if(sock) close(sock);
	close(lsock);
	return EXIT_SUCCESS;
FAIL:
	return EXIT_FAILURE;
}

int
client_connect()
{
	int sock;

	if((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		perror("client_connect: socket");
		return -1;
	}

	if((connect(sock, (struct sockaddr*)&addr, sizeof(addr))) < 0) {
		perror("client_connect: connect");
		close(sock);
		return -1;
	}

	return sock;
}

int
client_rawterm(raw)
	bool raw;
{
	static struct termios bak;
	struct termios cfg;
	
	if(raw) {
		if(tcgetattr(STDIN_FILENO, &cfg) < 0) {
			perror("client_rawterm: tcgetattr");
			return -1;
		}

		bak = cfg;
		cfmakeraw(&cfg);
		cfg.c_lflag &= ~ECHO;

		if(tcsetattr(STDIN_FILENO, TCSADRAIN, &cfg) < 0) {
			perror("client_rawterm: tcgetattr");
			return -1;
		}
	} else {
		tcsetattr(STDIN_FILENO, TCSADRAIN, &bak);
		/* ignore error */
	}

	return 0;
}

void
client_onsignal(sig)
	int sig;
{
	client_signals(sig, true);
}

bool
client_signals(sig, val)
	int sig;
	bool val;
{
	static bool winch = false;
	static bool tstp = false;
	bool *p = NULL;
	bool old;

	switch(sig) {
	case SIGWINCH: p = &winch; break;
	case SIGTSTP : p = &tstp ; break;
	}

	if(!p) return false;

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
	int maxfd;
	int sock;
	int ret;

	if((sock = client_connect()) < 0)
		return EXIT_FAILURE;

	if(client_rawterm(true) < 0)
		return EXIT_FAILURE;

	sigprocmask(0, NULL, &sigs);
	sigaddset(&sigs, SIGWINCH);
	sigaddset(&sigs, SIGTSTP);
	sigprocmask(SIG_SETMASK, &sigs, NULL);

	sa.sa_handler = client_onsignal;
	sa.sa_mask = sigs;
	sigaction(SIGWINCH, &sa, NULL);
	sigaction(SIGTSTP, &sa, NULL);

	sigprocmask(0, NULL, &sigs);
	sigdelset(&sigs, SIGWINCH);
	sigdelset(&sigs, SIGTSTP);

	while(1) {
		FD_ZERO(&rfds);
		FD_SET(STDIN_FILENO, &rfds);
		FD_SET(sock, &rfds);
		maxfd = sock;

		ret = pselect(maxfd + 1, &rfds, NULL, NULL, NULL, &sigs);
		if(ret < 0 && errno != EINTR) {
			perror("client_main: select");
			goto FAIL;
		} else if(ret < 0 && errno == EINTR) {
			if(client_signals(SIGWINCH, false)) {
				ret = ioctl(STDIN_FILENO, TIOCGWINSZ, &pkt.load.wsz);
				assert(ret == 0);
				pkt.type = PKT_WINCH;
				ret = write(sock, &pkt, sizeof(pkt));
				assert(ret == sizeof(pkt));
			}
			if(client_signals(SIGTSTP, false)) {
				/* TODO: pause */
			}
			continue;
		}

		if(FD_ISSET(STDIN_FILENO, &rfds)) {
			ret = read(STDIN_FILENO, pkt.load.bytes, sizeof(pkt.load));
			if(ret < 0) {
				perror("client_main: read(STDIN)");
				goto FAIL;
			} else if(ret == 0) {
				goto EXIT;
			}

			pkt.type = PKT_INPUT;
			pkt.size = ret;
			ret = write(sock, &pkt, sizeof(pkt));
			if(ret < 0) {
				perror("client_main: write(sock)");
				goto FAIL;
			}
		}

		if(FD_ISSET(sock, &rfds)) {
			ret = read(sock, &pkt, sizeof(pkt));
			if(ret < 0) {
				perror("client_main: read(sock)");
				goto FAIL;
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
	return EXIT_SUCCESS;
FAIL:
	return EXIT_FAILURE;
}

void
parse_args(argc, argv)
	int argc;
	char *argv[];
{
	/* TODO: stub */
	cfg.spawn_server = true;
}

int
main(argc, argv)
	int argc;
	char *argv[];
{
	/* fill `cfg` */
	parse_args(argc, argv);

	if(cfg.spawn_server) {
		if(server_start() < 0) {
			fprintf(stderr, "Cannot create server\n");
			return EXIT_FAILURE;
		}
		/* NOTE: server process does not reach here,
		 *       while client does */
	}

	return client_main();
}

