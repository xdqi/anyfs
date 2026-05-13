// LklFolderManager.h — IFolderManager implementation for LKL disk images

#ifndef LKL_FOLDER_MANAGER_H
#define LKL_FOLDER_MANAGER_H

#include "Common/MyCom.h"
#include "UI/FileManager/IFolder.h"

Z7_CLASS_IMP_COM_1(CLklFolderManager, IFolderManager)
public:
CLklFolderManager()
{
}
}
;

// {E7A2B5C1-4F3D-4A6E-B8C2-1D5F7A9E3B4D}
extern const GUID CLSID_CLklFolderManager;

#endif // LKL_FOLDER_MANAGER_H
