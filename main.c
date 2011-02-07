#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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
static void make_ac_proposal(struct ac_proposal *p, CXCompletionResult *r);

static CXIndex clang_index = 0;
static CXTranslationUnit clang_tu = 0;
static char *last_filename = 0;

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

static void process_ac(int sock)
{
	tpl_node *tn;
	struct msg_ac msg;

	tn = msg_ac_node(&msg);
	tpl_load(tn, TPL_FD, sock);
	tpl_unpack(tn, 0);
	tpl_free(tn);

	struct CXUnsavedFile unsaved = {
		msg.filename,
		msg.buffer.addr,
		msg.buffer.sz
	};

	printf("AUTOCOMPLETION:\n");
	printf(" buffer size: %d\n", msg.buffer.sz);
	printf(" filename: %s\n", msg.filename);
	printf(" line: %d\n", msg.line);
	printf(" col: %d\n", msg.col);
	printf("--------------------------------------------------\n");

	CXCodeCompleteResults *results;

	if (!last_filename || strcmp(last_filename, msg.filename) != 0) {
		if (clang_tu)
			clang_disposeTranslationUnit(clang_tu);
		clang_tu = clang_parseTranslationUnit(clang_index, msg.filename,
						      0, 0, &unsaved, 1,
						      clang_defaultEditingTranslationUnitOptions());
		if (last_filename)
			free(last_filename);
		last_filename = strdup(msg.filename);
	/*
	for (int i = 0, n = clang_getNumDiagnostics(tu); i != n; ++i) {
		CXDiagnostic diag = clang_getDiagnostic(tu, i);
		CXString string = clang_formatDiagnostic(diag, clang_defaultDiagnosticDisplayOptions());
		fprintf(stderr, "%s\n", clang_getCString(string));
		clang_disposeString(string);
	}
	*/
	}

	results = clang_codeCompleteAt(clang_tu, msg.filename, msg.line, msg.col,
				       &unsaved, 1,
				       0);
	free_msg_ac(&msg);

	struct msg_ac_response msg_r = { 0, 0 };

	if (results) {
		clang_sortCodeCompletionResults(results->Results,
						results->NumResults);

		printf("got %d results\n", results->NumResults);
		printf("--------------------------------------------------\n");
		msg_r.proposals_n = results->NumResults;
		msg_r.proposals = malloc(sizeof(struct ac_proposal) *
					 msg_r.proposals_n);

		for (int i = 0; i < msg_r.proposals_n; ++i)
			make_ac_proposal(&msg_r.proposals[i], &results->Results[i]);
	}

	clang_disposeCodeCompleteResults(results);

	msg_ac_response_send(&msg_r, sock);
	free_msg_ac_response(&msg_r);
}

static void server_main(int argc, char **argv)
{
	struct str *path = get_socket_path();
	int sock = create_server_socket(path);
	if (sock == -1) {
		fprintf(stderr, "Error! Failed to create a server socket: %s\n", path->data);
		exit(1);
	}

	clang_index = clang_createIndex(0, 0);
	server_loop(sock);
	if (clang_tu)
		clang_disposeTranslationUnit(clang_tu);
	clang_disposeIndex(clang_index);

	close(sock);
	unlink(path->data);
	str_free(path);
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
		printf("[0, [");
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

static void make_ac_proposal(struct ac_proposal *p, CXCompletionResult *r)
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

	p->abbr = strdup(abbr->data);
	p->word = strdup(word->data);
	str_free(word);
	str_free(abbr);
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
