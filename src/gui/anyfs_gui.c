// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * anyfs_gui.c — GTK3 file manager for disk images via LKL
 *
 * Features:
 *   - List view with icons (file/dir/symlink)
 *   - Text and image preview panel
 *   - Drag files out to host (guest→host)
 *   - Drag files in from host (host→guest)
 *   - Directory navigation (double-click, breadcrumb)
 *
 * Build: meson compile -C builddir-ksmbd anyfs-gui
 * Run:   ./anyfs-gui disk.img
 *        ./anyfs-gui -w disk.img   (read-write)
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "anyfs.h"
#include <gtk/gtk.h>
#include <lkl.h>

/* ── Column definitions for the file list ─────────────────────── */
enum {
	COL_ICON,     /* GdkPixbuf* or icon name */
	COL_NAME,     /* filename (char*) */
	COL_SIZE,     /* file size string */
	COL_MODIFIED, /* mtime string */
	COL_MODE,     /* permission string */
	COL_IS_DIR,   /* gboolean: is directory? */
	COL_FULLPATH, /* full LKL path (char*) */
	NUM_COLS
};

/* ── Application state ────────────────────────────────────────── */
typedef struct {
	GtkWidget* window;
	GtkWidget* tree_view;
	GtkWidget* path_entry;	  /* breadcrumb / path bar */
	GtkWidget* preview_stack; /* GtkStack: text or image */
	GtkWidget* preview_text;  /* GtkTextView */
	GtkWidget* preview_image; /* GtkImage */
	GtkWidget* statusbar;
	GtkListStore* store;

	char current_path[4096]; /* current LKL directory path */
	char mount_point[64];	 /* e.g. "/lklmnt/img" */
	int disk_id;
	gboolean writable;
} AppState;

static AppState app;

/* ── Helpers ──────────────────────────────────────────────────── */

static const char* icon_for_mode(unsigned int mode)
{
	if (S_ISDIR(mode))
		return "folder";
	if (S_ISLNK(mode))
		return "emblem-symbolic-link";
	if (S_ISBLK(mode) || S_ISCHR(mode))
		return "drive-harddisk";
	if (S_ISFIFO(mode) || S_ISSOCK(mode))
		return "network-server";

	/* Guess by extension for common types */
	return "text-x-generic";
}

static const char* icon_for_file(const char* name, unsigned int mode)
{
	if (S_ISDIR(mode))
		return "folder";
	if (S_ISLNK(mode))
		return "emblem-symbolic-link";

	/* Image files */
	const char* ext = strrchr(name, '.');
	if (ext) {
		if (strcasecmp(ext, ".png") == 0 ||
		    strcasecmp(ext, ".jpg") == 0 ||
		    strcasecmp(ext, ".jpeg") == 0 ||
		    strcasecmp(ext, ".gif") == 0 ||
		    strcasecmp(ext, ".bmp") == 0 ||
		    strcasecmp(ext, ".webp") == 0 ||
		    strcasecmp(ext, ".svg") == 0)
			return "image-x-generic";
		if (strcasecmp(ext, ".txt") == 0 ||
		    strcasecmp(ext, ".md") == 0 ||
		    strcasecmp(ext, ".log") == 0 ||
		    strcasecmp(ext, ".conf") == 0 ||
		    strcasecmp(ext, ".cfg") == 0 ||
		    strcasecmp(ext, ".ini") == 0 ||
		    strcasecmp(ext, ".sh") == 0 || strcasecmp(ext, ".c") == 0 ||
		    strcasecmp(ext, ".h") == 0 || strcasecmp(ext, ".py") == 0)
			return "text-x-generic";
		if (strcasecmp(ext, ".zip") == 0 ||
		    strcasecmp(ext, ".tar") == 0 ||
		    strcasecmp(ext, ".gz") == 0 || strcasecmp(ext, ".xz") == 0)
			return "package-x-generic";
	}
	return "text-x-generic";
}

static void format_size(long long size, char* buf, size_t buflen)
{
	if (size < 1024)
		snprintf(buf, buflen, "%lld B", size);
	else if (size < 1024 * 1024)
		snprintf(buf, buflen, "%.1f KB", size / 1024.0);
	else if (size < 1024LL * 1024 * 1024)
		snprintf(buf, buflen, "%.1f MB", size / (1024.0 * 1024));
	else
		snprintf(buf, buflen, "%.1f GB", size / (1024.0 * 1024 * 1024));
}

