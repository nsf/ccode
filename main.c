#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wordexp.h>
#include <ctype.h>
#include <clang-c/Index.h>
#include "strstr.h"
#include "tpl.h"
#include "proto.h"

//-------------------------------------------------------------------------
// prototypes, just for quick reference
//-------------------------------------------------------------------------

static int starts_with(const char *s1, const char *s2);

// read file to a newly allocated buf, 0 on success, -1 on error
static int read_file(void **out, size_t *size, const char *filename);
static int read_stdin(void **out, size_t *size);

static struct str *get_socket_path();

static void client_main(int argc, char **argv);
static int create_client_socket();
static int try_connect(int sock, const char *file);
static char *prepend_cwd(const char *file);

static void server_main(int argc, char **argv);
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

static CXIndex clang_index;
static CXTranslationUnit clang_tu;
static char *last_filename;
static struct str *sock_path;

//-------------------------------------------------------------------------

static int starts_with(const char *s1, const char *s2)
{
	while (*s2) if (*s1++ != *s2++) return 0; return 1;
}

static int read_stdin(void **out, size_t *size)
{
	size_t read_n = 0;
	size_t alloc_n = 1024;
	void *buf = 0;

	*out = buf;
	*size = read_n;

	while (1) {
		if (feof(stdin))
			break;
		if (ferror(stdin)) {
			free(buf);
			return -1;
		}
		buf = realloc(buf, alloc_n);
		alloc_n *= 2;
		size_t n = fread(buf+read_n, 1, 1024, stdin);
		read_n += n;
	}

	*out = buf;
	*size = read_n;
	return 0;
}

static int read_file(void **out, size_t *size, const char *filename)
{
	struct stat st;
	FILE *f = fopen(filename, "r");
	if (!f)
		return -1;

	if (-1 == fstat(fileno(f), &st)) {
		fclose(f);
		return -1;
	}

	*size = st.st_size;
	*out = malloc(*size);
	if (*size != fread(*out, 1, *size, f)) {
		fclose(f);
		free(*out);
		return -1;
	}

	return 0;
}

static struct str *get_socket_path()
{
	char *user = getenv("USER");
	if (user)
		return str_printf("/tmp/ccode-server.%s", user);
	else
		return str_from_cstr("/tmp/ccode-server");
}

//-------------------------------------------------------------------------

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

	printf("AUTOCOMPLETION:\n");
	if (partial) {
		msg.col -= partial->len;
		printf(" partial: '%s'\n", partial->data);
	}
	printf(" buffer size: %d\n", msg.buffer.sz);
	printf(" location: %s:%d:%d\n", msg.filename, msg.line, msg.col);

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
	for (int i = 0, n = clang_getNumDiagnostics(clang_tu); i != n; ++i) {
		CXDiagnostic diag = clang_getDiagnostic(clang_tu, i);
		CXString string = clang_formatDiagnostic(diag, clang_defaultDiagnosticDisplayOptions());
		fprintf(stderr, "%s\n", clang_getCString(string));
		clang_disposeString(string);
		clang_disposeDiagnostic(diag);
	}

	CXCodeCompleteResults *results;
	results = clang_codeCompleteAt(clang_tu, msg.filename, msg.line, msg.col,
				       &unsaved, 1,
				       CXCodeComplete_IncludeMacros);
	free_msg_ac(&msg);

	// diag
	for (int i = 0, n = clang_codeCompleteGetNumDiagnostics(results); i != n; ++i) {
		CXDiagnostic diag = clang_codeCompleteGetDiagnostic(results, i);
		CXString string = clang_formatDiagnostic(diag, clang_defaultDiagnosticDisplayOptions());
		fprintf(stderr, "%s\n", clang_getCString(string));
		clang_disposeString(string);
		clang_disposeDiagnostic(diag);
	}

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

		printf(" results: %d\n", msg_r.proposals_n);
		printf("--------------------------------------------------\n");
	}

	if (partial)
		str_free(partial);
	clang_disposeCodeCompleteResults(results);

	msg_ac_response_send(&msg_r, sock);
	free_msg_ac_response(&msg_r);
}

static void handle_sigint(int unused)
{
	unlink(sock_path->data);
	exit(0);
}

static void server_main(int argc, char **argv)
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
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof addr.sun_path, "%s", file);
	addr.sun_path[sizeof addr.sun_path - 1] = '\0';

	return connect(sock, (struct sockaddr*)&addr, sizeof addr);
}

