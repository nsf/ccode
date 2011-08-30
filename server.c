#include "shared.h"
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <wordexp.h>
#include <clang-c/Index.h>

struct make_ac_ctx {
	str_t *word;
	str_t *abbr;
	str_t *type;
	str_t *text;
};

static void init_make_ac_ctx(struct make_ac_ctx *ctx);
static void free_make_ac_ctx(struct make_ac_ctx *ctx);

// for reference
static int create_server_socket(const str_t *file);
static void server_loop(int sock);
static void process_ac(int sock);
static void print_completion_result(CXCompletionResult *r);
static int make_ac_proposal(struct make_ac_ctx *ctx,
			    struct ac_proposal *p,
			    CXCompletionResult *r,
			    str_t *fmt);
static str_t *extract_partial(struct msg_ac *msg);
static int isident(int c);
static void try_load_dotccode(wordexp_t *wexp);
static void handle_sigint(int);
static int wordexps_the_same(wordexp_t *a, wordexp_t *b);
static int needs_reparsing(wordexp_t *w, const char *filename);
static void sort_cc_results(CXCompletionResult *results, size_t results_n);
static int code_completion_results_cmp(CXCompletionResult *r1,
				       CXCompletionResult *r2);
static CXString get_result_typed_text(CXCompletionResult *r);
static size_t count_type_chars(CXCompletionResult *r);
static size_t filter_out_cc_results(CXCompletionResult *results,
				    size_t results_n, str_t *partial,
				    str_t **fmt);
static str_t *all_results_fmt(CXCompletionResult *results,
				   size_t results_n);

//-------------------------------------------------------------------------

static CXIndex clang_index;
static CXTranslationUnit clang_tu;
static char *last_filename;
static wordexp_t last_wordexp;
static str_t *sock_path;

#define SERVER_SOCKET_BACKLOG 10
#define MAX_AC_RESULTS 999999
#define MAX_TYPE_CHARS 20
#define WIDTH_SIGNIFICANCE_THRESHOLD 100
#define AUTO_SHUTDOWN_TIME 15

static void init_make_ac_ctx(struct make_ac_ctx *ctx)
{
	ctx->word = str_new(0);
	ctx->abbr = str_new(0);
	ctx->type = str_new(0);
	ctx->text = str_new(0);
}

static void free_make_ac_ctx(struct make_ac_ctx *ctx)
{
	str_free(ctx->word);
	str_free(ctx->abbr);
	str_free(ctx->type);
	str_free(ctx->text);
}

static int needs_reparsing(wordexp_t *w, const char *filename)
{
	if (!last_filename)
		return 1;

	if (strcmp(filename, last_filename) != 0)
		return 1;

	if (!wordexps_the_same(w, &last_wordexp))
		return 1;

	return 0;
}

