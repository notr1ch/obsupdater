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

CRITICAL_SECTION updateMutex;

HANDLE cancelRequested;
HANDLE updateThread;
HINSTANCE hinstMain;
HWND hwndMain;
HCRYPTPROV hProvider;

BOOL bExiting;
BOOL updateFailed = FALSE;

BOOL downloadThreadFailure = FALSE;

int totalFileSize = 0;
int completedFileSize = 0;
int completedUpdates = 0;

//http://www.codeproject.com/Articles/320748/Haephrati-Elevating-during-runtime
BOOL IsAppRunningAsAdminMode()
{
    BOOL fIsRunAsAdmin = FALSE;
    DWORD dwError = ERROR_SUCCESS;
    PSID pAdministratorsGroup = NULL;

    // Allocate and initialize a SID of the administrators group.
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(
        &NtAuthority, 
        2, 
        SECURITY_BUILTIN_DOMAIN_RID, 
        DOMAIN_ALIAS_RID_ADMINS, 
        0, 0, 0, 0, 0, 0, 
        &pAdministratorsGroup))
    {
        dwError = GetLastError();
        goto Cleanup;
    }

    // Determine whether the SID of administrators group is enabled in 
    // the primary access token of the process.
    if (!CheckTokenMembership(NULL, pAdministratorsGroup, &fIsRunAsAdmin))
    {
        dwError = GetLastError();
        goto Cleanup;
    }

Cleanup:
    // Centralized cleanup for all allocated resources.
    if (pAdministratorsGroup)
    {
        FreeSid(pAdministratorsGroup);
        pAdministratorsGroup = NULL;
    }

    // Throw the error if something failed in the function.
    if (ERROR_SUCCESS != dwError)
        return FALSE;

    return fIsRunAsAdmin;
}

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