static void format_mode(unsigned int mode, char* buf, size_t buflen)
{
	snprintf(buf, buflen, "%c%c%c%c%c%c%c%c%c%c",
		 S_ISDIR(mode) ? 'd' : (S_ISLNK(mode) ? 'l' : '-'),
		 (mode & 0400) ? 'r' : '-', (mode & 0200) ? 'w' : '-',
		 (mode & 0100) ? 'x' : '-', (mode & 0040) ? 'r' : '-',
		 (mode & 0020) ? 'w' : '-', (mode & 0010) ? 'x' : '-',
		 (mode & 0004) ? 'r' : '-', (mode & 0002) ? 'w' : '-',
		 (mode & 0001) ? 'x' : '-');
}

static void format_time(long mtime, char* buf, size_t buflen)
{
	time_t t = mtime;
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(buf, buflen, "%Y-%m-%d %H:%M", &tm);
}

/* ── Directory listing ────────────────────────────────────────── */

static void populate_list(const char* path)
{
	gtk_list_store_clear(app.store);
	strncpy(app.current_path, path, sizeof(app.current_path) - 1);

	/* Update path bar: show path relative to mount point */
	const char* display_path = path + strlen(app.mount_point);
	if (!display_path[0])
		display_path = "/";
	gtk_entry_set_text(GTK_ENTRY(app.path_entry), display_path);

	int fd = lkl_sys_open(path, LKL_O_RDONLY | LKL_O_DIRECTORY, 0);
	if (fd < 0) {
		fprintf(stderr, "Cannot open directory %s: %d\n", path, fd);
		return;
	}

	char buf[4096];
	int nread;
	int count = 0;

	while ((nread = lkl_sys_getdents64(fd, (struct lkl_linux_dirent64*)buf,
					   sizeof(buf))) > 0) {
		int offset = 0;
		while (offset < nread) {
			struct lkl_linux_dirent64* de =
			    (struct lkl_linux_dirent64*)(buf + offset);
			offset += de->d_reclen;

			/* Skip . and .. */
			if (strcmp(de->d_name, ".") == 0 ||
			    strcmp(de->d_name, "..") == 0)
				continue;

			/* Stat the file */
			char fullpath[4096];
			snprintf(fullpath, sizeof(fullpath), "%s/%s", path,
				 de->d_name);

			struct lkl_stat st;
			int ret = lkl_sys_lstat(fullpath, &st);
			if (ret < 0)
				continue;

			char size_str[32], mode_str[16], time_str[32];
			format_size(st.st_size, size_str, sizeof(size_str));
			format_mode(st.st_mode, mode_str, sizeof(mode_str));
			format_time(st.lkl_st_mtime, time_str,
				    sizeof(time_str));

			const char* icon =
			    icon_for_file(de->d_name, st.st_mode);

			GtkTreeIter iter;
			gtk_list_store_append(app.store, &iter);
			gtk_list_store_set(app.store, &iter, COL_ICON, icon,
					   COL_NAME, de->d_name, COL_SIZE,
					   S_ISDIR(st.st_mode) ? "" : size_str,
					   COL_MODIFIED, time_str, COL_MODE,
					   mode_str, COL_IS_DIR,
					   (gboolean)S_ISDIR(st.st_mode),
					   COL_FULLPATH, fullpath, -1);
			count++;
		}
	}
	lkl_sys_close(fd);

	/* Update statusbar */
	char status[128];
	snprintf(status, sizeof(status), "%d items%s", count,
		 app.writable ? "" : " (read-only)");
	gtk_statusbar_push(GTK_STATUSBAR(app.statusbar), 0, status);
}

/* ── Preview ──────────────────────────────────────────────────── */

static gboolean is_text_file(const char* name)
{
	const char* ext = strrchr(name, '.');
	if (!ext)
		return TRUE; /* no extension: assume text */
	const char* text_exts[] = {".txt",  ".md",   ".log",   ".conf", ".cfg",
				   ".ini",  ".sh",   ".c",     ".h",	".py",
				   ".js",   ".json", ".xml",   ".html", ".css",
				   ".yaml", ".yml",  ".toml",  ".rs",	".java",
				   ".go",   ".rb",   ".pl",    ".lua",	".sql",
				   ".csv",  ".diff", ".patch", NULL};
	for (int i = 0; text_exts[i]; i++)
		if (strcasecmp(ext, text_exts[i]) == 0)
			return TRUE;
	return FALSE;
}