static int create_server_socket(const str_t *file)
{
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock == -1)
		return -1;

	struct sockaddr_un addr;
	fstr_t addrpath;

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
	struct timeval oneminute = { 60, 0 };
	fd_set sockset;
	int minutes_idle = 0;

	// accepting and dispatching messages
	for (;;) {
		int msg_type;
		tpl_node *tn;
		int incoming;
		int maxfd, result;

		FD_ZERO(&sockset);
		FD_SET(sock, &sockset);
		maxfd = sock;
		result = select(maxfd+1, &sockset, 0, 0, &oneminute);
		if (!result) {
			minutes_idle++;
			if (minutes_idle >= AUTO_SHUTDOWN_TIME)
				return;
			continue;
		}

		minutes_idle = 0;
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

static str_t *extract_partial(struct msg_ac *msg)
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

static void try_load_dotccode(wordexp_t *wexp)
{
	void *buf;
	size_t size;

	wexp->we_wordc = 0;
	wexp->we_wordv = 0;

	if (read_file(&buf, &size, ".ccode") == -1) {
		return;
	}

	// TODO: fstr trim? cstr trim?
	str_t *contents = str_from_cstr_len(buf, (unsigned int)size);
	str_trim(contents);

	wordexp(contents->data, wexp, 0);
	str_free(contents);
	free(buf);
}

static void change_dir(const char *filename)
{
	str_t *dir, *fn;
	fn = str_from_cstr(filename);
	dir = str_split_path(fn, 0);
	chdir(dir->data);
	str_free(dir);
	str_free(fn);
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

	change_dir(msg.filename);
	try_load_dotccode(&flags);

	str_t *partial = extract_partial(&msg);

	if (partial)
		msg.col -= partial->len;

	if (needs_reparsing(&flags, msg.filename)) {
		if (clang_tu)
			clang_disposeTranslationUnit(clang_tu);

		clang_tu = clang_parseTranslationUnit(clang_index, msg.filename,
						      (char const * const *)flags.we_wordv,
						      flags.we_wordc,
						      &unsaved, 1,
						      clang_defaultEditingTranslationUnitOptions());
		if (last_filename)
			free(last_filename);
		if (last_wordexp.we_wordv)
			wordfree(&last_wordexp);
		last_filename = strdup(msg.filename);
		last_wordexp = flags;
	}

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
		struct make_ac_ctx ctx;
		str_t *fmt;

		init_make_ac_ctx(&ctx);
		msg_r.proposals_n = filter_out_cc_results(results->Results,
							  results->NumResults,
							  partial, &fmt);
		sort_cc_results(results->Results, msg_r.proposals_n);
		if (msg_r.proposals_n > MAX_AC_RESULTS)
			msg_r.proposals_n = MAX_AC_RESULTS;
		msg_r.proposals = malloc(sizeof(struct ac_proposal) *
					 msg_r.proposals_n);

		int cur = 0;
		for (int i = 0; i < msg_r.proposals_n; ++i) {
			int added;
			added = make_ac_proposal(&ctx,
						 &msg_r.proposals[cur],
						 &results->Results[i],
						 fmt);
			if (added)
				cur++;
		}
		msg_r.proposals_n = cur;
		free_make_ac_ctx(&ctx);
		str_free(fmt);
	}

	if (partial)

		str_free(partial);
	clang_disposeCodeCompleteResults(results);

	msg_ac_response_send(&msg_r, sock);
	free_msg_ac_response(&msg_r);
}

static int code_completion_results_cmp(CXCompletionResult *r1,
				       CXCompletionResult *r2)
{
	int prio1 = clang_getCompletionPriority(r1->CompletionString);
	int prio2 = clang_getCompletionPriority(r2->CompletionString);
	if (prio1 != prio2)
		return prio1 - prio2;

	CXString r1t = get_result_typed_text(r1);
	CXString r2t = get_result_typed_text(r2);
	int cmp = strcmp(clang_getCString(r1t),
			 clang_getCString(r2t));
	clang_disposeString(r1t);
	clang_disposeString(r2t);
	return cmp;
}

static CXString get_result_typed_text(CXCompletionResult *r)
{
	unsigned int chunks_n = clang_getNumCompletionChunks(r->CompletionString);
	for (unsigned int i = 0; i < chunks_n; ++i) {
		enum CXCompletionChunkKind kind;
		kind = clang_getCompletionChunkKind(r->CompletionString, i);
		if (kind == CXCompletionChunk_TypedText)
			return clang_getCompletionChunkText(r->CompletionString, i);
	}
	CXString empty = {0,0};
	return empty;
}

static size_t count_type_chars(CXCompletionResult *r)
{
	unsigned int chars = 0;
	unsigned int chunks_n = clang_getNumCompletionChunks(r->CompletionString);
	for (unsigned int i = 0; i < chunks_n; ++i) {
		enum CXCompletionChunkKind kind;
		kind = clang_getCompletionChunkKind(r->CompletionString, i);
		if (kind == CXCompletionChunk_ResultType) {
			CXString s = clang_getCompletionChunkText(r->CompletionString, i);
			chars += strlen(clang_getCString(s));
			clang_disposeString(s);
		}
	}
	return chars;
}

