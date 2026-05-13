// LklFolder.cpp — IFolderFolder implementation using LKL VFS

#include "LklFolder.h"
#include "LklSession.h"
#include "StdAfx.h"

#include "PropID.h"
#include "Windows/PropVariant.h"

#include "lkl_bridge.h"
#include <stdio.h>
#include <string.h>

// Convert relative path within mount to full LKL path
static void MakeLklPath(CLklSession* session, const UString& relPath, char* out,
			size_t outSize)
{
	AString a;
	for (unsigned i = 0; i < relPath.Len(); i++)
		a += (char)relPath[i];
	session->GetFullPath((const wchar_t*)relPath, out, outSize);
}

// Helper: FILETIME from Unix timestamp
static FILETIME UnixTimeToFileTime(UInt64 unixTime)
{
	// Windows FILETIME: 100-nanosecond intervals since 1601-01-01
	// Unix time: seconds since 1970-01-01
	// Difference: 11644473600 seconds
	UInt64 ft = (unixTime + 11644473600ULL) * 10000000ULL;
	FILETIME result;
	result.dwLowDateTime = (DWORD)ft;
	result.dwHighDateTime = (DWORD)(ft >> 32);
	return result;
}

HRESULT CLklFolder::Init(CLklSession* session, const UString& path)
{
	_session = session;
	_path = path;
	_session->ref_count++;
	return S_OK;
}

HRESULT CLklFolder::ReadDir()
{
	_items.Clear();

	char lklPath[4096];
	MakeLklPath(_session, _path, lklPath, sizeof(lklPath));

	lklb_dir_t* dir = lklb_opendir(lklPath);
	if (!dir)
		return E_FAIL;

	LklbDirEntry de;
	while (lklb_readdir(dir, &de)) {
		// Skip . and ..
		if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
			continue;

		CLklDirItem item;
		for (const char* p = de.name; *p; p++)
			item.Name += (wchar_t)(unsigned char)*p;

		item.IsDir = de.is_dir;
		item.Attrib = item.IsDir ? FILE_ATTRIBUTE_DIRECTORY
					 : FILE_ATTRIBUTE_NORMAL;
		item.Size = 0;
		item.MTime = 0;

		// Get stat info
		char entryPath[4096];
		snprintf(entryPath, sizeof(entryPath), "%s/%s", lklPath,
			 de.name);
		uint64_t size, mtime;
		int is_dir;
		if (lklb_stat(entryPath, &size, &mtime, &is_dir) == 0) {
			item.Size = size;
			item.MTime = mtime;
		}

		_items.Add(item);
	}
	lklb_closedir(dir);
	return S_OK;
}

// ─── IFolderFolder ───────────────────────────────────────────────

Z7_COM7F_IMF(CLklFolder::LoadItems())
{
	return ReadDir();
}

Z7_COM7F_IMF(CLklFolder::GetNumberOfItems(UInt32* numItems))
{
	*numItems = _items.Size();
	return S_OK;
}

Z7_COM7F_IMF(CLklFolder::GetProperty(UInt32 itemIndex, PROPID propID,
				     PROPVARIANT* value))
{
	if (itemIndex >= (UInt32)_items.Size())
		return E_INVALIDARG;

	NWindows::NCOM::CPropVariant prop;
	const CLklDirItem& item = _items[itemIndex];

	switch (propID) {
	case kpidName:
		prop = item.Name;
		break;
	case kpidIsDir:
		prop = item.IsDir;
		break;
	case kpidSize:
		if (!item.IsDir)
			prop = item.Size;
		break;
	case kpidPackSize:
		if (!item.IsDir)
			prop = item.Size;
		break;
	case kpidMTime:
		if (item.MTime != 0) {
			FILETIME ft = UnixTimeToFileTime(item.MTime);
			prop = ft;
		}
		break;
	case kpidAttrib:
		prop = item.Attrib;
		break;
	default:
		break;
	}
	prop.Detach(value);
	return S_OK;
}

Z7_COM7F_IMF(CLklFolder::BindToFolder(UInt32 index,
				      IFolderFolder** resultFolder))
{
	if (index >= (UInt32)_items.Size())
		return E_INVALIDARG;
	if (!_items[index].IsDir)
		return E_INVALIDARG;

	UString subPath = _path;
	if (subPath.Back() != L'/')
		subPath += L'/';
	subPath += _items[index].Name;

	CLklFolder* folder = new CLklFolder;
	CMyComPtr<IFolderFolder> ref(folder);
	HRESULT hr = folder->Init(_session, subPath);
	if (hr != S_OK)
		return hr;
	*resultFolder = ref.Detach();
	return S_OK;
}

Z7_COM7F_IMF(CLklFolder::BindToFolder(const wchar_t* name,
				      IFolderFolder** resultFolder))
{
	// Find item by name
	for (unsigned i = 0; i < _items.Size(); i++) {
		if (_items[i].Name == name && _items[i].IsDir) {
			return BindToFolder(i, resultFolder);
		}
	}
	return E_INVALIDARG;
}

Z7_COM7F_IMF(CLklFolder::BindToParentFolder(IFolderFolder** resultFolder))
{
	// Find last '/' in path
	int lastSlash = _path.ReverseFind(L'/');
	if (lastSlash <= 0) {
		*resultFolder = NULL;
		return S_OK;
	}

	UString parentPath;
	parentPath.SetFrom(_path, lastSlash);

	CLklFolder* folder = new CLklFolder;
	CMyComPtr<IFolderFolder> ref(folder);
	HRESULT hr = folder->Init(_session, parentPath);
	if (hr != S_OK)
		return hr;
	*resultFolder = ref.Detach();
	return S_OK;
}

