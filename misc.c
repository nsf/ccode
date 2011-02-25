#include "shared.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int file_exists(const char *filename)
{
	struct stat st;
	return stat(filename, &st) == 0;
}

int starts_with(const char *s1, const char *s2)
{
	while (*s2) if (*s1++ != *s2++) return 0; return 1;
}

int read_file(void **out, size_t *size, const char *filename)
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

int read_stdin(void **out, size_t *size)
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

str_t *get_socket_path()
{
	char *user = getenv("USER");
	if (user)
		return str_printf("/tmp/ccode-server.%s", user);
	else
		return str_from_cstr("/tmp/ccode-server");
}
