/*
 * anyfs_u8_main.c — wmain() for the CLIs on Windows (linked with -municode;
 * the tool's own main() is renamed anyfs_tool_main by -Dmain=). The tool
 * gets its arguments in UTF-8, and GLib's print and log output goes through
 * the UTF-8 host layer instead of being converted to the ANSI code page.
 */
#include "anyfs_u8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ANYFS_U8_HAVE_GLIB
#include <glib.h>

static void print_out(const gchar* s)
{
	anyfs_u8_fputs(s, stdout);
}

static void print_err(const gchar* s)
{
	anyfs_u8_fputs(s, stderr);
}

static GLogWriterOutput log_writer(GLogLevelFlags level,
				   const GLogField* fields, gsize n_fields,
				   gpointer user_data)
{
	const char* domain = NULL;
	const char* message = NULL;

	(void)user_data;
	for (gsize i = 0; i < n_fields; i++) {
		if (strcmp(fields[i].key, "GLIB_DOMAIN") == 0)
			domain = fields[i].value;
		else if (strcmp(fields[i].key, "MESSAGE") == 0)
			message = fields[i].value;
	}
	if (g_log_writer_default_would_drop(level, domain))
		return G_LOG_WRITER_HANDLED;
	const char* name = (level & G_LOG_LEVEL_ERROR)	    ? "ERROR"
			   : (level & G_LOG_LEVEL_CRITICAL) ? "CRITICAL"
			   : (level & G_LOG_LEVEL_WARNING)  ? "WARNING"
			   : (level & G_LOG_LEVEL_MESSAGE)  ? "Message"
			   : (level & G_LOG_LEVEL_INFO)	    ? "INFO"
							    : "DEBUG";
	anyfs_u8_fprintf(stderr, "%s%s%s: %s\n", domain ? domain : "",
			 domain ? "-" : "", name, message ? message : "");
	return G_LOG_WRITER_HANDLED;
}
#endif

int anyfs_tool_main(int argc, char** argv);

int wmain(int argc, wchar_t** wargv)
{
	char** argv = calloc((size_t)argc + 1, sizeof(char*));
	if (!argv)
		return 2;
	for (int i = 0; i < argc; i++) {
		if (anyfs_u16_to_u8(wargv[i], &argv[i]) < 0) {
			anyfs_u8_fprintf(
			    stderr, "argument %d is not valid Unicode\n", i);
			return 2;
		}
	}
#ifdef ANYFS_U8_HAVE_GLIB
	g_set_print_handler(print_out);
	g_set_printerr_handler(print_err);
	g_log_set_writer_func(log_writer, NULL, NULL);
#endif
	return anyfs_tool_main(argc, argv);
}
