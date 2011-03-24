#include "strstr.h"
#include <sys/stat.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

//-------------------------------------------------------------------------------
// Default allocator
//-------------------------------------------------------------------------------

static void *xmalloc(size_t size)
{
	void *m = malloc(size);
	if (!m) {
		fprintf(stderr, "Fatal error! Memory allocation failed.\n");
		exit(1);
	}
	return m;
}

static __thread str_allocator_t allocator = {
	xmalloc,
	free
};

void str_set_allocator(const str_allocator_t *a)
{
	allocator = *a;
}

void str_get_allocator(str_allocator_t *a)
{
	*a = allocator;
}

//-------------------------------------------------------------------------------
// STR
//-------------------------------------------------------------------------------

static int is_cstr_in_str(str_t *str, const char *cstr)
{
	if ((str->data <= cstr) && (str->data + str->cap >= cstr))
		return 1;
	return 0;
}

//------------------------------------------------------------------------------

str_t *str_new(unsigned int cap)
{
	if (!cap)
		cap = STR_DEFAULT_CAPACITY;
	str_t *str = (*allocator.malloc)(sizeof(str_t) + cap + 1);
	str->len = 0;
	str->cap = cap;
	str->data[0] = '\0';
	return str;
}

void str_free(str_t *str)
{
	(*allocator.free)(str);
}

void str_clear(str_t *str)
{
	str->len = 0;
	str->data[0] = '\0';
}

str_t *str_from_cstr(const char *cstr)
{
	assert(cstr != 0);
	return str_from_cstr_len(cstr, strlen(cstr));
}

str_t *str_from_cstr_len(const char *cstr, unsigned int len)
{
	unsigned int cap = len > 0 ? len : STR_DEFAULT_CAPACITY;
	str_t *str = (*allocator.malloc)(sizeof(str_t) + cap + 1);
	str->len = len;
	str->cap = cap;
	if (len > 0)
		memcpy(str->data, cstr, len);
	str->data[len] = '\0';
	return str;
}

str_t *str_dup(const str_t *rhs)
{
	assert(rhs != 0);
	return str_from_cstr_len(rhs->data, rhs->len);
}

str_t *str_from_file(const char *filename)
{
	assert(filename != 0);

	struct stat st;
	FILE *f;
	str_t *str;

	if (-1 == stat(filename, &st))
		return 0;

	f = fopen(filename, "r");
	if (!f)
		return 0;


	str = (*allocator.malloc)(sizeof(str_t) + st.st_size + 1);
	str->cap = str->len = st.st_size;
	if (st.st_size != fread(str->data, 1, st.st_size, f)) {
		fclose(f);
		(*allocator.free)(str);
		return 0;
	}
	fclose(f);
	str->data[st.st_size] = '\0';
	return str;
}

void str_ensure_cap(str_t **out_str, unsigned int n)
{
	assert(out_str != 0);
	assert(*out_str != 0);

	str_t *str = *out_str;
	if (str->cap - str->len < n) {
		unsigned int newcap = str->cap * 2;
		if (newcap - str->len < n)
			newcap = str->len + n;

		str_t *newstr = (*allocator.malloc)(sizeof(str_t) + newcap + 1);
		newstr->cap = newcap;
		newstr->len = str->len;
		if (str->len > 0)
			memcpy(newstr->data, str->data, str->len + 1);
		else
			newstr->data[0] = '\0';
		(*allocator.free)(str);
		*out_str = newstr;
	}
}

str_t *str_printf(const char *fmt, ...)
{
	assert(fmt != 0);

	va_list va;

	va_start(va, fmt);
	unsigned int len = vsnprintf(0, 0, fmt, va);
	va_end(va);

	str_t *str = (*allocator.malloc)(sizeof(str_t) + len + 1);
	str->len = str->cap = len;
	va_start(va, fmt);
	vsnprintf(str->data, len + 1, fmt, va);
	va_end(va);
	return str;
}

void str_add_str(str_t **str, const str_t *str2)
{
	assert(str != 0);
	assert(str2 != 0);
	assert(*str != 0);
	assert(*str != str2);

	str_add_cstr_len(str, str2->data, str2->len);
}