static gboolean is_image_file(const char* name)
{
	const char* ext = strrchr(name, '.');
	if (!ext)
		return FALSE;
	const char* img_exts[] = {".png",  ".jpg",  ".jpeg", ".gif",
				  ".bmp",  ".webp", ".svg",  ".ico",
				  ".tiff", ".tif",  NULL};
	for (int i = 0; img_exts[i]; i++)
		if (strcasecmp(ext, img_exts[i]) == 0)
			return TRUE;
	return FALSE;
}

static void show_text_preview(const char* lkl_path)
{
	int fd = lkl_sys_open(lkl_path, LKL_O_RDONLY, 0);
	if (fd < 0)
		return;

	/* Read first 64KB for preview */
	char* buf = malloc(65536 + 1);
	if (!buf) {
		lkl_sys_close(fd);
		return;
	}

	long n = lkl_sys_read(fd, buf, 65536);
	lkl_sys_close(fd);

	if (n <= 0) {
		free(buf);
		return;
	}
	buf[n] = '\0';

	/* Validate UTF-8 or replace non-printable */
	if (!g_utf8_validate(buf, n, NULL)) {
		/* Show as hex dump instead */
		char* hex = g_strdup_printf("[Binary file, %ld bytes]", n);
		GtkTextBuffer* tbuf =
		    gtk_text_view_get_buffer(GTK_TEXT_VIEW(app.preview_text));
		gtk_text_buffer_set_text(tbuf, hex, -1);
		g_free(hex);
	} else {
		GtkTextBuffer* tbuf =
		    gtk_text_view_get_buffer(GTK_TEXT_VIEW(app.preview_text));
		gtk_text_buffer_set_text(tbuf, buf, n);
	}
	free(buf);
	gtk_stack_set_visible_child_name(GTK_STACK(app.preview_stack), "text");
}

static void show_image_preview(const char* lkl_path)
{
	/* Read entire file into memory, then load with GdkPixbuf */
	struct lkl_stat st;
	if (lkl_sys_lstat(lkl_path, &st) < 0)
		return;
	if (st.st_size > 10 * 1024 * 1024)
		return; /* skip >10MB */

	int fd = lkl_sys_open(lkl_path, LKL_O_RDONLY, 0);
	if (fd < 0)
		return;

	char* buf = g_malloc(st.st_size);
	long n = lkl_sys_read(fd, buf, st.st_size);
	lkl_sys_close(fd);

	if (n <= 0) {
		g_free(buf);
		return;
	}

	GInputStream* stream =
	    g_memory_input_stream_new_from_data(buf, n, g_free);
	GError* err = NULL;
	GdkPixbuf* pixbuf = gdk_pixbuf_new_from_stream(stream, NULL, &err);
	g_object_unref(stream);

	if (!pixbuf) {
		if (err)
			g_error_free(err);
		return;
	}

	/* Scale to fit preview area (max 400x400) */
	int w = gdk_pixbuf_get_width(pixbuf);
	int h = gdk_pixbuf_get_height(pixbuf);
	if (w > 400 || h > 400) {
		double scale = MIN(400.0 / w, 400.0 / h);
		GdkPixbuf* scaled = gdk_pixbuf_scale_simple(
		    pixbuf, (int)(w * scale), (int)(h * scale),
		    GDK_INTERP_BILINEAR);
		g_object_unref(pixbuf);
		pixbuf = scaled;
	}

	gtk_image_set_from_pixbuf(GTK_IMAGE(app.preview_image), pixbuf);
	g_object_unref(pixbuf);
	gtk_stack_set_visible_child_name(GTK_STACK(app.preview_stack), "image");
}

