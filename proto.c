#include "shared.h"
#include <stdlib.h>

tpl_node *msg_node_pack(int msgtype)
{
	tpl_node *tn = tpl_map("i", &msgtype);
	tpl_pack(tn, 0);
	return tn;
}

//-------------------------------------------------------------------------

tpl_node *msg_ac_node(struct msg_ac *msg)
{
	tpl_node *tn = tpl_map(MSG_AC_FMT,
			       &msg->buffer,
			       &msg->filename,
			       &msg->line,
			       &msg->col);
	return tn;
}

void free_msg_ac(struct msg_ac *msg)
{
	free(msg->buffer.addr);
	free(msg->filename);
}

//-------------------------------------------------------------------------

void msg_ac_response_send(struct msg_ac_response *msg, int sock)
{
	struct ac_proposal prop;
	tpl_node *tn;

	tn = tpl_map(MSG_AC_RESPONSE_FMT,
		     &msg->partial,
		     &prop);
	tpl_pack(tn, 0);
	for (size_t i = 0; i < msg->proposals_n; ++i) {
		prop = msg->proposals[i];
		tpl_pack(tn, 1);
	}
	tpl_dump(tn, TPL_FD, sock);
	tpl_free(tn);
}

void msg_ac_response_recv(struct msg_ac_response *msg, int sock)
{
	struct ac_proposal prop;
	tpl_node *tn;

	tn = tpl_map(MSG_AC_RESPONSE_FMT,
		     &msg->partial,
		     &prop);
	tpl_load(tn, TPL_FD, sock);
	tpl_unpack(tn, 0);
	msg->proposals_n = tpl_Alen(tn, 1);
	msg->proposals = malloc(sizeof(struct ac_proposal) *
				msg->proposals_n);
	for (size_t i = 0; i < msg->proposals_n; ++i) {
		tpl_unpack(tn, 1);
		msg->proposals[i] = prop;
	}
	tpl_free(tn);
}

void free_msg_ac_response(struct msg_ac_response *msg)
{
	for (size_t i = 0; i < msg->proposals_n; ++i) {
		struct ac_proposal *p = &msg->proposals[i];
		free(p->abbr);
		free(p->word);
	}
	if (msg->proposals)
		free(msg->proposals);
}
