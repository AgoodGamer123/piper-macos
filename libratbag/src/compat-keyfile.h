/*
 * Copyright © 2026 libratbag contributors
 * SPDX-License-Identifier: MIT
 *
 * A small reader for GLib "key files" (the .device data files), with the
 * same behaviour as the GKeyFile functions libratbag-data.c uses. Portable
 * builds use it through src/compat/glib.h so libratbag does not need GLib,
 * which keeps the macOS app self-contained.
 *
 * test/keyfile-compat-test.c checks it against GLib on every data file.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct rb_error {
	int domain;
	int code;
	char *message;
} RbError;

typedef struct rb_key_file RbKeyFile;

enum rb_key_file_error {
	RB_KEY_FILE_ERROR_UNKNOWN_ENCODING,
	RB_KEY_FILE_ERROR_PARSE,
	RB_KEY_FILE_ERROR_NOT_FOUND,
	RB_KEY_FILE_ERROR_KEY_NOT_FOUND,
	RB_KEY_FILE_ERROR_GROUP_NOT_FOUND,
	RB_KEY_FILE_ERROR_INVALID_VALUE,
};

RbKeyFile *rb_key_file_new(void);
void rb_key_file_free(RbKeyFile *kf);
bool rb_key_file_load_from_file(RbKeyFile *kf, const char *path, int flags, RbError **error);
char **rb_key_file_get_groups(RbKeyFile *kf, size_t *length);
char **rb_key_file_get_keys(RbKeyFile *kf, const char *group, size_t *length, RbError **error);
char *rb_key_file_get_string(RbKeyFile *kf, const char *group, const char *key, RbError **error);
int rb_key_file_get_integer(RbKeyFile *kf, const char *group, const char *key, RbError **error);
char **rb_key_file_get_string_list(RbKeyFile *kf, const char *group, const char *key,
				   size_t *length, RbError **error);

void rb_strfreev(char **strv);
void rb_error_free(RbError *error);
void rb_clear_error(RbError **error);
