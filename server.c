#include "shared.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <wordexp.h>
#include <clang-c/Index.h>

// for reference
static int create_server_socket(const struct str *file);
static void server_loop(int sock);
static void process_ac(int sock);
static void print_completion_result(CXCompletionResult *r);
static int make_ac_proposal(struct ac_proposal *p,
			    CXCompletionResult *r,
			    struct str *partial);
static struct str *extract_partial(struct msg_ac *msg);
static int isident(int c);
static void try_load_dotccode(wordexp_t *wexp, const char *filename);
static void handle_sigint(int);

//-------------------------------------------------------------------------

static CXIndex clang_index;
static CXTranslationUnit clang_tu;
static char *last_filename;
static struct str *sock_path;

#define SERVER_SOCKET_BACKLOG 10

static int create_server_socket(const struct str *file)
{
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock == -1)
		return -1;

	struct fstr addrpath;
	struct sockaddr_un addr;
	addr.sun_family = AF_UNIX;

	FSTR_INIT_FOR_BUF(&addrpath, addr.sun_path);
	fstr_add_str(&addrpath, file);

	if (-1 == bind(sock, (struct sockaddr*)&addr, sizeof addr))
		return -1;

	if (-1 == listen(sock, SERVER_SOCKET_BACKLOG))
		return -1;

	return sock;
}

static void server_loop(int sock)
{
	// accepting and dispatching messages
	for (;;) {
		int msg_type;
		tpl_node *tn;
		int incoming;

		incoming = accept(sock, 0, 0);
		if (incoming == -1) {
			fprintf(stderr, "Error! Failed to accept an incoming connection.\n");
			exit(1);
		}

		tn = tpl_map("i", &msg_type);
		tpl_load(tn, TPL_FD, incoming);
		tpl_unpack(tn, 0);
		tpl_free(tn);

		switch (msg_type) {
		case MSG_CLOSE:
			close(incoming);
			return;
		case MSG_AC:
			process_ac(incoming);
			break;
		default:
			;
		}

		close(incoming);
	}
}

static struct str *extract_partial(struct msg_ac *msg)
{
	char *cursor;
	char *c = msg->buffer.addr;
	char *end = msg->buffer.addr + msg->buffer.sz;

	for (int line = 1; line < msg->line; line++) {
		while (1) {
			if (c == end)
				return 0;
			if (*c == '\n') {
				c++;
				break;
			}
			c++;
		}
	}

	cursor = c + (msg->col - 1);
	c += msg->col - 2;
	while (isident(*c))
		c--;
	c++;

	if (c == cursor)
		return 0;

	return str_from_cstr_len(c, cursor - c);
}

static int isident(int c)
{
	if (isalnum(c) || c == '_')
		return 1;
	return 0;
}

static void try_load_dotccode(wordexp_t *wexp, const char *filename)
{
	void *buf;
	size_t size;
	struct str *fn;
	struct str *dotccode;

	wexp->we_wordc = 0;
	wexp->we_wordv = 0;

	fn = str_from_cstr(filename);
	dotccode = str_path_split(fn, 0);
	str_add_cstr(&dotccode, "/.ccode");

	if (read_file(&buf, &size, dotccode->data) == -1) {
		str_free(fn);
		str_free(dotccode);
		return;
	}

	// TODO: fstr trim? cstr trim?
	struct str *contents = str_from_cstr_len(buf, (unsigned int)size);
	str_trim(contents);

	wordexp(contents->data, wexp, 0);
	str_free(fn);
	str_free(dotccode);
	str_free(contents);
	free(buf);
}

