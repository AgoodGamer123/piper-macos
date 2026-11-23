/*
 * Copyright © 2026 libratbag contributors
 * SPDX-License-Identifier: MIT
 *
 * See compat-keyfile.h. The parsing rules follow GLib's gkeyfile.c:
 *  - leading whitespace on a line is ignored; '#' starts a comment line
 *  - "[name]" starts a group, optionally followed by blanks; repeating a
 *    group adds to it
 *  - "key = value": blanks around the key and before the value are
 *    dropped; a later key of the same name wins (both are listed)
 *  - strings understand \s \n \t \r \\ escapes, lists also \; and are split
 *    at ';' with a trailing ';' ignored; an empty value is an empty list
 *  - integers are base 10, trailing blanks allowed
 */

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat-keyfile.h"

struct kf_entry {
	char *key;
	char *value;
};

struct kf_group {
	char *name;
	struct kf_entry *entries;
	size_t nentries;
};

struct rb_key_file {
	struct kf_group *groups;
	size_t ngroups;
};

static void *
xrealloc(void *p, size_t size)
{
	void *n = realloc(p, size ? size : 1);

	if (!n)
		abort();
	return n;
}

static char *
xstrndup(const char *s, size_t n)
{
	char *d = xrealloc(NULL, n + 1);

	memcpy(d, s, n);
	d[n] = '\0';
	return d;
}

static void
set_error(RbError **error, int code, const char *fmt, ...)
{
	va_list args;
	RbError *e;
	char buf[512];

	if (!error)
		return;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	e = xrealloc(NULL, sizeof(*e));
	e->domain = 0;
	e->code = code;
	e->message = xstrndup(buf, strlen(buf));
	*error = e;
}

