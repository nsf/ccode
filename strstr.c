#include "strstr.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

//-------------------------------------------------------------------------------

static void die(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

static void *xalloc(size_t size)
{
	void *m = malloc(size);
	if (!m)
		die("Fatal error! Memory allocation failed.");
	return m;
}

//-------------------------------------------------------------------------------
// STR
//-------------------------------------------------------------------------------

static int is_cstr_in_str(struct str *str, const char *cstr)
{
	if ((str->data <= cstr) && (str->data + str->cap >= cstr))
		return 1;
	return 0;
}

static void str_add_cstr_len(struct str **str, const char *data, unsigned int len)
{
	if (!len)
		return;

	str_ensure_cap(str, len);

	struct str *s = *str;
	memcpy(&s->data[s->len], data, len + 1);
	s->len += len;
}

//------------------------------------------------------------------------------

struct str *str_new(unsigned int cap)
{
	if (!cap)
		cap = STR_DEFAULT_CAPACITY;
	struct str *str = xalloc(sizeof(struct str) + cap + 1);
	str->len = 0;
	str->cap = cap;
	str->data[0] = '\0';
	return str;
}

void str_free(struct str *str)
{
	free(str);
}

void str_clear(struct str *str)
{
	str->len = 0;
	str->data[0] = '\0';
}

struct str *str_from_cstr(const char *cstr)
{
	assert(cstr != 0);
	return str_from_cstr_len(cstr, strlen(cstr));
}

struct str *str_from_cstr_len(const char *cstr, unsigned int len)
{
	unsigned int cap = len > 0 ? len : STR_DEFAULT_CAPACITY;
	struct str *str = xalloc(sizeof(struct str) + cap + 1);
	str->len = len;
	str->cap = cap;
	if (len > 0)
		memcpy(str->data, cstr, len);
	str->data[len] = '\0';
	return str;
}

struct str *str_dup(const struct str *rhs)
{
	assert(rhs != 0);
	return str_from_cstr_len(rhs->data, rhs->len);
}

void str_ensure_cap(struct str **out_str, unsigned int n)
{
	assert(out_str != 0);
	assert(*out_str != 0);

	struct str *str = *out_str;
	if (str->cap - str->len < n) {
		unsigned int newcap = str->cap * 2;
		if (newcap - str->len < n)
			newcap = str->len + n;

		struct str *newstr = xalloc(sizeof(struct str) +
					    newcap + 1);
		newstr->cap = newcap;
		newstr->len = str->len;
		if (str->len > 0)
			memcpy(newstr->data, str->data, str->len + 1);
		free(str);
		*out_str = newstr;
	}
}

struct str *str_printf(const char *fmt, ...)
{
	assert(fmt != 0);

	va_list va;

	va_start(va, fmt);
	unsigned int len = vsnprintf(0, 0, fmt, va);
	va_end(va);

	struct str *str = xalloc(sizeof(struct str) +
				 len + 1);
	str->len = str->cap = len;
	va_start(va, fmt);
	vsnprintf(str->data, len + 1, fmt, va);
	va_end(va);
	return str;
}

void str_add_str(struct str **str, const struct str *str2)
{
	assert(str != 0);
	assert(str2 != 0);
	assert(*str != 0);
	assert(*str != str2);

	str_add_cstr_len(str, str2->data, str2->len);
}

void str_add_cstr(struct str **str, const char *cstr)
{
	assert(str != 0);
	assert(cstr != 0);
	assert(*str != 0);
	assert(!is_cstr_in_str(*str, cstr));

	str_add_cstr_len(str, cstr, strlen(cstr));
}

void str_add_printf(struct str **str, const char *fmt, ...)
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

	struct str *s = *str;
	va_start(va, fmt);
	vsnprintf(&s->data[s->len], len + 1, fmt, va);
	va_end(va);
	s->len += len;
}

void str_trim(struct str *str)
{
	str_rtrim(str);
	str_ltrim(str);
}

void str_ltrim(struct str *str)
{
	char *c = str->data;
	while (str->len > 0 && isspace(*c)) {
		str->len--;
		c++;
	}
	memmove(str->data, c, str->len);
	str->data[str->len] = '\0';
}

void str_rtrim(struct str *str)
{
	while (str->len > 0 && isspace(str->data[str->len - 1]))
		str->len--;
	str->data[str->len] = '\0';
}

struct str *str_path_split(const struct str *str, struct str **half2)
{
	const char *c = str->data + (str->len - 1);
	while (c != str->data && *c != '/')
		c--;

	if (c == str->data) {
		if (half2)
			*half2 = str_dup(str);
		return str_new(0);
	}

	if (half2)
		*half2 = str_from_cstr_len(c+1, str->data + str->len - (c+1));

	return str_from_cstr_len(str->data, c - str->data);
}

//-------------------------------------------------------------------------------
// FSTR
//-------------------------------------------------------------------------------

void fstr_add_cstr_len(struct fstr *fstr, const char *data, unsigned int len)
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

void fstr_init(struct fstr *fstr, char *data, unsigned int len, unsigned int cap)
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

void fstr_add_str(struct fstr *fstr, const struct str *str)
{
	assert(fstr != 0);
	assert(str != 0);

	fstr_add_cstr_len(fstr, str->data, str->len);
}

void fstr_add_cstr(struct fstr *fstr, const char *cstr)
{
	assert(fstr != 0);
	assert(cstr != 0);

	fstr_add_cstr_len(fstr, cstr, strlen(cstr));
}

void fstr_add_printf(struct fstr *fstr, const char *fmt, ...)
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
