// LklFolderManager.cpp — Opens disk images via LKL

#include "LklFolder.h"
#include "LklFolderManager.h"
#include "LklSession.h"
#include "StdAfx.h"

#include "Windows/PropVariant.h"

// {E7A2B5C1-4F3D-4A6E-B8C2-1D5F7A9E3B4D}
const GUID CLSID_CLklFolderManager = {
    0xE7A2B5C1,
    0x4F3D,
    0x4A6E,
    {0xB8, 0xC2, 0x1D, 0x5F, 0x7A, 0x9E, 0x3B, 0x4D}};

Z7_COM7F_IMF(CLklFolderManager::OpenFolderFile(IInStream* /* inStream */,
					       const wchar_t* filePath,
					       const wchar_t* /* arcFormat */,
					       IFolderFolder** resultFolder,
					       IProgress* /* progress */))
{
	if (!filePath || !resultFolder)
		return E_INVALIDARG;
	*resultFolder = NULL;

	// Convert wchar_t path to UTF-8
	char pathA[4096];
	size_t i;
	for (i = 0; filePath[i] && i < sizeof(pathA) - 1; i++)
		pathA[i] = (char)filePath[i];
	pathA[i] = '\0';

	// Create session and open image
	CLklSession* session = new CLklSession;
	int ret = session->Open(pathA, 1 /* readonly */);
	if (ret < 0) {
		delete session;
		return E_FAIL;
	}

	// Create root folder
	CLklFolder* folder = new CLklFolder;
	CMyComPtr<IFolderFolder> ref(folder);
	UString rootPath(L"/");
	HRESULT hr = folder->Init(session, rootPath);
	if (hr != S_OK) {
		session->Close();
		delete session;
		return hr;
	}

	*resultFolder = ref.Detach();
	return S_OK;
}

Z7_COM7F_IMF(CLklFolderManager::GetExtensions(BSTR* extensions))
{
	// File extensions that this plugin handles
	// 7zFM will try this plugin when user opens files with these extensions
	static const wchar_t* kExtensions =
	    L"img raw qcow2 qcow vhd vhdx vmdk vdi "
	    L"iso bin dd wim";
	*extensions = ::SysAllocString(kExtensions);
	return S_OK;
}

Z7_COM7F_IMF(CLklFolderManager::GetIconPath(const wchar_t* /* ext */,
					    BSTR* iconPath, Int32* iconIndex))
{
	*iconPath = NULL;
	*iconIndex = 0;
	return S_OK;
}
