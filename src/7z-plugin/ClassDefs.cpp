// ClassDefs.cpp — Initialize COM interface GUIDs for this DLL

#include "StdAfx.h"

#include "Common/MyInitGuid.h"
#include "Common/MyWindows.h"

// This triggers DEFINE_GUID to create actual GUID values (not just extern refs)
// for IFolderFolder, IFolderManager, IFolderGetItemName, IFolderOperations,
// etc.
#include "UI/FileManager/IFolder.h"