static int
is_blank(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

RbKeyFile *
rb_key_file_new(void)
{
	RbKeyFile *kf = xrealloc(NULL, sizeof(*kf));

	kf->groups = NULL;
	kf->ngroups = 0;
	return kf;
}

static void
kf_clear(RbKeyFile *kf)
{
	for (size_t g = 0; g < kf->ngroups; g++) {
		for (size_t e = 0; e < kf->groups[g].nentries; e++) {
			free(kf->groups[g].entries[e].key);
			free(kf->groups[g].entries[e].value);
		}
		free(kf->groups[g].entries);
		free(kf->groups[g].name);
	}
	free(kf->groups);
	kf->groups = NULL;
	kf->ngroups = 0;
}

void
rb_key_file_free(RbKeyFile *kf)
{
	if (!kf)
		return;
	kf_clear(kf);
	free(kf);
}

static struct kf_group *
find_group(RbKeyFile *kf, const char *name)
{
	for (size_t g = 0; g < kf->ngroups; g++)
		if (strcmp(kf->groups[g].name, name) == 0)
			return &kf->groups[g];
	return NULL;
}

static struct kf_group *
add_group(RbKeyFile *kf, const char *name, size_t len)
{
	char *n = xstrndup(name, len);
	struct kf_group *g = find_group(kf, n);

	if (g) {
		free(n);
		return g;
	}
	kf->groups = xrealloc(kf->groups, (kf->ngroups + 1) * sizeof(*kf->groups));
	g = &kf->groups[kf->ngroups++];
	g->name = n;
	g->entries = NULL;
	g->nentries = 0;
	return g;
}

static void
set_value(struct kf_group *g, const char *key, size_t keylen, const char *value, size_t valuelen)
{
	char *k = xstrndup(key, keylen);

	/* like GLib, keep duplicates; lookups find the last one */
	g->entries = xrealloc(g->entries, (g->nentries + 1) * sizeof(*g->entries));
	g->entries[g->nentries].key = k;
	g->entries[g->nentries].value = xstrndup(value, valuelen);
	g->nentries++;
}

/* One line, without its line terminator */
static bool
parse_line(RbKeyFile *kf, struct kf_group **current, const char *line, size_t len,
	   unsigned lineno, RbError **error)
{
	const char *end = line + len;
	const char *p = line;

	while (p < end && is_blank(*p))
		p++;

	if (p == end || *p == '#')
		return true;

	if (*p == '[') {
		const char *close = memchr(p + 1, ']', end - (p + 1));
		const char *q;

		if (!close)
			goto invalid;
		for (q = close + 1; q < end; q++)
			if (*q != ' ' && *q != '\t')
				goto invalid;
		if (close == p + 1)
			goto invalid;	/* empty group name */
		*current = add_group(kf, p + 1, close - (p + 1));
		return true;
	} else {
		const char *eq = memchr(p, '=', end - p);
		const char *key_end, *value;

		if (!eq)
			goto invalid;
		if (!*current) {
			set_error(error, RB_KEY_FILE_ERROR_GROUP_NOT_FOUND,
				  "Key file does not start with a group");
			return false;
		}
		key_end = eq;
		while (key_end > p && is_blank(key_end[-1]))
			key_end--;
		if (key_end == p)
			goto invalid;	/* empty key */
		value = eq + 1;
		while (value < end && is_blank(*value))
			value++;
		set_value(*current, p, key_end - p, value, end - value);
		return true;
	}

invalid:
	set_error(error, RB_KEY_FILE_ERROR_PARSE,
		  "Key file contains line %u which is not a key-value pair, group, or comment",
		  lineno);
	return false;
}

bool
rb_key_file_load_from_file(RbKeyFile *kf, const char *path, int flags, RbError **error)
{
	struct kf_group *current = NULL;
	char *data = NULL;
	size_t size = 0, used = 0;
	unsigned lineno = 0;
	const char *p, *end;
	FILE *f;

	(void)flags;

	f = fopen(path, "rb");
	if (!f) {
		set_error(error, RB_KEY_FILE_ERROR_NOT_FOUND, "%s: %s", path, strerror(errno));
		return false;
	}
	while (!feof(f)) {
		size_t n;

		if (size - used < 4096) {
			size += 16384;
			data = xrealloc(data, size);
		}
		n = fread(data + used, 1, size - used, f);
		if (n == 0 && ferror(f)) {
			fclose(f);
			free(data);
			set_error(error, RB_KEY_FILE_ERROR_NOT_FOUND, "%s: read error", path);
			return false;
		}
		used += n;
	}
	fclose(f);

	kf_clear(kf);

	p = data;
	end = data + used;
	while (p < end) {
		const char *nl = memchr(p, '\n', end - p);
		const char *line_end = nl ? nl : end;
		size_t len = line_end - p;

		lineno++;
		if (len > 0 && p[len - 1] == '\r')
			len--;
		if (!parse_line(kf, &current, p, len, lineno, error)) {
			kf_clear(kf);
			free(data);
			return false;
		}
		p = nl ? nl + 1 : end;
	}

	free(data);
	return true;
}

char **
rb_key_file_get_groups(RbKeyFile *kf, size_t *length)
{
	char **groups = xrealloc(NULL, (kf->ngroups + 1) * sizeof(char *));

	for (size_t g = 0; g < kf->ngroups; g++)
		groups[g] = xstrndup(kf->groups[g].name, strlen(kf->groups[g].name));
	groups[kf->ngroups] = NULL;
	if (length)
		*length = kf->ngroups;
	return groups;
}

static const char *
get_value(RbKeyFile *kf, const char *group, const char *key, RbError **error)
{
	struct kf_group *g = find_group(kf, group);

	if (!g) {
		set_error(error, RB_KEY_FILE_ERROR_GROUP_NOT_FOUND,
			  "Key file does not have group \"%s\"", group);
		return NULL;
	}
	for (size_t e = g->nentries; e > 0; e--)
		if (strcmp(g->entries[e - 1].key, key) == 0)
			return g->entries[e - 1].value;

	set_error(error, RB_KEY_FILE_ERROR_KEY_NOT_FOUND,
		  "Key file does not have key \"%s\" in group \"%s\"", key, group);
	return NULL;
}

char **
rb_key_file_get_keys(RbKeyFile *kf, const char *group, size_t *length, RbError **error)
{
	struct kf_group *g = find_group(kf, group);
	char **keys;

	if (!g) {
		set_error(error, RB_KEY_FILE_ERROR_GROUP_NOT_FOUND,
			  "Key file does not have group \"%s\"", group);
		return NULL;
	}
	keys = xrealloc(NULL, (g->nentries + 1) * sizeof(char *));
	for (size_t e = 0; e < g->nentries; e++)
		keys[e] = xstrndup(g->entries[e].key, strlen(g->entries[e].key));
	keys[g->nentries] = NULL;
	if (length)
		*length = g->nentries;
	return keys;
}

/*
 * Unescapes value. With pieces != NULL, an unescaped ';' ends a piece
 * (list mode); a final empty piece is dropped unless it is the only one.
 */
static char *
unescape(const char *value, char ***pieces, size_t *npieces, RbError **error)
{
	size_t len = strlen(value);
	char *out = xrealloc(NULL, len + 1);
	char *q = out;
	char *piece_start = out;

	if (pieces) {
		/* an empty value is an empty list, not NULL */
		*pieces = xrealloc(NULL, sizeof(char *));
		(*pieces)[0] = NULL;
		*npieces = 0;
	}

	for (const char *p = value; *p; p++) {
		if (*p == '\\') {
			p++;
			switch (*p) {
			case 's': *q++ = ' '; break;
			case 'n': *q++ = '\n'; break;
			case 't': *q++ = '\t'; break;
			case 'r': *q++ = '\r'; break;
			case '\\': *q++ = '\\'; break;
			case ';':
				if (pieces) {
					*q++ = ';';
					break;
				}
				/* like GLib: \; is only an escape in lists */
				set_error(error, RB_KEY_FILE_ERROR_INVALID_VALUE,
					  "Key file contains invalid escape sequence \"\\;\"");
				goto fail;
			case '\0':
				set_error(error, RB_KEY_FILE_ERROR_INVALID_VALUE,
					  "Key file contains escape character at end of line");
				goto fail;
			default:
				set_error(error, RB_KEY_FILE_ERROR_INVALID_VALUE,
					  "Key file contains invalid escape sequence \"\\%c\"", *p);
				goto fail;
			}
		} else if (pieces && *p == ';') {
			*pieces = xrealloc(*pieces, (*npieces + 2) * sizeof(char *));
			(*pieces)[(*npieces)++] = xstrndup(piece_start, q - piece_start);
			(*pieces)[*npieces] = NULL;
			piece_start = q;
		} else {
			*q++ = *p;
		}
	}
	*q = '\0';

	if (pieces && q != piece_start) {
		*pieces = xrealloc(*pieces, (*npieces + 2) * sizeof(char *));
		(*pieces)[(*npieces)++] = xstrndup(piece_start, q - piece_start);
		(*pieces)[*npieces] = NULL;
	}
	return out;

fail:
	free(out);
	if (pieces) {
		rb_strfreev(*pieces);
		*pieces = NULL;
		*npieces = 0;
	}
	return NULL;
}

char *
rb_key_file_get_string(RbKeyFile *kf, const char *group, const char *key, RbError **error)
{
	const char *value = get_value(kf, group, key, error);

	if (!value)
		return NULL;
	return unescape(value, NULL, NULL, error);
}

int
rb_key_file_get_integer(RbKeyFile *kf, const char *group, const char *key, RbError **error)
{
	const char *value = get_value(kf, group, key, error);
	char *endp;
	long v;

	if (!value)
		return 0;

	errno = 0;
	v = strtol(value, &endp, 10);
	if (*value == '\0' || (*endp != '\0' && !is_blank(*endp))) {
		set_error(error, RB_KEY_FILE_ERROR_INVALID_VALUE,
			  "Value \"%s\" cannot be interpreted as a number.", value);
		return 0;
	}
	if (errno == ERANGE || v < INT_MIN || v > INT_MAX) {
		set_error(error, RB_KEY_FILE_ERROR_INVALID_VALUE,
			  "Integer value \"%s\" out of range", value);
		return 0;
	}
	return (int)v;
}

char **
rb_key_file_get_string_list(RbKeyFile *kf, const char *group, const char *key,
			    size_t *length, RbError **error)
{
	const char *value = get_value(kf, group, key, error);
	char **pieces = NULL;
	size_t npieces = 0;
	char *joined;

	if (length)
		*length = 0;
	if (!value)
		return NULL;

	joined = unescape(value, &pieces, &npieces, error);
	if (!joined)
		return NULL;
	free(joined);

	if (length)
		*length = npieces;
	return pieces;
}

void
rb_strfreev(char **strv)
{
	if (!strv)
		return;
	for (char **s = strv; *s; s++)
		free(*s);
	free(strv);
}

void
rb_error_free(RbError *error)
{
	if (!error)
		return;
	free(error->message);
	free(error);
}

void
rb_clear_error(RbError **error)
{
	if (error && *error) {
		rb_error_free(*error);
		*error = NULL;
	}
}