void str_add_cstr(str_t **str, const char *cstr)
{
	assert(str != 0);
	assert(cstr != 0);
	assert(*str != 0);
	assert(!is_cstr_in_str(*str, cstr));

	str_add_cstr_len(str, cstr, strlen(cstr));
}

void str_add_cstr_len(str_t **str, const char *data, unsigned int len)
{
	if (!len)
		return;

	str_ensure_cap(str, len);

	str_t *s = *str;
	memcpy(&s->data[s->len], data, len + 1);
	s->len += len;
}

void str_add_printf(str_t **str, const char *fmt, ...)
{
	assert(str != 0);
	assert(fmt != 0);
	assert(*str != 0);
	assert(!is_cstr_in_str(*str, fmt));

	va_list va;

	va_start(va, fmt);
	unsigned int len = vsnprintf(0, 0, fmt, va);
	va_end(va);

	str_ensure_cap(str, len);

	str_t *s = *str;
	va_start(va, fmt);
	vsnprintf(&s->data[s->len], len + 1, fmt, va);
	va_end(va);
	s->len += len;
}

void str_add_file(str_t **str, const char *filename)
{
	assert(str != 0);
	assert(filename != 0);
	assert(*str != 0);

	struct stat st;
	FILE *f;

	if (-1 == stat(filename, &st))
		return;

	f = fopen(filename, "r");
	if (!f)
		return;

	str_ensure_cap(str, st.st_size);
	str_t *s = *str;
	if (st.st_size == fread(s->data + s->len, 1, st.st_size, f))
		s->len += st.st_size;
	fclose(f);
	s->data[s->len] = '\0';
}

void str_trim(str_t *str)
{
	str_rtrim(str);
	str_ltrim(str);
}

void str_ltrim(str_t *str)
{
	char *c = str->data;
	while (str->len > 0 && isspace(*c)) {
		str->len--;
		c++;
	}
	memmove(str->data, c, str->len);
	str->data[str->len] = '\0';
}

void str_rtrim(str_t *str)
{
	while (str->len > 0 && isspace(str->data[str->len - 1]))
		str->len--;
	str->data[str->len] = '\0';
}

str_t *str_split_path(const str_t *str, str_t **half2)
{
	const char *c = str->data + (str->len - 1);
	while (c != str->data && *c != '/')
		c--;

	if (c == str->data) {
		if (half2)
			*half2 = str_dup(str);
		return 0;
	}

	if (half2)
		*half2 = str_from_cstr_len(c+1, str->data + str->len - (c+1));

	return str_from_cstr_len(str->data, c - str->data);
}

//-------------------------------------------------------------------------------
// FSTR
//-------------------------------------------------------------------------------

void fstr_add_cstr_len(fstr_t *fstr, const char *data, unsigned int len)
{
	if (!len)
		return;

	unsigned int avail = fstr->cap - fstr->len;
	if (len > avail)
		len = avail;

	memcpy(&fstr->data[fstr->len], data, len);
	fstr->data[fstr->len + len] = '\0';
	fstr->len += len;
}

//------------------------------------------------------------------------------

void fstr_init(fstr_t *fstr, char *data, unsigned int len, unsigned int cap)
{
	assert(fstr != 0);
	assert(data != 0);
	assert(cap >= len);
	assert(cap > 0);

	fstr->cap = cap;
	fstr->len = len;
	fstr->data = data;
	fstr->data[0] = '\0';
}

void fstr_add_str(fstr_t *fstr, const str_t *str)
{
	assert(fstr != 0);
	assert(str != 0);

	fstr_add_cstr_len(fstr, str->data, str->len);
}

void fstr_add_cstr(fstr_t *fstr, const char *cstr)
{
	assert(fstr != 0);
	assert(cstr != 0);

	fstr_add_cstr_len(fstr, cstr, strlen(cstr));
}

void fstr_add_printf(fstr_t *fstr, const char *fmt, ...)
{
	assert(fstr != 0);
	assert(fmt != 0);

	va_list va;
	unsigned int avail = fstr->cap - fstr->len;

	va_start(va, fmt);
	int plen = vsnprintf(&fstr->data[fstr->len], avail+1, fmt, va);
	va_end(va);

	assert(plen >= 0);

	unsigned int len = (unsigned int)plen;
	if (len > avail)
		len = avail;
	fstr->len += len;
}
