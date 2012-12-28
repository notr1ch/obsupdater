#include "Updater.h"

BOOL HTTPGetFile (const _TCHAR *url, const _TCHAR *outputPath, const _TCHAR *extraHeaders, int *responseCode)
{
    HINTERNET hSession = NULL;
    HINTERNET hConnect = NULL;
    HINTERNET hRequest = NULL;
    URL_COMPONENTS  urlComponents;
    BOOL secure = FALSE;
    BOOL ret = FALSE;

    _TCHAR hostName[256];
    _TCHAR path[1024];

    const TCHAR *acceptTypes[] = {
        TEXT("*/*"),
        NULL
    };

    ZeroMemory (&urlComponents, sizeof(urlComponents));

    urlComponents.dwStructSize = sizeof(urlComponents);
    
    urlComponents.lpszHostName = hostName;
    urlComponents.dwHostNameLength = _countof(hostName);

    urlComponents.lpszUrlPath = path;
    urlComponents.dwUrlPathLength = _countof(path);

    WinHttpCrackUrl(url, 0, 0, &urlComponents);

    if (urlComponents.nPort == 443)
        secure = TRUE;

    hSession = WinHttpOpen(_T("OBS Updater/1.0"), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        goto failure;

    hConnect = WinHttpConnect(hSession, hostName, secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect)
        goto failure;

    hRequest = WinHttpOpenRequest(hConnect, TEXT("GET"), path, NULL, WINHTTP_NO_REFERER, acceptTypes, secure ? WINHTTP_FLAG_SECURE|WINHTTP_FLAG_REFRESH : WINHTTP_FLAG_REFRESH);
    if (!hRequest)
        goto failure;

    BOOL bResults = WinHttpSendRequest(hRequest, extraHeaders, extraHeaders ? -1 : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

    // End the request.
    if (bResults)
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    else
        goto failure;

    TCHAR encoding[64];
    DWORD encodingLen;

    TCHAR statusCode[8];
    DWORD statusCodeLen;

    statusCodeLen = sizeof(statusCode);
    if (!WinHttpQueryHeaders (hRequest, WINHTTP_QUERY_STATUS_CODE, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeLen, WINHTTP_NO_HEADER_INDEX))
        goto failure;

    encoding[0] = 0;
    encodingLen = sizeof(encoding);
    if (!WinHttpQueryHeaders (hRequest, WINHTTP_QUERY_CONTENT_ENCODING, WINHTTP_HEADER_NAME_BY_INDEX, encoding, &encodingLen, WINHTTP_NO_HEADER_INDEX))
    {
        if (GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND )
            goto failure;
    }

    BOOL gzip = FALSE;
    BYTE *outputBuffer;

    z_stream strm;

    if (!_tcscmp(encoding, _T("gzip")))
    {
        gzip = TRUE;

        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.avail_in = 0;
        strm.next_in = Z_NULL;
        
        ret = inflateInit2(&strm, 16+MAX_WBITS);

        if (ret != Z_OK)
            goto failure;

        outputBuffer = (BYTE *)malloc(262144);
        if (!outputBuffer)
            goto failure;
    }

    *responseCode = wcstoul(statusCode, NULL, 10);

    if (bResults && *responseCode == 200)
    {
        BYTE buffer[16384];
        DWORD dwSize, dwOutSize, wrote;

        HANDLE updateFile;

        updateFile = CreateFile(outputPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (updateFile == INVALID_HANDLE_VALUE)
            goto failure;

        do 
        {
            // Check for available data.
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize))
                goto failure;

            dwSize = min(dwSize, sizeof(buffer));

            if (!WinHttpReadData(hRequest, (LPVOID)buffer, dwSize, &dwOutSize))
            {
                goto failure;
            }
            else
            {
                if (!dwOutSize)
                    break;

                if (gzip)
                {
                    do
                    {
                        strm.avail_in = dwOutSize;
                        strm.next_in = buffer;

                        strm.avail_out = 262144;
                        strm.next_out = outputBuffer;

                        int ret = inflate(&strm, Z_NO_FLUSH);
                        if (ret != Z_STREAM_END && ret != Z_OK)
                        {
                            inflateEnd(&strm);
                            CloseHandle (updateFile);
                            goto failure;
                        }

                        if (!WriteFile(updateFile, outputBuffer, 262144 - strm.avail_out, &wrote, NULL))
                        {
                            CloseHandle (updateFile);
                            goto failure;
                        }
                        if (wrote != 262144 - strm.avail_out)
                        {
                            CloseHandle (updateFile);
                            goto failure;
                        }

                        completedFileSize += wrote;
                    }
                    while (strm.avail_out == 0);
                }
                else
                {
                    if (!WriteFile(updateFile, buffer, dwOutSize, &wrote, NULL))
                    {
                        CloseHandle (updateFile);
                        goto failure;
                    }

                    if (wrote != dwOutSize)
                    {
                        CloseHandle (updateFile);
                        goto failure;
                    }

                    completedFileSize += dwOutSize;
                }

                int position = (int)(((float)completedFileSize / (float)totalFileSize) * 100.0f);
                SendDlgItemMessage (hwndMain, IDC_PROGRESS, PBM_SETPOS, position, 0);
            }

            if (WaitForSingleObject(cancelRequested, 0) == WAIT_OBJECT_0)
            {
                CloseHandle (updateFile);
                goto failure;
            }

        } while (dwSize > 0);

        CloseHandle (updateFile);
    }

    ret = TRUE;

failure:
    if (hSession)
        WinHttpCloseHandle(hSession);
    if (hConnect)
        WinHttpCloseHandle(hConnect);
    if (hRequest)
        WinHttpCloseHandle(hRequest);

    return ret;
}
