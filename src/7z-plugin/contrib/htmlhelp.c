/* htmlhelp.c — Stub for HtmlHelp (not available in MinGW) */
#include <windows.h>

HWND WINAPI HtmlHelpW(HWND hwndCaller, LPCWSTR pszFile, UINT uCommand,
		      DWORD_PTR dwData)
{
	(void)hwndCaller;
	(void)pszFile;
	(void)uCommand;
	(void)dwData;
	return NULL;
}

HWND WINAPI HtmlHelpA(HWND hwndCaller, LPCSTR pszFile, UINT uCommand,
		      DWORD_PTR dwData)
{
	(void)hwndCaller;
	(void)pszFile;
	(void)uCommand;
	(void)dwData;
	return NULL;
}