static void update_preview(const char* name, const char* fullpath,
			   gboolean is_dir)
{
	if (is_dir) {
		GtkTextBuffer* tbuf =
		    gtk_text_view_get_buffer(GTK_TEXT_VIEW(app.preview_text));
		gtk_text_buffer_set_text(tbuf, "[Directory]", -1);
		gtk_stack_set_visible_child_name(GTK_STACK(app.preview_stack),
						 "text");
		return;
	}

	if (is_image_file(name))
		show_image_preview(fullpath);
	else if (is_text_file(name))
		show_text_preview(fullpath);
	else {
		GtkTextBuffer* tbuf =
		    gtk_text_view_get_buffer(GTK_TEXT_VIEW(app.preview_text));
		struct lkl_stat st;
		if (lkl_sys_lstat(fullpath, &st) == 0) {
			char* msg = g_strdup_printf("[Binary file, %lld bytes]",
						    (long long)st.st_size);
			gtk_text_buffer_set_text(tbuf, msg, -1);
			g_free(msg);
		} else {
			gtk_text_buffer_set_text(tbuf, "[Cannot read file]",
						 -1);
		}
		gtk_stack_set_visible_child_name(GTK_STACK(app.preview_stack),
						 "text");
	}
}

/* ── Signal handlers ──────────────────────────────────────────── */

static void on_row_activated(GtkTreeView* tv, GtkTreePath* path,
			     GtkTreeViewColumn* col, gpointer data)
{
	(void)tv;
	(void)col;
	(void)data;
	GtkTreeIter iter;
	if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(app.store), &iter, path))
		return;

	gboolean is_dir;
	char* fullpath;
	gtk_tree_model_get(GTK_TREE_MODEL(app.store), &iter, COL_IS_DIR,
			   &is_dir, COL_FULLPATH, &fullpath, -1);

	if (is_dir) {
		populate_list(fullpath);
	}
	g_free(fullpath);
}

static void on_selection_changed(GtkTreeSelection* sel, gpointer data)
{
	(void)data;
	GtkTreeIter iter;
	if (!gtk_tree_selection_get_selected(sel, NULL, &iter))
		return;

	gboolean is_dir;
	char *name, *fullpath;
	gtk_tree_model_get(GTK_TREE_MODEL(app.store), &iter, COL_NAME, &name,
			   COL_IS_DIR, &is_dir, COL_FULLPATH, &fullpath, -1);

	update_preview(name, fullpath, is_dir);
	g_free(name);
	g_free(fullpath);
}

static void on_go_up(GtkWidget* btn, gpointer data)
{
	(void)btn;
	(void)data;
	/* Navigate to parent directory */
	if (strcmp(app.current_path, app.mount_point) == 0)
		return; /* already at root */

	char* last_slash = strrchr(app.current_path, '/');
	if (last_slash && last_slash != app.current_path) {
		char parent[4096];
		size_t len = last_slash - app.current_path;
		memcpy(parent, app.current_path, len);
		parent[len] = '\0';
		/* Don't go above mount point */
		if (strlen(parent) >= strlen(app.mount_point))
			populate_list(parent);
	}
}

static void on_path_activate(GtkEntry* entry, gpointer data)
{
	(void)data;
	const char* text = gtk_entry_get_text(entry);
	char fullpath[4096];
	snprintf(fullpath, sizeof(fullpath), "%s%s", app.mount_point,
		 text[0] == '/' ? text : "");
	populate_list(fullpath);
}

/* ── Drag and Drop: Guest → Host ─────────────────────────────── */

static char* extract_to_tmp(const char* lkl_path, const char* name)
{
	/* Extract file from LKL to /tmp for DnD */
	struct lkl_stat st;
	if (lkl_sys_lstat(lkl_path, &st) < 0)
		return NULL;
	if (S_ISDIR(st.st_mode))
		return NULL; /* TODO: recursive extract */

	char* tmp_path =
	    g_strdup_printf("/tmp/anyfs-dnd-%d/%s", getpid(), name);
	char* tmp_dir = g_path_get_dirname(tmp_path);
	g_mkdir_with_parents(tmp_dir, 0700);
	g_free(tmp_dir);

	int src = lkl_sys_open(lkl_path, LKL_O_RDONLY, 0);
	if (src < 0) {
		g_free(tmp_path);
		return NULL;
	}

	FILE* dst = fopen(tmp_path, "wb");
	if (!dst) {
		lkl_sys_close(src);
		g_free(tmp_path);
		return NULL;
	}

	char buf[65536];
	long n;
	while ((n = lkl_sys_read(src, buf, sizeof(buf))) > 0)
		fwrite(buf, 1, n, dst);

	lkl_sys_close(src);
	fclose(dst);
	return tmp_path;
}

