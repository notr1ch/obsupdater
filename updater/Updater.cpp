/********************************************************************************
 Copyright (C) 2012 Hugh Bailey <obs.jim@gmail.com>

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
********************************************************************************/

#include "Updater.h"

HANDLE cancelRequested;
HANDLE updateThread;
HINSTANCE hinstMain;
HWND hwndMain;
HCRYPTPROV hProvider;

BOOL bExiting;

int totalFileSize = 0;
int completedFileSize = 0;

VOID Status (const _TCHAR *fmt, ...)
{
    _TCHAR str[512];

    va_list argptr;
    va_start(argptr, fmt);

    StringCbVPrintf(str, sizeof(str), fmt, argptr);

    SetDlgItemText(hwndMain, IDC_STATUS, str);

    va_end(argptr);
}

DWORD WINAPI UpdateThread (VOID *arg)
{
    DWORD ret = 1;

    HANDLE hObsMutex;

    hObsMutex = OpenMutex(SYNCHRONIZE, FALSE, TEXT("OBSMutex"));
    if (hObsMutex)
    {
        WaitForSingleObject(hObsMutex, INFINITE);
        ReleaseMutex (hObsMutex);
        CloseHandle (hObsMutex);
    }

    if (!CryptAcquireContext(&hProvider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
    {
        SetDlgItemText(hwndMain, IDC_STATUS, TEXT("Update failed: CryptAcquireContext failure"));
        return 1;
    }

    SetDlgItemText(hwndMain, IDC_STATUS, TEXT("Searching for available updates..."));

    int responseCode;
    TCHAR extraHeaders[256];
    BYTE manifestHash[20];
    TCHAR manifestPath[MAX_PATH];
    TCHAR tempPath[MAX_PATH];
    TCHAR lpAppDataPath[MAX_PATH];

    SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, lpAppDataPath);
    StringCbCat (lpAppDataPath, sizeof(lpAppDataPath), TEXT("\\OBS"));

    StringCbPrintf (manifestPath, sizeof(manifestPath), TEXT("%s\\updates\\packages.xconfig"), lpAppDataPath);
    StringCbPrintf (tempPath, sizeof(tempPath), TEXT("%s\\updates\\temp"), lpAppDataPath);

    CreateDirectory(tempPath, NULL);

    HANDLE hManifest = CreateFile (manifestPath, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hManifest == INVALID_HANDLE_VALUE)
    {
        Status(TEXT("Update failed: Could not open update manifest"));
        return 1;
    }

    LARGE_INTEGER fileSize;

    if (!GetFileSizeEx(hManifest, &fileSize))
    {
        Status(TEXT("Update failed: Could not check size of update manifest"));
        return 1;
    }

    CHAR *buff = (CHAR *)malloc ((size_t)fileSize.QuadPart + 1);
    if (!buff)
    {
        Status(TEXT("Update failed: Could not allocate memory for update manifest"));
        return 1;
    }

    DWORD read;

    if (!ReadFile (hManifest, buff, (DWORD)fileSize.QuadPart, &read, NULL))
    {
        CloseHandle (hManifest);
        Status(TEXT("Update failed: Error reading update manifest"));
        return 1;
    }

    CloseHandle (hManifest);

    if (read != fileSize.QuadPart)
    {
        Status(_T("Update failed: Failed to read update manifest"));
        return 1;
    }

    buff[read] = 0;

    json_t *root;
    json_error_t error;

    root = json_loads(buff, 0, &error);

    free (buff);

    if (!root)
    {
        Status (_T("Update failed: Couldn't parse update manifest: %S"), error.text);
        return 1;
    }

    if(!json_is_object(root))
    {
        Status(_T("Update failed: Invalid update manifest"));
        return 1;
    }

    const LPSTR baseDirectory = (const LPSTR)arg;

    SetCurrentDirectoryA(baseDirectory);

    const char *packageName;
    json_t *package;

    update_t updateList;

    update_t *updates = &updateList;

    int totalUpdates = 0;

    json_object_foreach (root, packageName, package)
    {
        if (!json_is_object(package))
            goto failure;

        json_t *platform = json_object_get(package, "platform");
        if (!json_is_string(platform))
            continue;

        const char *platformStr = json_string_value(platform);

        if (strcmp(platformStr, "all"))
        {
#if defined _M_IX86
            if (strcmp (platformStr, "Win32"))
                continue;
#elif defined _M_X64
            if (strcmp(platformStr, "Win64"))
                continue;
#endif
        }

        json_t *name = json_object_get(package, "name");
        json_t *version = json_object_get(package, "version");
        json_t *source = json_object_get(package, "source");
        json_t *path = json_object_get(package, "path");
        json_t *files = json_object_get(package, "files");

        if (!json_is_object(files))
            continue;

        if (!json_is_string(path))
            continue;

        const char *pathStr = json_string_value(path);

        json_t *file;
        const char *fileName;

        json_object_foreach (files, fileName, file)
        {
            if (!json_is_object(file))
                continue;

            json_t *hash = json_object_get(file, "hash");
            if (!json_is_string(hash))
                continue;

            const char *hashStr = json_string_value(hash);
            if (strlen(hashStr) != 40)
                continue;

            const char *sourceStr = json_string_value(source);
            if (strncmp (sourceStr, "https://obsproject.com/", 23))
                continue;

            json_t *size = json_object_get(file, "size");
            if (!json_is_integer(size))
                continue;

            int fileSize = (int)json_integer_value(size);

            _TCHAR sourceURL[1024];
            _TCHAR fullPath[MAX_PATH];
            _TCHAR updateFileName[MAX_PATH];
            _TCHAR updateHashStr[41];
            _TCHAR tempFilePath[MAX_PATH];

            if (!MultiByteToWideChar(CP_UTF8, 0, pathStr, -1, fullPath, _countof(fullPath)))
                continue;

            if (!MultiByteToWideChar(CP_UTF8, 0, fileName, -1, updateFileName, _countof(updateFileName)))
                continue;
            StringCbCat(fullPath, sizeof(fullPath), updateFileName);

            if (!MultiByteToWideChar(CP_UTF8, 0, hashStr, -1, updateHashStr, _countof(updateHashStr)))
                continue;

            if (!MultiByteToWideChar(CP_UTF8, 0, sourceStr, -1, sourceURL, _countof(sourceURL)))
                continue;
            StringCbCat(sourceURL, sizeof(sourceURL), updateFileName);

            StringCbPrintf(tempFilePath, sizeof(tempFilePath), _T("%s\\%s"), tempPath, updateHashStr);

            BYTE existingHash[20];

            //We don't really care if this fails, it's just to avoid wasting bandwidth by downloading unmodified files
            if (CalculateFileHash(fullPath, existingHash))
            {
                _TCHAR fileHashStr[41];

                HashToString(existingHash, fileHashStr);

                if (!_tccmp(fileHashStr, updateHashStr))
                    continue;
            }

            updates->next = (update_t *)malloc(sizeof(*updates));
            updates = updates->next;

            updates->next = NULL;
            updates->fileSize = fileSize;
            updates->completed = FALSE;
            updates->outputPath = _tcsdup(fullPath);
            updates->tempPath = _tcsdup(tempFilePath);
            updates->URL = _tcsdup(sourceURL);
            StringToHash(updateHashStr, updates->hash);

            totalUpdates++;
            totalFileSize += fileSize;
        }
    }

    updates = &updateList;
    while (updates->next)
    {
        int responseCode;

        if (WaitForSingleObject(cancelRequested, 0) == WAIT_OBJECT_0)
            goto failure;

        updates = updates->next;
        Status (_T("Updating %s"), updates->outputPath);

        if (!HTTPGetFile(updates->URL, updates->tempPath, _T("Accept-Encoding: gzip"), &responseCode))
        {
            Status (_T("Update failed: Could not download %s"), updates->URL);
            goto failure;
        }

        if (responseCode != 200)
        {
            Status (_T("Update failed: %s (%d)"), updates->URL, responseCode);
            goto failure;
        }

        BYTE downloadHash[20];
        if (!CalculateFileHash(updates->tempPath, downloadHash))
        {
            Status (_T("Update failed: Couldn't verify integrity of %s"), updates->outputPath);
            goto failure;
        }

        if (memcmp(updates->hash, downloadHash, 20))
        {
            Status (_T("Update failed: SHA1 integrity check failed on %s"), updates->outputPath);
            goto failure;
        }
    }

    Status(_T("Update complete."));

    ret = 0;

    SetDlgItemText(hwndMain, IDC_BUTTON, _T("Launch OBS"));

failure:

    RemoveDirectory (tempPath);

    if (bExiting)
        ExitProcess (ret);

    if (ret)
    {
        if (WaitForSingleObject(cancelRequested, 0) == WAIT_OBJECT_0)
            Status (_T("Update aborted."));

        SetDlgItemText(hwndMain, IDC_BUTTON, _T("Exit"));
        EnableWindow (GetDlgItem(hwndMain, IDC_BUTTON), TRUE);
    }

    return ret;

}

VOID CancelUpdate (BOOL quit)
{
    if (WaitForSingleObject(updateThread, 0) != WAIT_OBJECT_0)
    {
        bExiting = quit;
        SetEvent (cancelRequested);
    }
    else
    {
        PostQuitMessage(0);
    }
}

VOID LaunchOBS ()
{
    ExitProcess (0);
}

INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch(message)
    {
        case WM_INITDIALOG:
            {
                return TRUE;
            }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BUTTON)
            {
                if (HIWORD(wParam) == BN_CLICKED)
                {
                    if (WaitForSingleObject(updateThread, 0) == WAIT_OBJECT_0)
                    {
                        LaunchOBS ();
                    }
                    else
                    {
                        EnableWindow ((HWND)lParam, FALSE);
                        CancelUpdate (FALSE);
                    }
                }
            }
            return TRUE;

        case WM_CLOSE:
            CancelUpdate (TRUE);
            return TRUE;
    }

    return FALSE;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    INITCOMMONCONTROLSEX icce;

    hinstMain = hInstance;

    icce.dwSize = sizeof(icce);
    icce.dwICC = ICC_PROGRESS_CLASS;

    InitCommonControlsEx(&icce);

    hwndMain = CreateDialog(hInstance, MAKEINTRESOURCE(IDD_UPDATEDIALOG), NULL, DialogProc);

    if (hwndMain)
        ShowWindow(hwndMain, SW_SHOWNORMAL);

    cancelRequested = CreateEvent (NULL, TRUE, FALSE, NULL);

    updateThread = CreateThread (NULL, 0, UpdateThread, lpCmdLine, 0, NULL);

    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0))
    {
        if(!IsDialogMessage(hwndMain, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
}