static void sort_cc_results(CXCompletionResult *results, size_t results_n)
{
	qsort(results, results_n, sizeof(CXCompletionResult),
	      (int (*)(const void*,const void*))code_completion_results_cmp);
}

static str_t *all_results_fmt(CXCompletionResult *results,
				   size_t results_n)
{
	if (results_n > WIDTH_SIGNIFICANCE_THRESHOLD)
		return str_printf("%%%ds %%s", MAX_TYPE_CHARS);

	size_t maxl = 0;
	for (size_t i = 0; i < results_n; ++i) {
		if (maxl != MAX_TYPE_CHARS) {
			size_t l = count_type_chars(&results[i]);
			if (l > maxl) {
				if (l > MAX_TYPE_CHARS)
					maxl = MAX_TYPE_CHARS;
				else
					maxl = l;
			}
		}
	}
	return str_printf("%%%ds %%s", maxl);
}

static size_t filter_out_cc_results(CXCompletionResult *results,
				    size_t results_n,
				    str_t *partial,
				    str_t **fmt)
{
	if (!partial) {
		*fmt = all_results_fmt(results, results_n);
		return results_n;
	}

	size_t maxl = 0;
	size_t cur = 0;
	for (size_t i = 0; i < results_n; ++i) {
		CXString s = get_result_typed_text(&results[i]);
		if (!s.data)
			continue;
		if (!starts_with(clang_getCString(s), partial->data)) {
			clang_disposeString(s);
			continue;
		}
		clang_disposeString(s);

		CXCompletionResult tmp = results[cur];
		results[cur] = results[i];
		results[i] = tmp;

		if (maxl != MAX_TYPE_CHARS) {
			size_t l = count_type_chars(&results[cur]);
			if (l > maxl) {
				if (l > MAX_TYPE_CHARS)
					maxl = MAX_TYPE_CHARS;
				else
					maxl = l;
			}
		}

		cur++;
	}
	*fmt = str_printf("%%%ds %%s", maxl);
	return cur;
}

static int make_ac_proposal(struct make_ac_ctx *ctx, struct ac_proposal *p,
			    CXCompletionResult *r, str_t *fmt)
{
	unsigned int chunks_n;

	chunks_n = clang_getNumCompletionChunks(r->CompletionString);
	str_clear(ctx->word);
	str_clear(ctx->abbr);
	str_clear(ctx->type);
	str_clear(ctx->text);

	for (unsigned int i = 0; i < chunks_n; ++i) {
		enum CXCompletionChunkKind kind;
		CXString s;

		kind = clang_getCompletionChunkKind(r->CompletionString, i);
		s = clang_getCompletionChunkText(r->CompletionString, i);
		switch (kind) {
		case CXCompletionChunk_ResultType:
			str_add_printf(&ctx->type, "%s", clang_getCString(s));
			break;
		case CXCompletionChunk_TypedText:
			str_add_cstr(&ctx->word, clang_getCString(s));
		default:
			str_add_cstr(&ctx->text, clang_getCString(s));
			break;
		}
		clang_disposeString(s);
	}

	if (ctx->type->len > MAX_TYPE_CHARS) {
		ctx->type->len = MAX_TYPE_CHARS-1;
		str_add_cstr(&ctx->type, "…");
	}
	str_add_printf(&ctx->abbr, fmt->data,
		       ctx->type->data, ctx->text->data);

	p->abbr = strdup(ctx->abbr->data);
	p->word = strdup(ctx->word->data);
	return 1;
}

static void handle_sigint(int unused)
{
	unlink(sock_path->data);
	exit(0);
}

static int wordexps_the_same(wordexp_t *a, wordexp_t *b)
{
	if (a->we_wordc != b->we_wordc)
		return 0;

	for (size_t i = 0; i < a->we_wordc; i++) {
		if (strcmp(a->we_wordv[i], b->we_wordv[i]) != 0)
			return 0;
	}
	return 1;
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