static void on_drag_data_get(GtkWidget* widget, GdkDragContext* ctx,
			     GtkSelectionData* sel, guint info, guint time,
			     gpointer data)
{
	(void)widget;
	(void)ctx;
	(void)info;
	(void)time;
	(void)data;

	GtkTreeSelection* selection =
	    gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree_view));
	GtkTreeIter iter;
	if (!gtk_tree_selection_get_selected(selection, NULL, &iter))
		return;

	char *name, *fullpath;
	gtk_tree_model_get(GTK_TREE_MODEL(app.store), &iter, COL_NAME, &name,
			   COL_FULLPATH, &fullpath, -1);

	char* tmp = extract_to_tmp(fullpath, name);
	g_free(name);
	g_free(fullpath);

	if (tmp) {
		char* uri = g_filename_to_uri(tmp, NULL, NULL);
		char* uri_list = g_strdup_printf("%s\r\n", uri);
		gtk_selection_data_set(sel,
				       gdk_atom_intern("text/uri-list", FALSE),
				       8, (guchar*)uri_list, strlen(uri_list));
		g_free(uri);
		g_free(uri_list);
		g_free(tmp);
	}
}

/* ── Drag and Drop: Host → Guest ─────────────────────────────── */

static void on_drag_data_received(GtkWidget* widget, GdkDragContext* ctx,
				  gint x, gint y, GtkSelectionData* sel,
				  guint info, guint time, gpointer data)
{
	(void)widget;
	(void)x;
	(void)y;
	(void)info;
	(void)data;

	if (!app.writable) {
		gtk_drag_finish(ctx, FALSE, FALSE, time);
		return;
	}

	gchar** uris = gtk_selection_data_get_uris(sel);
	if (!uris) {
		gtk_drag_finish(ctx, FALSE, FALSE, time);
		return;
	}

	for (int i = 0; uris[i]; i++) {
		char* path = g_filename_from_uri(uris[i], NULL, NULL);
		if (!path)
			continue;

		const char* basename = strrchr(path, '/');
		basename = basename ? basename + 1 : path;

		/* Create destination path in current LKL directory */
		char dst_path[4096];
		snprintf(dst_path, sizeof(dst_path), "%s/%s", app.current_path,
			 basename);

		/* Copy host file → LKL */
		FILE* src = fopen(path, "rb");
		if (!src) {
			g_free(path);
			continue;
		}

		int dst_fd = lkl_sys_open(
		    dst_path, LKL_O_WRONLY | LKL_O_CREAT | LKL_O_TRUNC, 0644);
		if (dst_fd < 0) {
			fclose(src);
			g_free(path);
			continue;
		}

		char buf[65536];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
			lkl_sys_write(dst_fd, buf, n);

		fclose(src);
		lkl_sys_close(dst_fd);
		g_free(path);
	}
	g_strfreev(uris);
	gtk_drag_finish(ctx, TRUE, FALSE, time);

	/* Refresh listing */
	populate_list(app.current_path);
}

/* ── UI construction ──────────────────────────────────────────── */

static GtkWidget* create_toolbar(void)
{
	GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_set_margin_start(toolbar, 4);
	gtk_widget_set_margin_end(toolbar, 4);
	gtk_widget_set_margin_top(toolbar, 4);
	gtk_widget_set_margin_bottom(toolbar, 4);

	/* Up button */
	GtkWidget* up_btn =
	    gtk_button_new_from_icon_name("go-up", GTK_ICON_SIZE_BUTTON);
	gtk_widget_set_tooltip_text(up_btn, "Go up");
	g_signal_connect(up_btn, "clicked", G_CALLBACK(on_go_up), NULL);
	gtk_box_pack_start(GTK_BOX(toolbar), up_btn, FALSE, FALSE, 0);

	/* Path entry */
	app.path_entry = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(app.path_entry), "/");
	g_signal_connect(app.path_entry, "activate",
			 G_CALLBACK(on_path_activate), NULL);
	gtk_box_pack_start(GTK_BOX(toolbar), app.path_entry, TRUE, TRUE, 0);

	return toolbar;
}

