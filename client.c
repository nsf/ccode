#include "shared.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// for reference
static int create_client_socket();
static int try_connect(int sock, const char *file);
static char *prepend_cwd(const char *file);
static int connect_or_die();
static void run_server_and_wait(const char *path);

//-------------------------------------------------------------------------

static int create_client_socket()
{
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock == -1)
		return -1;

	return sock;
}

static int try_connect(int sock, const char *file)
{
	struct sockaddr_un addr;
	fstr_t addrpath;

	addr.sun_family = AF_UNIX;

	FSTR_INIT_FOR_BUF(&addrpath, addr.sun_path);
	fstr_add_cstr(&addrpath, file);

	return connect(sock, (struct sockaddr*)&addr, sizeof addr);
}

static void run_server_and_wait(const char *path)
{
	if (fork() == 0) {
		pid_t sid;

		// Change file mode mask
		umask(0);
		// new SID for the child, detach from the parent
		sid = setsid();
		if (sid < 0)
			exit(1);
		// chdir (unlock the dir)
		if (chdir("/") < 0)
			exit(1);

		// redirect standard files to /dev/null
		freopen( "/dev/null", "r", stdin);
		freopen( "/dev/null", "w", stdout);
		freopen( "/dev/null", "w", stderr);

		server_main();

		exit(0);
	} else {
		// wait for 10ms up to 100 times (1 second) for socket
		for (int i = 0; i < 100; ++i) {
			usleep(10000);
			if (file_exists(path))
				return;
		}
		fprintf(stderr, "Failed to start a server, can't see socket: %s\n",
			path);
		exit(1);
	}
}

static int connect_or_die()
{
	str_t *path;
	int sock;

	path = get_socket_path();

	if (!file_exists(path->data))
		run_server_and_wait(path->data);

	sock = create_client_socket();
	if (sock == -1) {
		fprintf(stderr, "Error! Failed to create a client socket: %s\n", path->data);
		exit(1);
	}

	if (-1 == try_connect(sock, (char*)path->data)) {
		fprintf(stderr, "Error! Failed to connect to a server at: %s\n", path->data);
		exit(1);
	}
	str_free(path);
	return sock;
}


static char *prepend_cwd(const char *file)
{
	str_t *tmp;
	char cwd[1024];
	char *ret;
	char *pcwd;

	pcwd = getcwd(cwd, 1024);
	if (!pcwd) {
		fprintf(stderr, "Path is too long, more than 1024? wtf, man?\n");
		exit(1);
	}

	tmp = str_from_cstr(pcwd);
	str_add_cstr(&tmp, "/");
	str_add_cstr(&tmp, file);
	ret = strdup(tmp->data);
	str_free(tmp);
	return ret;
}

//-------------------------------------------------------------------------

void client_main(int argc, char **argv)
{
	int sock;

	if (argc < 2) {
		printf("ccode client, commands:\n"
		       "  close\n"
		       "  ac <filename> <line> <col> (+ currently editted buffer as stdin)\n");
		return;
	}

	if (strcmp(argv[1], "close") == 0) {
		sock = connect_or_die();
		tpl_node *tn = msg_node_pack(MSG_CLOSE);
		tpl_dump(tn, TPL_FD, sock);
		tpl_free(tn);
		close(sock);
	} else if (strcmp(argv[1], "ac") == 0) {
		sock = connect_or_die();
		char *end;
		size_t sz;
		struct msg_ac msg;

		if (argc != 5 && argc != 6) {
			fprintf(stderr, "Not enough arguments\n");
			exit(1);
		}

		if (starts_with(argv[2], "/"))
			msg.filename = strdup(argv[2]);
		else
			msg.filename = prepend_cwd(argv[2]);

		msg.line = strtol(argv[3], &end, 10);
		if (*end != '\0') {
			fprintf(stderr, "Failed to parse an int from string: %s\n", argv[3]);
			exit(1);
		}
		msg.col = strtol(argv[4], &end, 10);
		if (*end != '\0') {
			fprintf(stderr, "Failed to parse an int from string: %s\n", argv[4]);
			exit(1);
		}

		// if there is a fifth argument, load currently editted buffer
		// from a file, otherwise use stdin
		if (argc == 6) {
			const char *fn = argv[5];
			if (read_file(&msg.buffer.addr, &sz, fn) == -1) {
				fprintf(stderr, "Error! Failed to read from file: %s\n", fn);
				exit(1);
			}
			msg.buffer.sz = (uint32_t)sz;
		} else {
			if (read_stdin(&msg.buffer.addr, &sz) == -1) {
				fprintf(stderr, "Error! Failed to read from stdin\n");
				exit(1);
			}
			msg.buffer.sz = (uint32_t)sz;
		}

		// send msg type
		tpl_node *tn = msg_node_pack(MSG_AC);
		tpl_dump(tn, TPL_FD, sock);
		tpl_free(tn);

		// send ac msg itself
		tn = msg_ac_node(&msg);
		tpl_pack(tn, 0);
		tpl_dump(tn, TPL_FD, sock);
		tpl_free(tn);

		struct msg_ac_response msg_r;

		msg_ac_response_recv(&msg_r, sock);
		printf("[%d, [", msg_r.partial);
		for (size_t i = 0; i < msg_r.proposals_n; ++i) {
			struct ac_proposal *p = &msg_r.proposals[i];
			printf("{'word':'%s','abbr':'%s'}", p->word, p->abbr);
			if (i != msg_r.proposals_n - 1)
				printf(",");

		}
		printf("]]");
		free_msg_ac_response(&msg_r);
		close(sock);
	} else {
		printf("ccode client, commands:\n"
		       "  close\n"
		       "  ac <filename> <line> <col> (+ currently editted buffer as stdin)\n");
	}

}
