/*
 * Stand-in for <glib.h> in portable builds: libratbag-data.c only uses
 * GKeyFile, which src/compat-keyfile.c provides without GLib.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include "compat-keyfile.h"

typedef RbKeyFile GKeyFile;
typedef RbError GError;
typedef size_t gsize;

#define G_KEY_FILE_NONE 0

#define g_key_file_new			rb_key_file_new
#define g_key_file_free			rb_key_file_free
#define g_key_file_load_from_file	rb_key_file_load_from_file
#define g_key_file_get_groups		rb_key_file_get_groups
#define g_key_file_get_keys		rb_key_file_get_keys
#define g_key_file_get_string		rb_key_file_get_string
#define g_key_file_get_integer		rb_key_file_get_integer
#define g_key_file_get_string_list	rb_key_file_get_string_list
#define g_strfreev			rb_strfreev
#define g_error_free			rb_error_free
#define g_clear_error			rb_clear_error
