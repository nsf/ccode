#pragma once

#include "strstr.h"
#include "tpl.h"

//-------------------------------------------------------------------------
// Protocol
//-------------------------------------------------------------------------

tpl_node *msg_node_pack(int msgtype);

// CLOSE

#define MSG_CLOSE		0

// AC (autocompletion)

#define MSG_AC			1
#define MSG_AC_FMT		"Bsii"

struct msg_ac {
	tpl_bin buffer;
	char *filename;
	int line;
	int col;
};

tpl_node *msg_ac_node(struct msg_ac *msg);
void free_msg_ac(struct msg_ac *msg);

// AC_RESPONSE

#define MSG_AC_RESPONSE		2
#define MSG_AC_RESPONSE_FMT	"iA(S(ss))"

struct ac_proposal {
	char *word;
	char *abbr;
};

struct msg_ac_response {
	int partial;
	struct ac_proposal *proposals;
	size_t proposals_n;
};

void msg_ac_response_send(struct msg_ac_response *msg, int sock);
void msg_ac_response_recv(struct msg_ac_response *msg, int sock);
void free_msg_ac_response(struct msg_ac_response *msg);

//-------------------------------------------------------------------------
// Misc
//-------------------------------------------------------------------------

int file_exists(const char *filename);
int starts_with(const char *s1, const char *s2);

// read file to a newly allocated buf, 0 on success, -1 on error
int read_file(void **out, size_t *size, const char *filename);
int read_stdin(void **out, size_t *size);

str_t *get_socket_path();

void client_main(int argc, char **argv);
void server_main();
