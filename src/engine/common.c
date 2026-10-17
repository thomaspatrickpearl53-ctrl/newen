// newen - memory, strings, tokenizer, command line
#include "common.h"
#include <ctype.h>

// ---------------------------------------------------------------- memory

void *Mem_Alloc(size_t size)
{
	void *p = calloc(1, size ? size : 1);
	if (!p)
		Sys_Error("Out of memory allocating %zu bytes", size);
	return p;
}

void *Mem_Realloc(void *ptr, size_t size)
{
	void *p = realloc(ptr, size ? size : 1);
	if (!p)
		Sys_Error("Out of memory reallocating %zu bytes", size);
	return p;
}

void Mem_Free(void *ptr)
{
	free(ptr);
}

char *Mem_Strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = Mem_Alloc(n);
	memcpy(d, s, n);
	return d;
}

// ---------------------------------------------------------------- strings

void Q_strncpy(char *dst, const char *src, size_t size)
{
	if (!size)
		return;
	size_t n = strlen(src);
	if (n >= size)
		n = size - 1;
	memcpy(dst, src, n);
	dst[n] = 0;
}

char *va(const char *fmt, ...)
{
	static char bufs[8][4096];
	static int index;
	char *buf = bufs[index++ & 7];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(bufs[0]), fmt, ap);
	va_end(ap);
	return buf;
}

// '*' matches any run of characters, '?' any one; case-insensitive
qboolean Q_WildcardMatch(const char *p, const char *s)
{
	for (; *p; p++, s++) {
		if (*p == '*') {
			while (p[1] == '*')
				p++;
			if (!p[1])
				return true;
			for (; *s; s++)
				if (Q_WildcardMatch(p + 1, s))
					return true;
			return false;
		}
		if (!*s)
			return false;
		if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s))
			return false;
	}
	return *s == 0;
}

void SB_Append(strbuf_t *sb, const char *s, size_t n)
{
	if (sb->len + n + 1 > sb->cap) {
		size_t cap = sb->cap ? sb->cap : 256;
		while (cap < sb->len + n + 1)
			cap *= 2;
		sb->data = Mem_Realloc(sb->data, cap);
		sb->cap = cap;
	}
	memcpy(sb->data + sb->len, s, n);
	sb->len += n;
	sb->data[sb->len] = 0;
}

void SB_Clear(strbuf_t *sb)
{
	sb->len = 0;
	if (sb->data)
		sb->data[0] = 0;
}

void SB_Free(strbuf_t *sb)
{
	Mem_Free(sb->data);
	memset(sb, 0, sizeof(*sb));
}

// ---------------------------------------------------------------- tokenizer

char com_token[8192];

// Quake-style: skips whitespace and // comments; quoted strings are one token;
// { } ( ) ' : are single-character tokens.
const char *COM_Parse(const char *data)
{
	size_t len = 0;
	com_token[0] = 0;
	if (!data)
		return NULL;

	for (;;) {
		while (*data && (unsigned char)*data <= ' ')
			data++;
		if (!*data)
			return NULL;
		if (data[0] == '/' && data[1] == '/') {
			while (*data && *data != '\n')
				data++;
			continue;
		}
		break;
	}

	if (*data == '"') {
		data++;
		while (*data && *data != '"') {
			if (len < sizeof(com_token) - 1)
				com_token[len++] = *data;
			data++;
		}
		com_token[len] = 0;
		return *data ? data + 1 : data;
	}

	if (strchr("{}()':", *data)) {
		com_token[0] = *data;
		com_token[1] = 0;
		return data + 1;
	}

	while ((unsigned char)*data > ' ' && !strchr("{}()':", *data)) {
		if (len < sizeof(com_token) - 1)
			com_token[len++] = *data;
		data++;
	}
	com_token[len] = 0;
	return data;
}

// ---------------------------------------------------------------- command line

int    com_argc;
char **com_argv;

int COM_CheckParm(const char *parm)
{
	for (int i = 1; i < com_argc; i++)
		if (!stricmp(com_argv[i], parm))
			return i;
	return 0;
}

const char *COM_ParmValue(const char *parm)
{
	int i = COM_CheckParm(parm);
	if (i && i + 1 < com_argc)
		return com_argv[i + 1];
	return NULL;
}
