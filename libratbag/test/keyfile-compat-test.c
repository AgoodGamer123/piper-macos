/*
 * Copyright © 2026 libratbag contributors
 * SPDX-License-Identifier: MIT
 *
 * Checks src/compat-keyfile.c against GLib's GKeyFile: every file given on
 * the command line (or every file in a directory) is loaded with both, and
 * every group, key, and string/integer/list lookup must give the same
 * result. Built only where GLib is available (the udev build).
 */

#include <dirent.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "compat-keyfile.h"

static int failures;

#define FAIL(...) do { failures++; fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

static void
compare_key(const char *path, GKeyFile *g, RbKeyFile *r, const char *group, const char *key)
{
	GError *gerr = NULL;
	RbError *rerr = NULL;
	gchar *gs = g_key_file_get_string(g, group, key, &gerr);
	char *rs = rb_key_file_get_string(r, group, key, &rerr);

	if (!!gs != !!rs || !!gerr != !!rerr || (gs && strcmp(gs, rs) != 0))
		FAIL("%s [%s] %s: string '%s' vs '%s'", path, group, key, gs ? gs : "(null)", rs ? rs : "(null)");
	g_free(gs);
	free(rs);
	g_clear_error(&gerr);
	rb_clear_error(&rerr);

	{
		gint gi = g_key_file_get_integer(g, group, key, &gerr);
		int ri = rb_key_file_get_integer(r, group, key, &rerr);

		if (gi != ri || !!gerr != !!rerr)
			FAIL("%s [%s] %s: integer %d%s vs %d%s", path, group, key,
			     gi, gerr ? " (error)" : "", ri, rerr ? " (error)" : "");
		g_clear_error(&gerr);
		rb_clear_error(&rerr);
	}

	{
		gsize glen = 0;
		size_t rlen = 0;
		gchar **gl = g_key_file_get_string_list(g, group, key, &glen, &gerr);
		char **rl = rb_key_file_get_string_list(r, group, key, &rlen, &rerr);

		if (!!gl != !!rl || !!gerr != !!rerr || glen != rlen) {
			FAIL("%s [%s] %s: list of %zu vs %zu", path, group, key, (size_t)glen, rlen);
		} else {
			for (gsize i = 0; gl && i < glen; i++)
				if (strcmp(gl[i], rl[i]) != 0)
					FAIL("%s [%s] %s: list item %zu '%s' vs '%s'", path, group, key,
					     (size_t)i, gl[i], rl[i]);
		}
		g_strfreev(gl);
		rb_strfreev(rl);
		g_clear_error(&gerr);
		rb_clear_error(&rerr);
	}
}

static int
compare_file(const char *path)
{
	GKeyFile *g = g_key_file_new();
	RbKeyFile *r = rb_key_file_new();
	GError *gerr = NULL;
	RbError *rerr = NULL;
	gboolean gok = g_key_file_load_from_file(g, path, G_KEY_FILE_NONE, &gerr);
	bool rok = rb_key_file_load_from_file(r, path, 0, &rerr);
	int keys = 0;

	if (gok != rok) {
		FAIL("%s: GLib %s, compat %s (%s / %s)", path, gok ? "loads" : "fails",
		     rok ? "loads" : "fails", gerr ? gerr->message : "", rerr ? rerr->message : "");
		goto out;
	}
	if (!gok)
		goto out;

	{
		gsize ng = 0;
		size_t nr = 0;
		gchar **gg = g_key_file_get_groups(g, &ng);
		char **rg = rb_key_file_get_groups(r, &nr);

		if (ng != nr)
			FAIL("%s: %zu groups vs %zu", path, (size_t)ng, nr);
		for (gsize i = 0; i < ng && i < nr; i++) {
			gsize nk = 0;
			size_t nrk = 0;
			gchar **gk;
			char **rk;

			if (strcmp(gg[i], rg[i]) != 0)
				FAIL("%s: group %zu '%s' vs '%s'", path, (size_t)i, gg[i], rg[i]);
			gk = g_key_file_get_keys(g, gg[i], &nk, NULL);
			rk = rb_key_file_get_keys(r, gg[i], &nrk, NULL);
			if (nk != nrk)
				FAIL("%s [%s]: %zu keys vs %zu", path, gg[i], (size_t)nk, nrk);
			for (gsize k = 0; k < nk && k < nrk; k++) {
				if (strcmp(gk[k], rk[k]) != 0)
					FAIL("%s [%s]: key %zu '%s' vs '%s'", path, gg[i], (size_t)k, gk[k], rk[k]);
				compare_key(path, g, r, gg[i], gk[k]);
				keys++;
			}
			/* lookups that must fail the same way */
			compare_key(path, g, r, gg[i], "NoSuchKey");
			g_strfreev(gk);
			rb_strfreev(rk);
		}
		compare_key(path, g, r, "NoSuchGroup", "Name");
		g_strfreev(gg);
		rb_strfreev(rg);
	}

out:
	g_clear_error(&gerr);
	rb_clear_error(&rerr);
	g_key_file_free(g);
	rb_key_file_free(r);
	return keys;
}

int
main(int argc, char **argv)
{
	int files = 0, keys = 0;

	for (int i = 1; i < argc; i++) {
		struct stat st;

		if (stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode)) {
			DIR *d = opendir(argv[i]);
			struct dirent *e;

			while (d && (e = readdir(d))) {
				char path[4096];

				if (e->d_name[0] == '.')
					continue;
				snprintf(path, sizeof(path), "%s/%s", argv[i], e->d_name);
				keys += compare_file(path);
				files++;
			}
			if (d)
				closedir(d);
		} else {
			keys += compare_file(argv[i]);
			files++;
		}
	}

	printf("%d files, %d keys compared, %d differences\n", files, keys, failures);
	return failures ? 1 : 0;
}
