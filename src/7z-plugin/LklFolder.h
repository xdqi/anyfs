// LklFolder.h — IFolderFolder implementation backed by LKL VFS
//
// Browses directories inside a disk image using Linux Kernel Library.

#ifndef LKL_FOLDER_H
#define LKL_FOLDER_H

#include "Common/MyCom.h"
#include "Common/MyString.h"
#include "Common/MyVector.h"
#include "UI/FileManager/IFolder.h"

struct CLklDirItem {
	UString Name;
	UInt64 Size;
	UInt64 MTime;  // Unix timestamp
	UInt32 Attrib; // FILE_ATTRIBUTE_DIRECTORY etc.
	bool IsDir;
};

// Forward declaration — shared state across all folders of one open image
struct CLklSession;

class CLklFolder Z7_final : public IFolderFolder,
			    public IFolderGetItemName,
			    public IFolderOperations,
			    public CMyUnknownImp
{
	Z7_COM_QI_BEGIN2(IFolderFolder)
	Z7_COM_QI_ENTRY(IFolderGetItemName)
	Z7_COM_QI_ENTRY(IFolderOperations)
	Z7_COM_QI_END
	Z7_COM_ADDREF_RELEASE

	Z7_IFACE_COM7_IMP(IFolderFolder)
	Z7_IFACE_COM7_IMP(IFolderGetItemName)
	Z7_IFACE_COM7_IMP(IFolderOperations)

      public:
	CLklFolder()
	{
	}

	// Initialize with a session and current path inside the mounted FS
	HRESULT Init(CLklSession* session, const UString& path);

      private:
	CLklSession* _session; // shared, not owned
	UString _path; // current directory path (e.g., "/lklmnt/img0/home")
	CObjectVector<CLklDirItem> _items;

	HRESULT ReadDir();
};

#endif // LKL_FOLDER_H