static GtkWidget* create_file_list(void)
{
	/* Create list store */
	app.store = gtk_list_store_new(NUM_COLS, G_TYPE_STRING, /* icon name */
				       G_TYPE_STRING,		/* name */
				       G_TYPE_STRING,		/* size */
				       G_TYPE_STRING,		/* modified */
				       G_TYPE_STRING,		/* mode */
				       G_TYPE_BOOLEAN,		/* is_dir */
				       G_TYPE_STRING		/* fullpath */
	);

	/* Create tree view */
	app.tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app.store));
	g_object_unref(app.store);

	/* Icon + Name column */
	GtkCellRenderer* pix_rend = gtk_cell_renderer_pixbuf_new();
	GtkCellRenderer* txt_rend = gtk_cell_renderer_text_new();
	GtkTreeViewColumn* col_name = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(col_name, "Name");
	gtk_tree_view_column_pack_start(col_name, pix_rend, FALSE);
	gtk_tree_view_column_pack_start(col_name, txt_rend, TRUE);
	gtk_tree_view_column_add_attribute(col_name, pix_rend, "icon-name",
					   COL_ICON);
	gtk_tree_view_column_add_attribute(col_name, txt_rend, "text",
					   COL_NAME);
	gtk_tree_view_column_set_sort_column_id(col_name, COL_NAME);
	gtk_tree_view_column_set_expand(col_name, TRUE);
	gtk_tree_view_column_set_resizable(col_name, TRUE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree_view), col_name);

	/* Size column */
	GtkCellRenderer* size_rend = gtk_cell_renderer_text_new();
	GtkTreeViewColumn* col_size = gtk_tree_view_column_new_with_attributes(
	    "Size", size_rend, "text", COL_SIZE, NULL);
	gtk_tree_view_column_set_sort_column_id(col_size, COL_SIZE);
	gtk_tree_view_column_set_resizable(col_size, TRUE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree_view), col_size);

	/* Modified column */
	GtkCellRenderer* mod_rend = gtk_cell_renderer_text_new();
	GtkTreeViewColumn* col_mod = gtk_tree_view_column_new_with_attributes(
	    "Modified", mod_rend, "text", COL_MODIFIED, NULL);
	gtk_tree_view_column_set_sort_column_id(col_mod, COL_MODIFIED);
	gtk_tree_view_column_set_resizable(col_mod, TRUE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree_view), col_mod);

	/* Permissions column */
	GtkCellRenderer* perm_rend = gtk_cell_renderer_text_new();
	GtkTreeViewColumn* col_perm = gtk_tree_view_column_new_with_attributes(
	    "Permissions", perm_rend, "text", COL_MODE, NULL);
	gtk_tree_view_column_set_resizable(col_perm, TRUE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(app.tree_view), col_perm);

	/* Signals */
	g_signal_connect(app.tree_view, "row-activated",
			 G_CALLBACK(on_row_activated), NULL);
	GtkTreeSelection* sel =
	    gtk_tree_view_get_selection(GTK_TREE_VIEW(app.tree_view));
	gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);
	g_signal_connect(sel, "changed", G_CALLBACK(on_selection_changed),
			 NULL);

	/* Drag source: guest → host */
	GtkTargetEntry drag_targets[] = {
	    {"text/uri-list", 0, 0},
	};
	gtk_tree_view_enable_model_drag_source(GTK_TREE_VIEW(app.tree_view),
					       GDK_BUTTON1_MASK, drag_targets,
					       1, GDK_ACTION_COPY);
	g_signal_connect(app.tree_view, "drag-data-get",
			 G_CALLBACK(on_drag_data_get), NULL);

	/* Drag destination: host → guest */
	gtk_drag_dest_set(app.tree_view, GTK_DEST_DEFAULT_ALL, drag_targets, 1,
			  GDK_ACTION_COPY);
	g_signal_connect(app.tree_view, "drag-data-received",
			 G_CALLBACK(on_drag_data_received), NULL);

	/* Scrolled window */
	GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_AUTOMATIC,
				       GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), app.tree_view);

	return scroll;
}

static GtkWidget* create_preview_panel(void)
{
	app.preview_stack = gtk_stack_new();

	/* Text preview */
	app.preview_text = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(app.preview_text), FALSE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app.preview_text),
				    GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(app.preview_text), TRUE);
	GtkWidget* text_scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(text_scroll),
				       GTK_POLICY_AUTOMATIC,
				       GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(text_scroll), app.preview_text);
	gtk_stack_add_named(GTK_STACK(app.preview_stack), text_scroll, "text");

	/* Image preview */
	app.preview_image = gtk_image_new();
	GtkWidget* img_scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(img_scroll),
				       GTK_POLICY_AUTOMATIC,
				       GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(img_scroll), app.preview_image);
	gtk_stack_add_named(GTK_STACK(app.preview_stack), img_scroll, "image");

	gtk_stack_set_visible_child_name(GTK_STACK(app.preview_stack), "text");

	return app.preview_stack;
}