static void process_ac(int sock)
{
	tpl_node *tn;
	struct msg_ac msg;
	wordexp_t flags;

	tn = msg_ac_node(&msg);
	tpl_load(tn, TPL_FD, sock);
	tpl_unpack(tn, 0);
	tpl_free(tn);

	struct CXUnsavedFile unsaved = {
		msg.filename,
		msg.buffer.addr,
		msg.buffer.sz
	};

	try_load_dotccode(&flags, msg.filename);

	struct str *partial = extract_partial(&msg);

	//printf("AUTOCOMPLETION:\n");
	if (partial) {
		msg.col -= partial->len;
		//printf(" partial: '%s'\n", partial->data);
	}
	//printf(" buffer size: %d\n", msg.buffer.sz);
	//printf(" location: %s:%d:%d\n", msg.filename, msg.line, msg.col);

	if (!last_filename || strcmp(last_filename, msg.filename) != 0) {
		if (clang_tu)
			clang_disposeTranslationUnit(clang_tu);

		clang_tu = clang_parseTranslationUnit(clang_index, msg.filename,
						      (char const * const *)flags.we_wordv,
						      flags.we_wordc,
						      &unsaved, 1,
						      clang_defaultEditingTranslationUnitOptions());
		if (last_filename)
			free(last_filename);
		last_filename = strdup(msg.filename);
	}

	if (flags.we_wordv)
		wordfree(&flags);

	// diag
	/*
	for (int i = 0, n = clang_getNumDiagnostics(clang_tu); i != n; ++i) {
		CXDiagnostic diag = clang_getDiagnostic(clang_tu, i);
		CXString string = clang_formatDiagnostic(diag, clang_defaultDiagnosticDisplayOptions());
		fprintf(stderr, "%s\n", clang_getCString(string));
		clang_disposeString(string);
		clang_disposeDiagnostic(diag);
	}
	*/

	CXCodeCompleteResults *results;
	results = clang_codeCompleteAt(clang_tu, msg.filename, msg.line, msg.col,
				       &unsaved, 1,
				       CXCodeComplete_IncludeMacros);
	free_msg_ac(&msg);

	// diag
	/*
	for (int i = 0, n = clang_codeCompleteGetNumDiagnostics(results); i != n; ++i) {
		CXDiagnostic diag = clang_codeCompleteGetDiagnostic(results, i);
		CXString string = clang_formatDiagnostic(diag, clang_defaultDiagnosticDisplayOptions());
		fprintf(stderr, "%s\n", clang_getCString(string));
		clang_disposeString(string);
		clang_disposeDiagnostic(diag);
	}
	*/

	struct msg_ac_response msg_r = { (partial) ? partial->len : 0, 0, 0 };

	if (results) {
		clang_sortCodeCompletionResults(results->Results,
						results->NumResults);

		msg_r.proposals_n = results->NumResults;
		msg_r.proposals = malloc(sizeof(struct ac_proposal) *
					 msg_r.proposals_n);

		int cur = 0;
		for (int i = 0; i < msg_r.proposals_n; ++i) {
			int added;
			added = make_ac_proposal(&msg_r.proposals[cur],
						 &results->Results[i],
						 partial);
			if (added)
				cur++;
		}
		msg_r.proposals_n = cur;

		//printf(" results: %d\n", msg_r.proposals_n);
		//printf("--------------------------------------------------\n");
	}

	if (partial)
		str_free(partial);
	clang_disposeCodeCompleteResults(results);

	msg_ac_response_send(&msg_r, sock);
	free_msg_ac_response(&msg_r);
}


static int make_ac_proposal(struct ac_proposal *p,
			    CXCompletionResult *r,
			    struct str *partial)
{
	struct str *abbr, *word;
	unsigned int chunks_n;

	chunks_n = clang_getNumCompletionChunks(r->CompletionString);
	word = str_new(0);
	abbr = str_new(0);

	for (unsigned int i = 0; i < chunks_n; ++i) {
		enum CXCompletionChunkKind kind;
		CXString s;

		kind = clang_getCompletionChunkKind(r->CompletionString, i);
		s = clang_getCompletionChunkText(r->CompletionString, i);
		switch (kind) {
		case CXCompletionChunk_ResultType:
			//str_add_printf(&abbr, "%s ", clang_getCString(s));
			break;
		case CXCompletionChunk_TypedText:
			str_add_cstr(&word, clang_getCString(s));
			if (partial && !starts_with(word->data, partial->data)) {
				str_free(word);
				str_free(abbr);
				clang_disposeString(s);
				return 0;
			}
		default:
			str_add_cstr(&abbr, clang_getCString(s));
			break;
		}
		clang_disposeString(s);
	}

	p->abbr = strdup(abbr->data);
	p->word = strdup(word->data);
	str_free(word);
	str_free(abbr);
	return 1;
}

static void handle_sigint(int unused)
{
	unlink(sock_path->data);
	exit(0);
}

void server_main()
{
	struct sigaction sa;
	int sock;

	sock_path = get_socket_path();
	sock = create_server_socket(sock_path);
	if (sock == -1) {
		fprintf(stderr, "Error! Failed to create a server socket: %s\n",
			sock_path->data);
		exit(1);
	}

	sa.sa_handler = handle_sigint;
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, 0);

	clang_index = clang_createIndex(0, 0);
	server_loop(sock);
	if (clang_tu)
		clang_disposeTranslationUnit(clang_tu);
	clang_disposeIndex(clang_index);

	close(sock);
	unlink(sock_path->data);
	str_free(sock_path);
}
