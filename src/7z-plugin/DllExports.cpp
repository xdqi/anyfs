// DllExports.cpp — DLL entry point and exported functions for 7zFM plugin
//
// Exports:
//   CreateObject  — creates CLklFolderManager
//   GetPluginProperty — returns plugin metadata

#include "LklFolderManager.h"
#include "LklSession.h"
#include "StdAfx.h"

#include "Common/MyCom.h"
#include "UI/FileManager/IFolder.h"
#include "Windows/PropVariant.h"

#include <windows.h>

HINSTANCE g_hInstance = NULL;

extern "C" BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason,
			       LPVOID /*lpReserved*/)
{
	if (dwReason == DLL_PROCESS_ATTACH) {
		g_hInstance = hInstance;
	}
	return TRUE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateObject(const GUID* clsid,
							     const GUID* iid,
							     void** outObject)
{
	if (!clsid || !iid || !outObject)
		return E_INVALIDARG;
	*outObject = NULL;

	if (*clsid == CLSID_CLklFolderManager && *iid == IID_IFolderManager) {
		CLklFolderManager* mgr = new CLklFolderManager;
		CMyComPtr<IFolderManager> ref(mgr);
		*outObject = ref.Detach();
		return S_OK;
	}
	return CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI
GetPluginProperty(PROPID propID, PROPVARIANT* value)
{
	NWindows::NCOM::CPropVariant prop;
	switch (propID) {
	case 0: // NPlugin::kName
		prop = L"LKL Disk Image";
		break;
	case 1: // NPlugin::kType (kPluginTypeFF = file folder)
		prop = (UInt32)0; // kPluginTypeFF
		break;
	case 2: // NPlugin::kClassID
	{
		prop.vt = VT_BSTR;
		prop.bstrVal = ::SysAllocStringByteLen(
		    (const char*)&CLSID_CLklFolderManager, sizeof(GUID));
		break;
	}
	}
	prop.Detach(value);
	return S_OK;
}