static void client_main(int argc, char **argv)
{
	if (argc < 2) {
		printf("ccode client, commands:\n"
		       "  close\n"
		       "  ac <filename> <line> <col> (+ currently editted buffer as stdin)\n");
		return;
	}

	struct str *path = get_socket_path();
	int sock = create_client_socket();
	if (sock == -1) {
		fprintf(stderr, "Error! Failed to create a client socket: %s\n", path->data);
		exit(1);
	}

	if (-1 == try_connect(sock, (char*)path->data)) {
		fprintf(stderr, "Error! Failed to connect to a server at: %s\n", path->data);
		exit(1);
	}
	str_free(path);

	if (strcmp(argv[1], "close") == 0) {
		tpl_node *tn = msg_node_pack(MSG_CLOSE);
		tpl_dump(tn, TPL_FD, sock);
		tpl_free(tn);
	} else if (strcmp(argv[1], "ac") == 0) {
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
	} else {
		printf("ccode client, commands:\n"
		       "  close\n"
		       "  ac <filename> <line> <col> (+ currently editted buffer as stdin)\n");
	}

	close(sock);
}

static char *prepend_cwd(const char *file)
{
	struct str *tmp;
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

static void print_completion_result(CXCompletionResult *r)
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
			str_add_printf(&abbr, "%s ", clang_getCString(s));
			break;
		case CXCompletionChunk_TypedText:
			str_add_cstr(&word, clang_getCString(s));
		default:
			str_add_cstr(&abbr, clang_getCString(s));
			break;
		}
		clang_disposeString(s);
	}
	printf("%s", word->data);
	int tabs = word->len / 8;
	if (tabs > 5)
		tabs = 5;

	tabs = 6 - tabs;
	for (int i = 0; i < tabs; ++i)
		printf("\t");

	printf("%s\n", abbr->data);
	str_free(word);
	str_free(abbr);
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
			str_add_printf(&abbr, "%s ", clang_getCString(s));
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

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp("-s", argv[1]) == 0) {
		server_main(argc, argv);
	} else {
		client_main(argc, argv);
	}
	/*
	CXIndex index;
	CXTranslationUnit tu;

	index = clang_createIndex(0, 0);

	tu = clang_parseTranslationUnit(index, 0, (const char * const *)argv, argc,
					0, 0, CXTranslationUnit_None);
	for (int i = 0, n = clang_getNumDiagnostics(tu); i != n; ++i) {
		CXDiagnostic diag = clang_getDiagnostic(tu, i);
		CXString string = clang_formatDiagnostic(diag, clang_defaultDiagnosticDisplayOptions());
		fprintf(stderr, "%s\n", clang_getCString(string));
		clang_disposeString(string);
	}
	clang_disposeTranslationUnit(tu);
	clang_disposeIndex(index);

	const char *filename;
	int line;
	int col;

	struct argument args[] = {
		ARG_STRING("filename",
			   &filename,
			   "filename of the file for autocompletion",
			   ""),
		ARG_INTEGER("line",
			    &line,
			    "line position, where autocompletion should happen",
			    -1),
		ARG_INTEGER("col",
			    &col,
			    "column position, where autocompletion should happen",
			    -1),
		ARG_END
	};

	parse_args(args, argc, argv, "ccode");
	printf("filename: %s, line: %d, col: %d\n", filename, line, col);

	CXIndex index;
	CXCodeCompleteResults *proposals;
	CXTranslationUnit tu;

	const char *clang_argv[] = {
		"clang",
		"-c",
		"test.c"
	};

	index = clang_createIndex(0, 0);
	tu = clang_parseTranslationUnit(index, filename, 0, 0, 0, 0,
					CXTranslationUnit_CacheCompletionResults);
	for (int i = 0, n = clang_getNumDiagnostics(tu); i != n; ++i) {
		CXDiagnostic diag = clang_getDiagnostic(tu, i);
		CXString string = clang_formatDiagnostic(diag, clang_defaultDiagnosticDisplayOptions());
		fprintf(stderr, "%s\n", clang_getCString(string));
		clang_disposeString(string);
	}

	proposals = clang_codeCompleteAt(tu, filename, line, col, 0, 0,
					 CXCodeComplete_IncludeMacros);

	if (proposals) {
		printf("got N results: %d\n", proposals->NumResults);
		clang_sortCodeCompletionResults(proposals->Results,
						proposals->NumResults);
		for (int i = 0; i < proposals->NumResults; ++i)
			print_completion_result(&proposals->Results[i]);
	}

	clang_disposeIndex(index);
	*/
	return 0;
}
