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
BOOL updateFailed = FALSE;

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

VOID CreateFoldersForPath (_TCHAR *path)
{
    _TCHAR *p = path;

    while (*p)
    {
        if (*p == '\\' || *p == '/')
        {
            *p = 0;
            CreateDirectory (path, NULL);
            *p = '\\';
        }
        p++;
    }
}

VOID CleanupPartialUpdates (update_t *updates)
{
    while (updates->next)
    {
        updates = updates->next;

        if (updates->state == STATE_INSTALLED)
        {
            if (updates->previousFile)
            {
                DeleteFile (updates->outputPath);
                MoveFile (updates->previousFile, updates->outputPath);
            }
            else
            {
                DeleteFile (updates->outputPath);
            }
        }
        else if (updates->state == STATE_DOWNLOADED)
        {
            DeleteFile (updates->tempPath);
        }
    }
}

VOID DestroyUpdateList (update_t *updates)
{
    update_t *next;

    updates = updates->next;
    if (!updates)
        return;

    updates->next = NULL;

    while (updates)
    {
        next = updates->next;

        if (updates->outputPath)
            free (updates->outputPath);
        if (updates->previousFile)
            free (updates->previousFile);
        if (updates->tempPath)
            free (updates->tempPath);
        if (updates->URL)
            free (updates->URL);

        free (updates);

        updates = next;
    }
}

BOOL IsSafeFilename (_TCHAR *path)
{
    const _TCHAR *p;

    p = path;

    if (!*p)
       return FALSE;

    if (_tcsstr(path, _T("..")))
        return FALSE;

    if (*p == '/')
        return FALSE;

    while (*p)
    {
        if (!isalnum(*p) && *p != '.' && *p != '/' && *p != '_')
            return FALSE;
        p++;
    }

    return TRUE;
}

BOOL IsSafePath (_TCHAR * path)
{
    const _TCHAR *p;

    p = path;

    if (!*p)
        return TRUE;

    if (!isalnum(*p))
        return FALSE;

    while (*p)
    {
        if (*p == '.' || *p == '\\')
            return FALSE;
        p++;
    }
    
    return TRUE;
}

DWORD WINAPI UpdateThread (VOID *arg)
{
    DWORD ret = 1;

    update_t updateList = {0};
    update_t *updates = &updateList;

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

    const _TCHAR *targetPlatform = (const _TCHAR *)arg;
    if (!targetPlatform[0])
    {
        Status(_T("Update failed: Missing platform paramater."));
        return 1;
    }

    //----------------------
    //Parse update manifest
    //----------------------
    const char *packageName;
    json_t *package;

    int totalUpdates = 0;
    int completedUpdates = 0;

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
            if (!_tcscmp(targetPlatform, _T("Win32")))
            {
                if (strcmp (platformStr, "Win32"))
                    continue;
            }
            else if (!_tcscmp(targetPlatform, _T("Win64")))
            {
                if (strcmp(platformStr, "Win64"))
                    continue;
            }
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

            if (!IsSafePath(fullPath))
            {
                Status (_T("Update failed: Unsafe path '%s' found in manifest"), fullPath);
                goto failure;
            }

            if (!MultiByteToWideChar(CP_UTF8, 0, fileName, -1, updateFileName, _countof(updateFileName)))
                continue;

            if (!IsSafeFilename(updateFileName))
            {
                Status (_T("Update failed: Unsafe path '%s' found in manifest"), updateFileName);
                goto failure;
            }

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

                if (!_tcscmp(fileHashStr, updateHashStr))
                    continue;
            }

            updates->next = (update_t *)malloc(sizeof(*updates));
            updates = updates->next;

            updates->next = NULL;
            updates->fileSize = fileSize;
            updates->previousFile = NULL;
            updates->outputPath = _tcsdup(fullPath);
            updates->tempPath = _tcsdup(tempFilePath);
            updates->URL = _tcsdup(sourceURL);
            updates->state = STATE_PENDING_DOWNLOAD;
            StringToHash(updateHashStr, updates->hash);

            totalUpdates++;
            totalFileSize += fileSize;
        }
    }

    json_decref(root);

    //-------------------
    //Download Updates
    //-------------------
    if (totalUpdates)
    {
        updates = &updateList;
        while (updates->next)
        {
            int responseCode;

            if (WaitForSingleObject(cancelRequested, 0) == WAIT_OBJECT_0)
                goto failure;

            updates = updates->next;
            Status (_T("Downloading %s"), updates->outputPath);

            if (!HTTPGetFile(updates->URL, updates->tempPath, _T("Accept-Encoding: gzip"), &responseCode))
            {
                DeleteFile (updates->tempPath);
                Status (_T("Update failed: Could not download %s"), updates->URL);
                goto failure;
            }

            if (responseCode != 200)
            {
                DeleteFile (updates->tempPath);
                Status (_T("Update failed: %s (%d)"), updates->URL, responseCode);
                goto failure;
            }

            BYTE downloadHash[20];
            if (!CalculateFileHash(updates->tempPath, downloadHash))
            {
                DeleteFile (updates->tempPath);
                Status (_T("Update failed: Couldn't verify integrity of %s"), updates->outputPath);
                goto failure;
            }

            if (memcmp(updates->hash, downloadHash, 20))
            {
                DeleteFile (updates->tempPath);
                Status (_T("Update failed: Integrity check failed on %s"), updates->outputPath);
                goto failure;
            }

            updates->state = STATE_DOWNLOADED;
            completedUpdates++;
        }

        //----------------
        //Install updates
        //----------------
        if (completedUpdates == totalUpdates)
        {
            _TCHAR oldFileRenamedPath[MAX_PATH];

            updates = &updateList;
            while (updates->next)
            {
                updates = updates->next;

                Status (_T("Installing %s..."), updates->outputPath);

                //Check if we're replacing an existing file or just installing a new one
                if (GetFileAttributes(updates->outputPath) != INVALID_FILE_ATTRIBUTES)
                {
                    //Backup the existing file in case a rollback is needed
                    StringCbCopy(oldFileRenamedPath, sizeof(oldFileRenamedPath), updates->outputPath);
                    StringCbCat(oldFileRenamedPath, sizeof(oldFileRenamedPath), _T(".old"));
                    DeleteFile(oldFileRenamedPath);
                    if (!MoveFile(updates->outputPath, oldFileRenamedPath))
                    {
                        Status (_T("Update failed: Couldn't move existing %s (error %d)"), updates->outputPath, GetLastError());
                        goto failure;
                    }
                    if (!MoveFile(updates->tempPath, updates->outputPath))
                    {
                        Status (_T("Update failed: Couldn't move updated %s (error %d)"), updates->outputPath, GetLastError());
                        goto failure;
                    }

                    updates->previousFile = _tcsdup(oldFileRenamedPath);
                    updates->state = STATE_INSTALLED;
                }
                else
                {
                    //We may be installing into new folders, make sure they exist
                    CreateFoldersForPath (updates->outputPath);

                    if (!MoveFile(updates->tempPath, updates->outputPath))
                    {
                        Status (_T("Update failed: Couldn't install %s (error %d)"), updates->outputPath, GetLastError());
                        goto failure;
                    }

                    updates->previousFile = NULL;
                    updates->state = STATE_INSTALLED;
                }
            }

            //If we get here, all updates installed successfully so we can purge the old versions
            updates = &updateList;
            while (updates->next)
            {
                updates = updates->next;

                if (updates->previousFile)
                    DeleteFile (updates->previousFile);
            }
        }

        Status(_T("Update complete."));
    }
    else
    {
        Status (_T("All available updates are already installed."));
    }

    ret = 0;

    SetDlgItemText(hwndMain, IDC_BUTTON, _T("Launch OBS"));

