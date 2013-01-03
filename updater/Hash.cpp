#include "Updater.h"

VOID HashToString (BYTE *in, TCHAR *out)
{
    const char alphabet[] = "0123456789abcdef";

    for (int i = 0; i != 20; i++)
    {
        out[2*i]     = alphabet[in[i] / 16];
        out[2*i + 1] = alphabet[in[i] % 16];
    }

    out[40] = 0;
}

VOID StringToHash (TCHAR *in, BYTE *out)
{
    int temp;

    for (int i = 0; i < 20; i++)
    {
        _stscanf_s (in + i * 2, _T("%02x"), &temp);
        out[i] = temp;
    }
}

BOOL CalculateFileHash (TCHAR *path, BYTE *hash)
{
    BYTE buff[65536];
    HCRYPTHASH hHash;

    if (!CryptCreateHash(hProvider, CALG_SHA1, 0, 0, &hHash))
        return FALSE;

    HANDLE hFile;

    hFile = CreateFile(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            //A missing file is OK
            memset (hash, 0, 20);
            return TRUE;
        }

        return FALSE;
    }

    for (;;)
    {
        DWORD read;

        if (!ReadFile(hFile, buff, sizeof(buff), &read, NULL))
        {
            CloseHandle (hFile);
            return FALSE;
        }

        if (!read)
            break;

        if (!CryptHashData(hHash, buff, read, 0))
        {
            CryptDestroyHash(hHash);
            CloseHandle (hFile);
            return FALSE;
        }
    }

    CloseHandle (hFile);

    DWORD hashLength = 20;
    if (!CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLength, 0))
        return FALSE;

    CryptDestroyHash(hHash);
    return TRUE;
}