static void build_ui(void)
{
	app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(app.window), "AnyFS File Manager");
	gtk_window_set_default_size(GTK_WINDOW(app.window), 1000, 600);
	g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit),
			 NULL);

	GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(app.window), vbox);

	/* Toolbar */
	GtkWidget* toolbar = create_toolbar();
	gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

	/* Main content: file list + preview */
	GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_paned_set_position(GTK_PANED(paned), 600);
	gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

	GtkWidget* list = create_file_list();
	gtk_paned_pack1(GTK_PANED(paned), list, TRUE, FALSE);

	GtkWidget* preview = create_preview_panel();
	gtk_paned_pack2(GTK_PANED(paned), preview, TRUE, FALSE);

	/* Status bar */
	app.statusbar = gtk_statusbar_new();
	gtk_box_pack_end(GTK_BOX(vbox), app.statusbar, FALSE, FALSE, 0);
}

/* ── Main ─────────────────────────────────────────────────────── */

static void print_usage(const char* prog)
{
	fprintf(stderr,
		"Usage: %s [-w] [-p partition] <disk.img>\n"
		"  -w        Mount read-write (default: read-only)\n"
		"  -p N      Use partition N (0=whole disk, default: "
		"auto-detect)\n",
		prog);
}

/* Show partition selection dialog. Returns chosen partition number (0..N), or
 * -1 on cancel. */
static int show_partition_dialog(int disk_id, int num_parts)
{
	GtkWidget* dialog = gtk_dialog_new_with_buttons(
	    "Select Partition", NULL, GTK_DIALOG_MODAL, "_Cancel",
	    GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
	gtk_window_set_default_size(GTK_WINDOW(dialog), 400, 300);

	GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
	GtkWidget* label =
	    gtk_label_new("This disk has multiple partitions. Select one:");
	gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 8);

	/* List store: partition number, size string, fstype */
	GtkListStore* store =
	    gtk_list_store_new(3, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
	GtkWidget* tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	g_object_unref(store);

	GtkCellRenderer* rend;
	GtkTreeViewColumn* col;

	rend = gtk_cell_renderer_text_new();
	col = gtk_tree_view_column_new_with_attributes("#", rend, "text", 0,
						       NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);

	rend = gtk_cell_renderer_text_new();
	col = gtk_tree_view_column_new_with_attributes("Size", rend, "text", 1,
						       NULL);
	gtk_tree_view_column_set_expand(col, TRUE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);

	rend = gtk_cell_renderer_text_new();
	col = gtk_tree_view_column_new_with_attributes("Filesystem", rend,
						       "text", 2, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);

	/* Populate partitions */
	for (int i = 1; i <= num_parts; i++) {
		AnyfsMount probe;
		uint32_t mflags = ANYFS_MOUNT_RDONLY;
		int ret =
		    anyfs_mount(disk_id, i, NULL, "probe", mflags, &probe);

		char size_str[32] = "Unknown";
		char fstype_str[32] = "Unknown";

		if (ret == 0) {
			strncpy(fstype_str, probe.fstype,
				sizeof(fstype_str) - 1);
			/* Get size via statfs */
			struct lkl_statfs sfs;
			if (lkl_sys_statfs(probe.mount_point, &sfs) == 0) {
				long long total =
				    (long long)sfs.f_blocks * sfs.f_bsize;
				format_size(total, size_str, sizeof(size_str));
			}
			anyfs_umount("probe");
		}

		GtkTreeIter iter;
		gtk_list_store_append(store, &iter);
		gtk_list_store_set(store, &iter, 0, i, 1, size_str, 2,
				   fstype_str, -1);
	}

	GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_AUTOMATIC,
				       GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), tv);
	gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 4);
	gtk_widget_show_all(content);

	/* Select first row by default */
	GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tv));
	GtkTreeIter first_iter;
	if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &first_iter))
		gtk_tree_selection_select_iter(sel, &first_iter);

	int result = -1;
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
		GtkTreeIter sel_iter;
		if (gtk_tree_selection_get_selected(sel, NULL, &sel_iter)) {
			gtk_tree_model_get(GTK_TREE_MODEL(store), &sel_iter, 0,
					   &result, -1);
		}
	}
	gtk_widget_destroy(dialog);
	return result;
}