failure:

    if (ret)
    {
        //This handles deleting temp files and rolling back and partially installed updates
        CleanupPartialUpdates (&updateList);
        RemoveDirectory (tempPath);

        if (WaitForSingleObject(cancelRequested, 0) == WAIT_OBJECT_0)
            Status (_T("Update aborted."));

        SendDlgItemMessage(hwndMain, IDC_PROGRESS, PBM_SETSTATE, PBST_ERROR, 0);

        SetDlgItemText(hwndMain, IDC_BUTTON, _T("Exit"));
        EnableWindow (GetDlgItem(hwndMain, IDC_BUTTON), TRUE);

        updateFailed = TRUE;
    }
    else
    {
        RemoveDirectory (tempPath);
    }

    DestroyUpdateList (&updateList);

    if (bExiting)
        ExitProcess (ret);

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
    _TCHAR cwd[MAX_PATH];
    _TCHAR obsPath[MAX_PATH];

    GetCurrentDirectory(_countof(cwd)-1, cwd);

    StringCbCopy(obsPath, sizeof(obsPath), cwd);
    StringCbCat(obsPath, sizeof(obsPath), _T("\\OBS.exe"));

    SHELLEXECUTEINFO execInfo;

    ZeroMemory(&execInfo, sizeof(execInfo));

    execInfo.cbSize = sizeof(execInfo);
    execInfo.lpFile = obsPath;
    execInfo.lpDirectory = cwd;
    execInfo.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteEx (&execInfo))
        Status(_T("Can't launch OBS: Error %d"), GetLastError());
    else
        ExitProcess (0);
}

INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch(message)
    {
        case WM_INITDIALOG:
            {
                //SendDlgItemMessage (hwnd, IDC_PROGRESS, PBM_SETRANGE, 0, MAKELPARAM(0, 200));
                return TRUE;
            }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BUTTON)
            {
                if (HIWORD(wParam) == BN_CLICKED)
                {
                    if (WaitForSingleObject(updateThread, 0) == WAIT_OBJECT_0)
                    {
                        if (updateFailed)
                            PostQuitMessage(0);
                        else
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

int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nShowCmd)
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

    return 0;
}