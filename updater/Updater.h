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
#include <ctype.h>

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

typedef enum
{
    STATE_INVALID,
    STATE_PENDING_DOWNLOAD,
    STATE_DOWNLOADING,
    STATE_DOWNLOADED,
    STATE_INSTALLED,
} state_t;

typedef struct update_s
{
    struct update_s *next;
    _TCHAR          *sourceURL;
    _TCHAR          *outputPath;
    _TCHAR          *tempPath;
    _TCHAR          *previousFile;
    _TCHAR          *basename;
    DWORD           fileSize;
    BYTE            hash[20];
    state_t         state;
    int             has_hash;
    int             patchable;
    BYTE            my_hash[20];
    char            *packageName;
} update_t;

BOOL HTTPGetFile (HINTERNET hSession, HINTERNET hConnect, const _TCHAR *url, const _TCHAR *outputPath, const _TCHAR *extraHeaders, int *responseCode);
BOOL HTTPPostData(const _TCHAR *url, const BYTE *data, int dataLen, const _TCHAR *extraHeaders, int *responseCode, BYTE **response, int *responseLen);

VOID HashToString (BYTE *in, TCHAR *out);
VOID StringToHash (TCHAR *in, BYTE *out);

BOOL CalculateFileHash (TCHAR *path, BYTE *hash);
BOOL VerifyDigitalSignature(BYTE *buff, DWORD len, BYTE *signature, DWORD signatureLen);

BOOL ApplyPatch(LPCTSTR patchFile, LPCTSTR targetFile);

extern HWND hwndMain;
extern HCRYPTPROV hProvider;
extern int totalFileSize;
extern int completedFileSize;
extern HANDLE cancelRequested;

#pragma pack(push, r1, 1)

typedef struct {
    BLOBHEADER blobheader;
    RSAPUBKEY rsapubkey;
} PUBLICKEYHEADER;

#pragma pack(pop, r1)
