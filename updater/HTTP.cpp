#include "Updater.h"

BOOL HTTPPostData(const _TCHAR *url, const BYTE *data, int dataLen, const _TCHAR *extraHeaders, int *responseCode, BYTE **response, int *responseLen)
{
    HINTERNET hSession = NULL;
    HINTERNET hConnect = NULL;
    HINTERNET hRequest = NULL;
    URL_COMPONENTS  urlComponents;
    BOOL secure = FALSE;
    BOOL ret = FALSE;
    BYTE *responseBuffer = NULL;
    BYTE *outputBuffer = NULL;

    _TCHAR hostName[256];
    _TCHAR path[1024];

    const TCHAR *acceptTypes[] = {
        TEXT("*/*"),
        NULL
    };

    ZeroMemory(&urlComponents, sizeof(urlComponents));

    urlComponents.dwStructSize = sizeof(urlComponents);

    urlComponents.lpszHostName = hostName;
    urlComponents.dwHostNameLength = _countof(hostName);

    urlComponents.lpszUrlPath = path;
    urlComponents.dwUrlPathLength = _countof(path);

    WinHttpCrackUrl(url, 0, 0, &urlComponents);

    if (urlComponents.nPort == 443)
        secure = TRUE;

    hSession = WinHttpOpen(_T("OBS Updater/2.1"), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
    {
        *responseCode = -1;
        goto failure;
    }

    hConnect = WinHttpConnect(hSession, hostName, secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect)
    {
        *responseCode = -2;
        goto failure;
    }

    hRequest = WinHttpOpenRequest(hConnect, TEXT("POST"), path, NULL, WINHTTP_NO_REFERER, acceptTypes, secure ? WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH : WINHTTP_FLAG_REFRESH);
    if (!hRequest)
    {
        *responseCode = -3;
        goto failure;
    }

    BOOL bResults = WinHttpSendRequest(hRequest, extraHeaders, extraHeaders ? -1 : 0, (LPVOID)data, dataLen, dataLen, 0);

    // End the request.
    if (bResults)
    {
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }
    else
    {
        *responseCode = GetLastError();
        goto failure;
    }

    TCHAR encoding[64];
    DWORD encodingLen;

    TCHAR statusCode[8];
    DWORD statusCodeLen;

    statusCodeLen = sizeof(statusCode);
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeLen, WINHTTP_NO_HEADER_INDEX))
    {
        *responseCode = -4;
        goto failure;
    }
	else
	{
		statusCode[_countof(statusCode)-1] = 0;
	}

    encodingLen = sizeof(encoding);
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_ENCODING, WINHTTP_HEADER_NAME_BY_INDEX, encoding, &encodingLen, WINHTTP_NO_HEADER_INDEX))
    {
        encoding[0] = 0;
        if (GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND)
        {
            *responseCode = -5;
            goto failure;
        }
    }
	else
	{
		encoding[_countof(encoding)-1] = 0;
	}

    DWORD responseBufferSize = 262144;

    responseBuffer = (BYTE *)malloc(responseBufferSize);

    BOOL gzip = FALSE;

    z_stream strm;

    if (!_tcscmp(encoding, _T("gzip")))
    {
        gzip = TRUE;

        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.avail_in = 0;
        strm.next_in = Z_NULL;

        ret = inflateInit2(&strm, 16 + MAX_WBITS);

        if (ret != Z_OK)
            goto failure;

        outputBuffer = (BYTE *)malloc(262144);
        if (!outputBuffer)
        {
            *responseCode = -6;
            goto failure;
        }
    }

    *responseCode = wcstoul(statusCode, NULL, 10);

    if (bResults && *responseCode == 200)
    {
        DWORD position = 0;
        BYTE buffer[32768];
        DWORD dwSize, dwOutSize;

        do
        {
            // Check for available data.
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize))
            {
                *responseCode = -8;
                goto failure;
            }

            dwSize = min(dwSize, sizeof(buffer));

            if (!WinHttpReadData(hRequest, (LPVOID)buffer, dwSize, &dwOutSize))
            {
                *responseCode = -9;
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

                        int zret = inflate(&strm, Z_NO_FLUSH);
                        if (zret != Z_STREAM_END && zret != Z_OK)
                        {
                            inflateEnd(&strm);
                            goto failure;
                        }

                        if (position + dwOutSize + 1 >= responseBufferSize)
                        {
                            responseBufferSize *= 2;
                            responseBuffer = (BYTE *)realloc(responseBuffer, responseBufferSize);
                            if (!responseBuffer)
                            {
                                *responseCode = -6;
                                goto failure;
                            }
                        }

                        memcpy(responseBuffer + position, outputBuffer, 262144 - strm.avail_out);
                        position += 262144 - strm.avail_out;
                    } while (strm.avail_out == 0);
                }
                else
                {
                    if (position + dwOutSize + 1 >= responseBufferSize)
                    {
                        responseBufferSize *= 2;
                        responseBuffer = (BYTE *)realloc(responseBuffer, responseBufferSize);
                        if (!responseBuffer)
                        {
                            *responseCode = -6;
                            goto failure;
                        }
                    }

                    memcpy(responseBuffer + position, buffer, dwOutSize);
                    position += dwOutSize;
                }
            }

            if (WaitForSingleObject(cancelRequested, 0) == WAIT_OBJECT_0)
            {
                *responseCode = -14;
                goto failure;
            }

        } while (dwSize > 0);
        
        responseBuffer[position] = 0;

        *response = responseBuffer;
        *responseLen = position;
    }

    ret = TRUE;

