#pragma once

/*
 * Str is a contiguous chunk of memory:
 * +-----+-----+-------------+
 * | CAP | LEN | STRING DATA |
 * +-----+-----+-------------+
 *
 * It can be used as a safe string implementation. The string itself is
 * null-terminated, so.. it's good for interop with standard C strings.
 *
 * I use 'unsigned int' as a type for length and capacity simply because 4
 * billion is enough for practically anything. And 'unsigned int' is 32 bits on
 * both x86 and x86_64, so I think it's a reasonable choice.
 *
 * The amount of memory required for a str with capacity == 5 is:
 *	(sizeof(str_t) + 5 + 1)
 *
 * Additional byte is used for zero termination and it's not a part of the
 * capacity.
 *
 * String data is always a correct C string.
 */

#include <stddef.h> /* for size_t */

typedef struct str_allocator {
	void *(*malloc)(size_t);
	void (*free)(void*);
} str_allocator_t;

/* These functions do not store the pointer to an allocator structure, they
 * just copy the contents of this structure.
 *
 * str_set_allocator - to specify a new pair of allocator functions for a
 * current thread
 *
 * str_get_allocator - to retrieve a pair of allocator functions for a current
 * thread.
 */
void str_set_allocator(const str_allocator_t *new_alloc);
void str_get_allocator(str_allocator_t *out);

/* should be > 0, and remember, that real memory size is +1 (trailing \0 byte) */
#ifndef STR_DEFAULT_CAPACITY
#define STR_DEFAULT_CAPACITY 7
#endif

typedef struct str {
	unsigned int cap;
	unsigned int len;
	char data[];
} str_t;

/* different ways to create a str */
str_t *str_new(unsigned int cap);
str_t *str_from_cstr(const char *cstr);
str_t *str_from_cstr_len(const char *cstr, unsigned int len);
str_t *str_printf(const char *fmt, ...);
str_t *str_dup(const str_t *str);
str_t *str_from_file(const char *filename);

void str_free(str_t *str);
void str_clear(str_t *str);

/* make sure there is enough capacity for 'n' additional bytes */
void str_ensure_cap(str_t **str, unsigned int n);

/* appending to a str */
void str_add_str(str_t **str, const str_t *str2);
void str_add_cstr(str_t **str, const char *cstr);
void str_add_cstr_len(str_t **str, const char *cstr, unsigned int len);
void str_add_printf(str_t **str, const char *fmt, ...);
void str_add_file(str_t **str, const char *filename);

/* trim, removes 'isspace' characters from sides: both, left, right */
void str_trim(str_t *str);
void str_ltrim(str_t *str);
void str_rtrim(str_t *str);

/* Splits 'path' immediately following the final path separator, separating it
 * into a directory and file name component. Returns a directory component if
 * any (if none, returns zero). If 'half2' isn't zero, writes file name
 * component to it (allocating a str, you're responsible to free it).
 */
str_t *str_split_path(const str_t *path, str_t **half2);

/*
 * FStr is a fixed string.
 * It doesn't manage its own memory, that's why it's called fixed.
 *
 * Length and capacity meanings are the same as in Str. Therefore usually the
 * capacity of the FStr is a buffer length minus one (zero termination).
 */

typedef struct fstr {
	unsigned int cap;
	unsigned int len;
	char *data;
} fstr_t;

#define FSTR_INIT_FOR_BUF(fstr, buf)\
	fstr_init(fstr, buf, 0, sizeof(buf)/sizeof(buf[0])-1)
void fstr_init(fstr_t *fstr, char *data, unsigned int len, unsigned int cap);

/* appending to a fstr */
void fstr_add_str(fstr_t *fstr, const str_t *str);
void fstr_add_cstr(fstr_t *fstr, const char *cstr);
void fstr_add_printf(fstr_t *fstr, const char *fmt, ...);