BOOL MyCopyFile (_TCHAR *src, _TCHAR *dest)
{
    int err = 0;
    HANDLE hSrc = INVALID_HANDLE_VALUE, hDest = INVALID_HANDLE_VALUE;

    hSrc = CreateFile (src, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hSrc == INVALID_HANDLE_VALUE)
    {
        err = GetLastError();
        goto failure;
    }

    hDest = CreateFile (dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hDest == INVALID_HANDLE_VALUE)
    {
        err = GetLastError();
        goto failure;
    }

    BYTE buff[65536];
    DWORD read, wrote;

    for (;;)
    {
        if (!ReadFile (hSrc, buff, sizeof(buff), &read, NULL))
        {
            err = GetLastError();
            goto failure;
        }

        if (read == 0)
            break;

        if (!WriteFile (hDest, buff, read, &wrote, NULL))
        {
            err = GetLastError();
            goto failure;
        }

        if (wrote != read)
            goto failure;
    }

    CloseHandle (hSrc);
    CloseHandle (hDest);

    if (err)
        SetLastError (err);

    return TRUE;

failure:
    if (hSrc != INVALID_HANDLE_VALUE)
        CloseHandle (hSrc);
    
    if (hDest != INVALID_HANDLE_VALUE)
        CloseHandle (hDest);

    if (err)
        SetLastError (err);

    return FALSE;
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
                MyCopyFile (updates->previousFile, updates->outputPath);
                DeleteFile (updates->previousFile);
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
        if (updates->sourceURL)
            free (updates->sourceURL);
        if (updates->basename)
            free (updates->basename);
        if (updates->packageName)
            free (updates->packageName);

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
        if (!isalnum(*p) && *p != '.' && *p != '/' && *p != '_' && *p != '-')
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

DWORD WINAPI DownloadWorkerThread (VOID *arg)
{
    BOOL foundWork;
    update_t *updates = (update_t *)arg;
    
    HINTERNET hSession = WinHttpOpen(_T("OBS Updater/2.1"), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
    {
        downloadThreadFailure = TRUE;
        Status(_T("Update failed: Couldn't open obsproject.com"));
        return 1;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, _T("obsproject.com"), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect)
    {
        downloadThreadFailure = TRUE;
        Status (_T("Update failed: Couldn't connect to obsproject.com"));
        return 1;
    }

    for (;;)
    {
        foundWork = FALSE;

        EnterCriticalSection (&updateMutex);

        while (updates->next)
        {
            int responseCode;

            if (WaitForSingleObject(cancelRequested, 0) == WAIT_OBJECT_0)
            {
                LeaveCriticalSection (&updateMutex);
                return 1;
            }

            updates = updates->next;

            if (updates->state != STATE_PENDING_DOWNLOAD)
                continue;

            updates->state = STATE_DOWNLOADING;

            LeaveCriticalSection (&updateMutex);

            foundWork = TRUE;

            if (downloadThreadFailure)
                return 1;

            Status (_T("Downloading %s"), updates->outputPath);

            if (!HTTPGetFile (hSession, hConnect, updates->sourceURL, updates->tempPath, _T("Accept-Encoding: gzip"), &responseCode))
            {
                downloadThreadFailure = TRUE;
                DeleteFile (updates->tempPath);
                Status (_T("Update failed: Could not download %s (error code %d)"), updates->outputPath, responseCode);
                return 1;
            }

            if (responseCode != 200)
            {
                downloadThreadFailure = TRUE;
                DeleteFile (updates->tempPath);
                Status (_T("Update failed: Could not download %s (error code %d)"), updates->outputPath, responseCode);
                return 1;
            }

            BYTE downloadHash[20];
            if (!CalculateFileHash(updates->tempPath, downloadHash))
            {
                downloadThreadFailure = TRUE;
                DeleteFile (updates->tempPath);
                Status (_T("Update failed: Couldn't verify integrity of %s"), updates->outputPath);
                return 1;
            }

            if (memcmp(updates->downloadhash, downloadHash, 20))
            {
                downloadThreadFailure = TRUE;
                DeleteFile (updates->tempPath);
                Status (_T("Update failed: Integrity check failed on %s"), updates->outputPath);
                return 1;
            }

            EnterCriticalSection (&updateMutex);

            updates->state = STATE_DOWNLOADED;
            completedUpdates++;

            LeaveCriticalSection (&updateMutex);
        }

        if (!foundWork)
        {
            LeaveCriticalSection (&updateMutex);
            break;
        }

        if (downloadThreadFailure)
            return 1;
    }

    if (hSession)
        WinHttpCloseHandle(hSession);

    if (hConnect)
        WinHttpCloseHandle(hConnect);

    return 0;
}

BOOL RunDownloadWorkers (int num, update_t *updates)
{
    DWORD threadID;
    HANDLE *handles;

    InitializeCriticalSection (&updateMutex);

    handles = (HANDLE *)malloc (sizeof(*handles) * num);
    if (!handles)
        return FALSE;

    for (int i = 0; i < num; i++)
    {
        handles[i] = CreateThread (NULL, 0, DownloadWorkerThread, updates, 0, &threadID);
        if (!handles[i])
            return FALSE;
    }

    WaitForMultipleObjects (num, handles, TRUE, INFINITE);

    for (int i = 0; i < num; i++)
    {
        DWORD exitCode;
        GetExitCodeThread (handles[i], &exitCode);
        if (exitCode != 0)
            return FALSE;
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
        HANDLE hWait[2];
        hWait[0] = hObsMutex;
        hWait[1] = cancelRequested;

        int i = WaitForMultipleObjects(2, hWait, FALSE, INFINITE);

        if (i == WAIT_OBJECT_0)
            ReleaseMutex(hObsMutex);

        CloseHandle(hObsMutex);

        if (i == WAIT_OBJECT_0 + 1)
            goto failure;
    }

    if (!CryptAcquireContext(&hProvider, NULL, MS_ENH_RSA_AES_PROV, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
    {
        SetDlgItemText(hwndMain, IDC_STATUS, TEXT("Update failed: CryptAcquireContext failure"));
        goto failure;
    }

    SetDlgItemText(hwndMain, IDC_STATUS, TEXT("Searching for available updates..."));

    BOOL bIsPortable = FALSE;

    _TCHAR *cmdLine = (_TCHAR *)arg;
    if (!cmdLine[0])
    {
        Status(_T("Update failed: Missing command line parameters."));
        goto failure;
    }

	_TCHAR *p = _tcschr(cmdLine, ' ');
	if (p)
	{
		*p = '\0';
		p++;

		if (!_tcscmp(p, _T("Portable")))
			bIsPortable = TRUE;
	}

    const _TCHAR *targetPlatform = cmdLine;

    TCHAR manifestPath[MAX_PATH];
    TCHAR tempPath[MAX_PATH];
    TCHAR lpAppDataPath[MAX_PATH];

	manifestPath[0] = 0;
	tempPath[0] = 0;
	lpAppDataPath[0] = 0;

    if (bIsPortable)
    {
        GetCurrentDirectory(_countof(lpAppDataPath), lpAppDataPath);
    }
    else
    {
		PWSTR pOut;
		if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, NULL, &pOut) != S_OK)
		{
			Status(TEXT("Update failed: Could not determine AppData location"));
			goto failure;
		}

		StringCbCopy(lpAppDataPath, sizeof(lpAppDataPath), pOut);
        StringCbCat(lpAppDataPath, sizeof(lpAppDataPath), TEXT("\\OBS"));
		CoTaskMemFree (pOut);
    }

    StringCbPrintf(manifestPath, sizeof(manifestPath), TEXT("%s\\updates\\packages.xconfig"), lpAppDataPath);
    StringCbPrintf(tempPath, sizeof(tempPath), TEXT("%s\\updates\\temp"), lpAppDataPath);

    CreateDirectory(tempPath, NULL);

    HANDLE hManifest = CreateFile(manifestPath, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hManifest == INVALID_HANDLE_VALUE)
    {
        Status(TEXT("Update failed: Could not open update manifest"));
        goto failure;
    }

    LARGE_INTEGER manifestfileSize;

    if (!GetFileSizeEx(hManifest, &manifestfileSize))
    {
        Status(TEXT("Update failed: Could not check size of update manifest"));
        return 1;
    }

    CHAR *buff = (CHAR *)malloc((size_t)manifestfileSize.QuadPart + 1);
    if (!buff)
    {
        Status(TEXT("Update failed: Could not allocate memory for update manifest"));
        goto failure;
    }

    DWORD read;

    if (!ReadFile(hManifest, buff, (DWORD)manifestfileSize.QuadPart, &read, NULL))
    {
        CloseHandle(hManifest);
        Status(TEXT("Update failed: Error reading update manifest"));
        goto failure;
    }

    CloseHandle(hManifest);

    if (read != manifestfileSize.QuadPart)
    {
        Status(_T("Update failed: Failed to read update manifest"));
        goto failure;
    }

    buff[read] = 0;

    json_t *root;
    json_error_t error;

    root = json_loads(buff, 0, &error);

    free (buff);

    if (!root)
    {
        Status (_T("Update failed: Couldn't parse update manifest: %S"), error.text);
        goto failure;
    }

    if (!json_is_object(root))
    {
        Status(_T("Update failed: Invalid update manifest"));
        goto failure;
    }

    //----------------------
    //Parse update manifest
    //----------------------
    const char *packageName;
    json_t *package;

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

            if (!json_is_string(source))
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
            _TCHAR fileHashStr[41];
            int has_hash;

            //We don't really care if this fails, it's just to avoid wasting bandwidth by downloading unmodified files
            if (CalculateFileHash(fullPath, existingHash))
            {
                HashToString(existingHash, fileHashStr);

                if (!_tcscmp(fileHashStr, updateHashStr))
                    continue;

                has_hash = 1;
            }
            else
            {
                has_hash = 0;
            }

            updates->next = (update_t *)malloc(sizeof(*updates));
            updates = updates->next;

            updates->next = NULL;
            updates->fileSize = fileSize;
            updates->previousFile = NULL;
            updates->basename = _tcsdup(updateFileName);
            updates->outputPath = _tcsdup(fullPath);
            updates->tempPath = _tcsdup(tempFilePath);
            updates->sourceURL = _tcsdup(sourceURL);
            updates->packageName = _strdup(packageName);
            updates->state = STATE_PENDING_DOWNLOAD;
            updates->patchable = 0;
			StringToHash(updateHashStr, updates->downloadhash);
			memcpy(updates->hash, updates->downloadhash, sizeof(updates->hash));
            
            updates->has_hash = has_hash;
            if (has_hash)
                StringToHash(fileHashStr, updates->my_hash);

            totalUpdates++;
            totalFileSize += fileSize;
        }
    }

    json_decref(root);

    if (totalUpdates)
    {
        json_t *req, *files, *packageFiles;
        _TCHAR hash_string[41];

        char *lastPackage = "";
        
        req = json_object();
        files = json_object();

        json_object_set(req, "packages", files);

        //----------------
        //Compute hashes
        //----------------
        updates = &updateList;
        while (updates->next)
        {
            char whash_string[41];
            char woutputPath[MAX_PATH];

            updates = updates->next;

            if (strcmp(lastPackage, updates->packageName))
            {
                packageFiles = json_object();
                json_object_set(files, updates->packageName, packageFiles);
                lastPackage = updates->packageName;
            }

            if (!updates->has_hash)
                continue;

            HashToString(updates->my_hash, hash_string);
            if (!_tcscmp(hash_string, _T("0000000000000000000000000000000000000000")))
                continue;

            WideCharToMultiByte(CP_UTF8, 0, hash_string, -1, whash_string, sizeof(whash_string), NULL, NULL);
            WideCharToMultiByte(CP_UTF8, 0, updates->basename, -1, woutputPath, sizeof(whash_string), NULL, NULL);

            //info = json_pack("{s:s,s:s}", "path", woutputPath, "hash", whash_string);
            json_t *value = json_string(whash_string);
            json_object_set(packageFiles, woutputPath, value);

            //json_array_append(packageFiles, info);
        }

        //-----------------
        //Send file hashes
        //-----------------

        char *post_body = json_dumps(req, JSON_COMPACT);

        BYTE *newManifest;
        int newManifestLength;
        int responseCode;

        if (!HTTPPostData(_T("https://obsproject.com/update/getpatchmanifest"), (BYTE *)post_body, strlen(post_body), _T("Accept-Encoding: gzip"), &responseCode, &newManifest, &newManifestLength))
            goto failure;

        if (responseCode != 200)
        {
            Status(_T("Update failed: HTTP/%d while trying to download patch manifest"), responseCode);
            goto failure;
        }

        //---------------------
        //Parse new manifest
        //---------------------
        root = json_loads((const char *)newManifest, 0, &error);

        //free(newManifest);

        if (!root)
        {
            Status(_T("Update failed: Couldn't parse patch manifest: %S"), error.text);
            goto failure;
        }

        if (!json_is_object(root))
        {
            Status(_T("Update failed: Invalid patch manifest"));
            goto failure;
        }

        const char *patchpackageName;
        json_t *patchpackage;

        json_object_foreach(root, patchpackageName, patchpackage)
        {
            if (!json_is_object(patchpackage))
            {
                Status(_T("Update failed: Invalid patch manifest"));
                goto failure;
            }

            json_t *value;
            const char *patchableFilename;

            _TCHAR widePatchableFilename[MAX_PATH];
            _TCHAR widePatchHash[MAX_PATH];
            _TCHAR patchHashStr[41];

            json_object_foreach(patchpackage, patchableFilename, value)
            {
                if (!json_is_object(value))
                {
                    Status(_T("Update failed: Invalid patch manifest"));
                    goto failure;
                }

                json_t *hash = json_object_get(value, "hash");
                if (!json_is_string(hash))
                    continue;

                const char *patchHash = json_string_value(hash);

                json_t *source = json_object_get(value, "source");
                if (!json_is_string(source))
                    continue;

                const char *sourceStr = json_string_value(source);
                if (strncmp (sourceStr, "https://obsproject.com/", 23))
                    continue;

                json_t *size = json_object_get(value, "size");
                if (!json_is_integer(size))
                    continue;

                int patchSize = (int)json_integer_value(size);

                MultiByteToWideChar(CP_UTF8, 0, patchableFilename, -1, widePatchableFilename, sizeof(widePatchableFilename));
                MultiByteToWideChar(CP_UTF8, 0, patchHash, -1, widePatchHash, sizeof(widePatchHash));

                updates = &updateList;
                while (updates->next)
                {
                    updates = updates->next;
                    if (strcmp(updates->packageName, patchpackageName))
                        continue;
                    if (_tcscmp(updates->basename, widePatchableFilename))
                        continue;

                    // Replace the source URL with the patch file and mark it as patchable
                    updates->patchable = true;

                    free(updates->sourceURL);

                    _TCHAR sourceURL[1024];
                    if (!MultiByteToWideChar(CP_UTF8, 0, sourceStr, -1, sourceURL, _countof(sourceURL)))
                        continue;

                    if (!MultiByteToWideChar(CP_UTF8, 0, patchHash, -1, patchHashStr, _countof(patchHashStr)))
                        continue;

                    StringToHash(patchHashStr, updates->downloadhash);

                    // Re-calculate download size
                    totalFileSize -= (updates->fileSize - patchSize);
                    updates->sourceURL = _tcsdup(sourceURL);
                    updates->fileSize = patchSize;

                    break;
                }

            }
        }

        //-------------------
        //Download Updates
        //-------------------
        updates = &updateList;
        if (!RunDownloadWorkers (2, updates))
            goto failure;

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

                if (updates->patchable)
                    Status (_T("Updating %s..."), updates->outputPath);
                else
                    Status (_T("Installing %s..."), updates->outputPath);

                //Check if we're replacing an existing file or just installing a new one
                if (GetFileAttributes(updates->outputPath) != INVALID_FILE_ATTRIBUTES)
                {
					_TCHAR *curFileName = NULL;
					_TCHAR baseName[MAX_PATH];

					StringCbCopy(baseName, sizeof(baseName), updates->outputPath);
					curFileName = _tcsrchr(baseName, '/');
					if (curFileName)
					{
						curFileName[0] = '\0';
						curFileName++;
					}
					else
						curFileName = baseName;
					
					//Backup the existing file in case a rollback is needed
                    StringCbCopy(oldFileRenamedPath, sizeof(oldFileRenamedPath), updates->outputPath);
                    StringCbCat(oldFileRenamedPath, sizeof(oldFileRenamedPath), _T(".old"));

                    if (!MyCopyFile(updates->outputPath, oldFileRenamedPath))
                    {
                        int is_sharing_violation = (GetLastError() == ERROR_SHARING_VIOLATION);

                        if (is_sharing_violation)
							Status(_T("Update failed: %s is still in use. Close all programs and try again."), curFileName);
                        else
							Status(_T("Update failed: Couldn't backup %s (error %d)"), curFileName, GetLastError());
                        goto failure;
                    }

                    int error_code;
                    BOOL installed_ok;

                    if (updates->patchable)
                    {
                        error_code = ApplyPatch(updates->tempPath, updates->outputPath);
                        installed_ok = (error_code == 0);

						if (installed_ok)
						{
							BYTE patchedFileHash[20];
							if (!CalculateFileHash(updates->outputPath, patchedFileHash))
							{
								Status(_T("Update failed: Couldn't verify integrity of patched %s"), curFileName);
								goto failure;
							}

							if (memcmp(updates->hash, patchedFileHash, 20))
							{
								Status(_T("Update failed: Integrity check of patched %s failed"), curFileName);
								goto failure;
							}
						}
                    }
                    else
                    {
                        installed_ok = MyCopyFile(updates->tempPath, updates->outputPath);
                        error_code = GetLastError();
                    }

                    if (!installed_ok)
                    {
                        int is_sharing_violation = (error_code == ERROR_SHARING_VIOLATION);

                        if (is_sharing_violation)
							Status(_T("Update failed: %s is still in use. Close all programs and try again."), curFileName);
                        else
							Status(_T("Update failed: Couldn't update %s (error %d)"), curFileName, GetLastError());
                        goto failure;
                    }

                    updates->previousFile = _tcsdup(oldFileRenamedPath);
                    updates->state = STATE_INSTALLED;
                }
                else
                {
                    if (updates->patchable)
                    {
                        //Uh oh, we thought we could patch something but it's no longer there!
                        Status(_T("Update failed: Source file %s not found"), updates->outputPath);
                        goto failure;
                    }

                    //We may be installing into new folders, make sure they exist
                    CreateFoldersForPath (updates->outputPath);

                    if (!MyCopyFile(updates->tempPath, updates->outputPath))
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

                //We delete here not above in case of duplicate hashes
                if (updates->tempPath)
                    DeleteFile(updates->tempPath);
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
		
		if (tempPath[0])
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
		if (tempPath[0])
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

    ShellExecuteEx (&execInfo);
}

INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch(message)
    {
        case WM_INITDIALOG:
            {
                static HICON hMainIcon = LoadIcon (hinstMain, MAKEINTRESOURCE(IDI_ICON1));
                SendMessage (hwnd, WM_SETICON, ICON_BIG, (LPARAM)hMainIcon);
                SendMessage (hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hMainIcon);
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
                            PostQuitMessage(1);
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

    if (!IsAppRunningAsAdminMode())
    {
        HANDLE hLowMutex = CreateMutex (NULL, TRUE, _T("OBSUpdaterRunningAsNonAdminUser"));
        _TCHAR myPath[MAX_PATH];
        if (GetModuleFileName (NULL, myPath, _countof(myPath)-1))
        {
            _TCHAR cwd[MAX_PATH];
            GetCurrentDirectory(_countof(cwd)-1, cwd);

            SHELLEXECUTEINFO shExInfo = {0};
            shExInfo.cbSize = sizeof(shExInfo);
            shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
            shExInfo.hwnd = 0;
            shExInfo.lpVerb = _T("runas");                // Operation to perform
            shExInfo.lpFile = myPath;       // Application to start    
            shExInfo.lpParameters = lpCmdLine;                  // Additional parameters
            shExInfo.lpDirectory = cwd;
            shExInfo.nShow = SW_NORMAL;
            shExInfo.hInstApp = 0;

            //annoyingly the actual elevated updater will disappear behind other windows :(
            AllowSetForegroundWindow (ASFW_ANY);

            if (ShellExecuteEx(&shExInfo))
            {
                DWORD exitCode;

                if (GetExitCodeProcess (shExInfo.hProcess, &exitCode))
                {
                    if (exitCode == 1)
                        LaunchOBS ();
                }
                CloseHandle (shExInfo.hProcess);
            }
        }

        if (hLowMutex)
        {
            ReleaseMutex (hLowMutex);
            CloseHandle (hLowMutex);
        }

        return 0;
    }
    else
    {
        hinstMain = hInstance;

        icce.dwSize = sizeof(icce);
        icce.dwICC = ICC_PROGRESS_CLASS;

        InitCommonControlsEx(&icce);

        hwndMain = CreateDialog(hInstance, MAKEINTRESOURCE(IDD_UPDATEDIALOG), NULL, DialogProc);

        if (!hwndMain)
            return -1;

        ShowWindow (hwndMain, SW_SHOWNORMAL);
        SetForegroundWindow (hwndMain);

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

        //there is no non-elevated process waiting for us if UAC is disabled
        HANDLE hMutex = OpenMutex (SYNCHRONIZE, FALSE, _T("OBSUpdaterRunningAsNonAdminUser"));

        if (msg.wParam == 1 && !hMutex)
            LaunchOBS ();

        return msg.wParam;
    }
}