failure:
    if (responseBuffer && !ret)
        free(responseBuffer);
    if (outputBuffer)
        free(outputBuffer);
    if (hSession)
        WinHttpCloseHandle(hSession);
    if (hConnect)
        WinHttpCloseHandle(hConnect);
    if (hRequest)
        WinHttpCloseHandle(hRequest);

    return ret;
}

BOOL HTTPGetFile (HINTERNET hSession, HINTERNET hConnect, const _TCHAR *url, const _TCHAR *outputPath, const _TCHAR *extraHeaders, int *responseCode)
{
    HINTERNET hRequest = NULL;
    BOOL ret = FALSE;

    BYTE *outputBuffer = NULL;

    const TCHAR *acceptTypes[] = {
        TEXT("*/*"),
        NULL
    };

    URL_COMPONENTS  urlComponents;
    BOOL secure = FALSE;

    _TCHAR hostName[256];
    _TCHAR path[1024];

    ZeroMemory (&urlComponents, sizeof(urlComponents));

    urlComponents.dwStructSize = sizeof(urlComponents);
    
    urlComponents.lpszHostName = hostName;
    urlComponents.dwHostNameLength = _countof(hostName);

    urlComponents.lpszUrlPath = path;
    urlComponents.dwUrlPathLength = _countof(path);

    WinHttpCrackUrl(url, 0, 0, &urlComponents);

    if (urlComponents.nPort == 443)
        secure = TRUE;

    hRequest = WinHttpOpenRequest(hConnect, TEXT("GET"), path, NULL, WINHTTP_NO_REFERER, acceptTypes, secure ? WINHTTP_FLAG_SECURE|WINHTTP_FLAG_REFRESH : WINHTTP_FLAG_REFRESH);
    if (!hRequest)
    {
        *responseCode = -3;
        goto failure;
    }

    BOOL bResults = WinHttpSendRequest(hRequest, extraHeaders, extraHeaders ? -1 : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

    // End the request.
    if (bResults)
    {
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }
    else
    {
        *responseCode = GetLastError ();
        goto failure;
    }

    TCHAR encoding[64];
    DWORD encodingLen;

    TCHAR statusCode[8];
    DWORD statusCodeLen;

    statusCodeLen = sizeof(statusCode);
    if (!WinHttpQueryHeaders (hRequest, WINHTTP_QUERY_STATUS_CODE, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeLen, WINHTTP_NO_HEADER_INDEX))
    {
        *responseCode = -4;
        goto failure;
    }
	else
	{
		statusCode[_countof(statusCode) - 1] = 0;
	}

    encodingLen = sizeof(encoding);
    if (!WinHttpQueryHeaders (hRequest, WINHTTP_QUERY_CONTENT_ENCODING, WINHTTP_HEADER_NAME_BY_INDEX, encoding, &encodingLen, WINHTTP_NO_HEADER_INDEX))
    {
        encoding[0] = 0;
        if (GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND)
        {
            *responseCode = -5;
            goto failure;
        }
    }
	else
	{
		encoding[_countof(encoding) - 1] = 0;
	}

    BOOL gzip = FALSE;

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
        {
            *responseCode = -6;
            goto failure;
        }
    }

    *responseCode = wcstoul(statusCode, NULL, 10);

    if (bResults && *responseCode == 200)
    {
        BYTE buffer[32768];
        DWORD dwSize, dwOutSize, wrote;

        HANDLE updateFile;
        int lastPosition = 0;

        updateFile = CreateFile(outputPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (updateFile == INVALID_HANDLE_VALUE)
        {
            *responseCode = -7;
            goto failure;
        }

        do 
        {
            // Check for available data.
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize))
            {
                *responseCode = -8;
                goto failure;
            }

            dwSize = min(dwSize, sizeof(buffer));

            if (!WinHttpReadData(hRequest, (LPVOID)buffer, dwSize, &dwOutSize))
            {
                *responseCode = -9;
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

                        int zret = inflate(&strm, Z_NO_FLUSH);
                        if (zret != Z_STREAM_END && zret != Z_OK)
                        {
                            inflateEnd(&strm);
                            CloseHandle (updateFile);
                            goto failure;
                        }

                        if (!WriteFile(updateFile, outputBuffer, 262144 - strm.avail_out, &wrote, NULL))
                        {
                            *responseCode = -10;
                            CloseHandle (updateFile);
                            goto failure;
                        }
                        if (wrote != 262144 - strm.avail_out)
                        {
                            *responseCode = -11;
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
                        *responseCode = -12;
                        CloseHandle (updateFile);
                        goto failure;
                    }

                    if (wrote != dwOutSize)
                    {
                        *responseCode = -13;
                        CloseHandle (updateFile);
                        goto failure;
                    }

                    completedFileSize += dwOutSize;
                }

                int position = (int)(((float)completedFileSize / (float)totalFileSize) * 100.0f);
                if (position > lastPosition)
                {
                    lastPosition = position;
                    SendDlgItemMessage (hwndMain, IDC_PROGRESS, PBM_SETPOS, position, 0);
                }
            }

            if (WaitForSingleObject(cancelRequested, 0) == WAIT_OBJECT_0)
            {
                *responseCode = -14;
                CloseHandle (updateFile);
                goto failure;
            }

        } while (dwSize > 0);

        CloseHandle (updateFile);
    }

    ret = TRUE;

failure:
    if (outputBuffer)
        free(outputBuffer);
    if (hRequest)
        WinHttpCloseHandle(hRequest);

    return ret;
}
