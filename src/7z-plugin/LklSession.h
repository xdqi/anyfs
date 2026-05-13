// LklSession.h — Shared state for one open disk image
//
// Owns the LKL kernel lifetime and mount.

#ifndef LKL_SESSION_H
#define LKL_SESSION_H

#include "lkl_bridge.h"
#include <stdint.h>

struct CLklSession {
	int disk_id;
	char mount_point[64];
	char fstype[32];
	bool mounted;
	int ref_count;

	CLklSession() : disk_id(-1), mounted(false), ref_count(0)
	{
		mount_point[0] = '\0';
		fstype[0] = '\0';
	}

	int Open(const char* image_path, int readonly);
	void Close();

	// Get the full LKL path for a relative path within the mount
	void GetFullPath(const wchar_t* relPath, char* out, size_t outSize);
};

#endif // LKL_SESSION_H
