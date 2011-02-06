#pragma once

#include "tpl.h"

tpl_node *msg_node_pack(int msgtype);

//-------------------------------------------------------------------------
// CLOSE
//-------------------------------------------------------------------------

#define MSG_CLOSE		0

//-------------------------------------------------------------------------
// AC (autocompletion)
//-------------------------------------------------------------------------

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

//-------------------------------------------------------------------------
// AC_RESPONSE
//-------------------------------------------------------------------------

#define MSG_AC_RESPONSE		2
#define MSG_AC_RESPONSE_FMT	"A(S(ss))"

struct ac_proposal {
	char *word;
	char *abbr;
};

struct msg_ac_response {
	struct ac_proposal *proposals;
	size_t proposals_n;
};

void msg_ac_response_send(struct msg_ac_response *msg, int sock);
void msg_ac_response_recv(struct msg_ac_response *msg, int sock);
void free_msg_ac_response(struct msg_ac_response *msg);