int main(int argc, char* argv[])
{
	gtk_init(&argc, &argv);

	/* Parse arguments */
	const char* image_path = NULL;
	app.writable = FALSE;
	int partition = -1; /* -1 = auto-detect */

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-w") == 0)
			app.writable = TRUE;
		else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
			partition = atoi(argv[++i]);
		else if (argv[i][0] != '-')
			image_path = argv[i];
	}

	if (!image_path) {
		print_usage(argv[0]);
		return 1;
	}

	/* Initialize LKL */
	fprintf(stderr, "Initializing LKL kernel...\n");
	AnyfsKernelOpts opts = {.mem_mb = 64, .loglevel = 0};
	int ret = anyfs_kernel_init(&opts);
	if (ret < 0) {
		fprintf(stderr, "Failed to init LKL kernel: %d\n", ret);
		return 1;
	}

	/* Add disk — detect backend by extension */
	uint32_t flags = app.writable ? 0 : ANYFS_DISK_READONLY;
#ifdef ANYFS_HAS_QEMU
	const char* ext = strrchr(image_path, '.');
	if (ext &&
	    (strcasecmp(ext, ".qcow2") == 0 || strcasecmp(ext, ".vmdk") == 0 ||
	     strcasecmp(ext, ".vdi") == 0 || strcasecmp(ext, ".vhd") == 0 ||
	     strcasecmp(ext, ".vhdx") == 0))
		flags |= ANYFS_BACKEND_QEMU;
#endif
	app.disk_id = anyfs_disk_add(image_path, flags);
	if (app.disk_id < 0) {
		fprintf(stderr, "Failed to add disk: %d\n", app.disk_id);
		anyfs_kernel_halt();
		return 1;
	}

	/* Detect partitions */
	unsigned int mount_part = 0;
	if (partition >= 0) {
		mount_part = partition;
	} else {
		int nparts = anyfs_disk_partitions(app.disk_id);
		if (nparts > 1) {
			/* Multi-partition: show selection dialog */
			int chosen = show_partition_dialog(app.disk_id, nparts);
			if (chosen < 0) {
				/* User cancelled */
				anyfs_disk_remove(app.disk_id);
				anyfs_kernel_halt();
				return 0;
			}
			mount_part = chosen;
		} else if (nparts == 1) {
			mount_part = 1;
		}
		/* nparts == 0: no partition table, use whole disk */
	}

	/* Mount */
	AnyfsMount mnt;
	uint32_t mnt_flags = app.writable ? 0 : ANYFS_MOUNT_RDONLY;
	ret =
	    anyfs_mount(app.disk_id, mount_part, NULL, "img", mnt_flags, &mnt);
	if (ret < 0) {
		fprintf(stderr, "Failed to mount partition %u: %d\n",
			mount_part, ret);
		anyfs_disk_remove(app.disk_id);
		anyfs_kernel_halt();
		return 1;
	}
	strncpy(app.mount_point, mnt.mount_point, sizeof(app.mount_point) - 1);
	fprintf(stderr, "Mounted %s (%s) at %s\n", image_path, mnt.fstype,
		mnt.mount_point);

	/* Build UI */
	build_ui();

	/* Populate initial directory listing */
	populate_list(app.mount_point);

	/* Show window and run GTK main loop */
	gtk_widget_show_all(app.window);

	/* Set window title with image info */
	char title[256];
	snprintf(title, sizeof(title), "AnyFS - %s (%s)%s", image_path,
		 mnt.fstype, app.writable ? " [RW]" : " [RO]");
	gtk_window_set_title(GTK_WINDOW(app.window), title);

	gtk_main();

	/* Cleanup */
	/* Remove DnD temp files */
	char tmp_dir[256];
	snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/anyfs-dnd-%d", getpid());
	char rm_cmd[300];
	snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", tmp_dir);
	system(rm_cmd);

	anyfs_umount("img");
	anyfs_disk_remove(app.disk_id);
	anyfs_kernel_halt();

	return 0;
}
