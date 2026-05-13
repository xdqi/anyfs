// LklSession.cpp — LKL kernel + disk management

#include "LklSession.h"
#include "StdAfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_session_count = 0;

int CLklSession::Open(const char* image_path, int readonly)
{
	int ret = lklb_kernel_init(64);
	if (ret < 0)
		return ret;

	disk_id = lklb_disk_add(image_path, readonly);
	if (disk_id < 0)
		return disk_id;

	char name[32];
	snprintf(name, sizeof(name), "img%d", g_session_count++);

	ret = lklb_mount(disk_id, 0, name, mount_point, sizeof(mount_point),
			 fstype, sizeof(fstype));
	if (ret < 0) {
		lklb_disk_remove(disk_id);
		disk_id = -1;
		return ret;
	}
	mounted = true;
	return 0;
}

void CLklSession::Close()
{
	if (mounted) {
		// Extract name from mount_point: "/lklmnt/<name>" → "<name>"
		const char* name = mount_point + 8; // skip "/lklmnt/"
		lklb_umount(name);
		mounted = false;
	}
	if (disk_id >= 0) {
		lklb_disk_remove(disk_id);
		disk_id = -1;
	}
}

void CLklSession::GetFullPath(const wchar_t* relPath, char* out, size_t outSize)
{
	char rel_utf8[4096];
	size_t i = 0;
	if (relPath) {
		for (; relPath[i] && i < sizeof(rel_utf8) - 1; i++)
			rel_utf8[i] = (char)relPath[i];
	}
	rel_utf8[i] = '\0';

	if (rel_utf8[0] == '/' || rel_utf8[0] == '\\')
		snprintf(out, outSize, "%s%s", mount_point, rel_utf8);
	else if (rel_utf8[0] == '\0')
		snprintf(out, outSize, "%s", mount_point);
	else
		snprintf(out, outSize, "%s/%s", mount_point, rel_utf8);
}