Z7_COM7F_IMF(CLklFolder::GetNumberOfProperties(UInt32* numProperties))
{
	*numProperties = 4; // Name, Size, MTime, Attrib
	return S_OK;
}

Z7_COM7F_IMF(CLklFolder::GetPropertyInfo(UInt32 index, BSTR* name,
					 PROPID* propID, VARTYPE* varType))
{
	*name = NULL;
	switch (index) {
	case 0:
		*propID = kpidName;
		*varType = VT_BSTR;
		break;
	case 1:
		*propID = kpidSize;
		*varType = VT_UI8;
		break;
	case 2:
		*propID = kpidMTime;
		*varType = VT_FILETIME;
		break;
	case 3:
		*propID = kpidAttrib;
		*varType = VT_UI4;
		break;
	default:
		return E_INVALIDARG;
	}
	return S_OK;
}

Z7_COM7F_IMF(CLklFolder::GetFolderProperty(PROPID propID, PROPVARIANT* value))
{
	NWindows::NCOM::CPropVariant prop;
	switch (propID) {
	case kpidPath:
		prop = _path;
		break;
	case kpidType:
		prop = L"LKL";
		break;
	}
	prop.Detach(value);
	return S_OK;
}

// ─── IFolderGetItemName ──────────────────────────────────────────

Z7_COM7F_IMF(CLklFolder::GetItemName(UInt32 index, const wchar_t** name,
				     unsigned* len))
{
	if (index >= (UInt32)_items.Size())
		return E_INVALIDARG;
	*name = _items[index].Name;
	*len = _items[index].Name.Len();
	return S_OK;
}

Z7_COM7F_IMF(CLklFolder::GetItemPrefix(UInt32 /* index */, const wchar_t** name,
				       unsigned* len))
{
	*name = L"";
	*len = 0;
	return S_OK;
}

Z7_COM7F_IMF2(UInt64, CLklFolder::GetItemSize(UInt32 index))
{
	if (index >= (UInt32)_items.Size())
		return 0;
	return _items[index].Size;
}

// ─── IFolderOperations (extract files) ──────────────────────────

Z7_COM7F_IMF(CLklFolder::CreateFolder(const wchar_t* /* name */,
				      IProgress* /* progress */))
{
	return E_NOTIMPL; // read-only
}

Z7_COM7F_IMF(CLklFolder::CreateFile(const wchar_t* /* name */,
				    IProgress* /* progress */))
{
	return E_NOTIMPL;
}

Z7_COM7F_IMF(CLklFolder::Rename(UInt32 /* index */,
				const wchar_t* /* newName */,
				IProgress* /* progress */))
{
	return E_NOTIMPL;
}

Z7_COM7F_IMF(CLklFolder::Delete(const UInt32* /* indices */,
				UInt32 /* numItems */,
				IProgress* /* progress */))
{
	return E_NOTIMPL;
}

Z7_COM7F_IMF(CLklFolder::CopyTo(Int32 /* moveMode */, const UInt32* indices,
				UInt32 numItems, Int32 /* includeAltStreams */,
				Int32 /* replaceAltStreamCharsMode */,
				const wchar_t* path,
				IFolderOperationsExtractCallback* callback))
{
	// Extract files from LKL to host filesystem
	for (UInt32 i = 0; i < numItems; i++) {
		UInt32 idx = indices[i];
		if (idx >= (UInt32)_items.Size())
			continue;

		const CLklDirItem& item = _items[idx];
		if (item.IsDir)
			continue; // TODO: recursive extract

		// Build LKL source path
		char srcPath[4096];
		char lklDir[4096];
		MakeLklPath(_session, _path, lklDir, sizeof(lklDir));
		AString nameA;
		for (unsigned j = 0; j < item.Name.Len(); j++)
			nameA += (char)item.Name[j];
		snprintf(srcPath, sizeof(srcPath), "%s/%s", lklDir,
			 (const char*)nameA);

		// Build host destination path
		UString destPath(path);
		destPath += item.Name;
		AString destPathA;
		for (unsigned j = 0; j < destPath.Len(); j++)
			destPathA += (char)destPath[j];

		if (callback)
			callback->SetCurrentFilePath(item.Name);

		// Open source in LKL
		int fd = lklb_open(srcPath);
		if (fd < 0)
			continue;

		// Open destination on host
		FILE* fp = fopen((const char*)destPathA, "wb");
		if (!fp) {
			lklb_close(fd);
			continue;
		}

		// Copy data
		char buf[65536];
		long n;
		while ((n = lklb_read(fd, buf, sizeof(buf))) > 0) {
			fwrite(buf, 1, (size_t)n, fp);
		}
		fclose(fp);
		lklb_close(fd);
	}
	return S_OK;
}

Z7_COM7F_IMF(CLklFolder::CopyFrom(Int32 /* moveMode */,
				  const wchar_t* /* fromFolderPath */,
				  const wchar_t* const* /* itemsPaths */,
				  UInt32 /* numItems */,
				  IProgress* /* progress */))
{
	return E_NOTIMPL; // read-only
}

Z7_COM7F_IMF(CLklFolder::SetProperty(UInt32 /* index */, PROPID /* propID */,
				     const PROPVARIANT* /* value */,
				     IProgress* /* progress */))
{
	return E_NOTIMPL;
}

Z7_COM7F_IMF(CLklFolder::CopyFromFile(UInt32 /* index */,
				      const wchar_t* /* fullFilePath */,
				      IProgress* /* progress */))
{
	return E_NOTIMPL;
}
