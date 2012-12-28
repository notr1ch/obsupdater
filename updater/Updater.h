#pragma once

#define WINVER         0x0600
#define _WIN32_WINDOWS 0x0600
#define _WIN32_WINNT   0x0600
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winhttp.h>
#include <commctrl.h>
#include <Wincrypt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <malloc.h>
#include <stdlib.h>
#include <tchar.h>
#include <strsafe.h>
#include <zlib.h>

#if defined _M_IX86
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")
#elif defined _M_IA64
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='ia64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#elif defined _M_X64
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#else
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#include <jansson.h>
#include "resource.h"

typedef struct update_s
{
    struct update_s *next;
    _TCHAR          *URL;
    _TCHAR          *outputPath;
    _TCHAR          *tempPath;
    DWORD           fileSize;
    BYTE            hash[20];
    BOOL            completed;
} update_t;

BOOL HTTPGetFile (const _TCHAR *url, const _TCHAR *outputPath, const _TCHAR *extraHeaders, int *responseCode);

VOID HashToString (BYTE *in, TCHAR *out);
VOID StringToHash (TCHAR *in, BYTE *out);

BOOL CalculateFileHash (TCHAR *path, BYTE *hash);

extern HWND hwndMain;
extern HCRYPTPROV hProvider;
extern int totalFileSize;
extern int completedFileSize;
extern HANDLE cancelRequested;
