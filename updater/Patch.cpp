#include "Updater.h"

#include <bzlib.h>
#include "stdint.h"

struct bspatch_stream
{
	void* opaque;
	int (*read)(const struct bspatch_stream* stream, void* buffer, int length);
};

int bspatch(const uint8_t* old, int64_t oldsize, uint8_t* newp, int64_t newsize, struct bspatch_stream* stream);

static int64_t offtin(uint8_t *buf)
{
	int64_t y;

	y=buf[7]&0x7F;
	y=y*256;y+=buf[6];
	y=y*256;y+=buf[5];
	y=y*256;y+=buf[4];
	y=y*256;y+=buf[3];
	y=y*256;y+=buf[2];
	y=y*256;y+=buf[1];
	y=y*256;y+=buf[0];

	if(buf[7]&0x80) y=-y;

	return y;
}

int bspatch(const uint8_t* old, int64_t oldsize, uint8_t* newp, int64_t newsize, struct bspatch_stream* stream)
{
	uint8_t buf[8];
	int64_t oldpos,newpos;
	int64_t ctrl[3];
	int64_t i;

	oldpos=0;newpos=0;
	while(newpos<newsize) {
		/* Read control data */
		for(i=0;i<=2;i++) {
			if (stream->read(stream, buf, 8))
				return -1;
			ctrl[i]=offtin(buf);
		};

		/* Sanity-check */
		if(newpos+ctrl[0]>newsize)
			return -1;

		/* Read diff string */
		if (stream->read(stream, newp + newpos, ctrl[0]))
			return -1;

		/* Add old data to diff string */
		for(i=0;i<ctrl[0];i++)
			if((oldpos+i>=0) && (oldpos+i<oldsize))
				newp[newpos+i]+=old[oldpos+i];

		/* Adjust pointers */
		newpos+=ctrl[0];
		oldpos+=ctrl[0];

		/* Sanity-check */
		if(newpos+ctrl[1]>newsize)
			return -1;

		/* Read extra string */
		if (stream->read(stream, newp + newpos, ctrl[1]))
			return -1;

		/* Adjust pointers */
		newpos+=ctrl[1];
		oldpos+=ctrl[2];
	};

	return 0;
}

static int bz2_read(const struct bspatch_stream* stream, void* buffer, int length)
{
	int n;
	int bz2err;
	BZFILE* bz2;

	bz2 = (BZFILE*)stream->opaque;
	n = BZ2_bzRead(&bz2err, bz2, buffer, length);
    if (n != length || bz2err < 0)
		return -1;

	return 0;
}

BOOL ApplyPatch(LPCTSTR patchFile, LPCTSTR targetFile)
{
    int ret = 1;
	int bz2err;
	uint8_t header[24];
	int64_t newsize;
	BZFILE* bz2;
	struct bspatch_stream stream;

    HANDLE hDest = INVALID_HANDLE_VALUE;
    HANDLE hPatch = INVALID_HANDLE_VALUE;
    HANDLE hTarget = INVALID_HANDLE_VALUE;
    FILE *f = NULL;

    uint8_t *old = NULL, *newData = NULL;

    hPatch = CreateFile (patchFile, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hPatch == INVALID_HANDLE_VALUE)
    {
        ret = GetLastError();
        goto error;
    }

    hTarget = CreateFile (targetFile, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hTarget == INVALID_HANDLE_VALUE)
    {
        ret = GetLastError();
        goto error;
    }

    //read patch header
    DWORD read;
    if (ReadFile (hPatch, header, sizeof(header), &read, NULL) && read == sizeof(header))
    {
        if (memcmp(header, "ENDSLEY/BSDIFF43", 16))
        {
            ret = -4;
            goto error;
        }
    }
    else
    {
        ret = GetLastError();
        goto error;
    }

    //read patch new file size
    newsize = offtin(header+16);

    if (newsize < 0 || newsize >= 0x7ffffffff)
    {
        ret = -5;
        goto error;
    }

    //read current file data
    DWORD targetFileSize;

    targetFileSize = GetFileSize (hTarget, NULL);
    if (targetFileSize == INVALID_FILE_SIZE)
    {
        ret = GetLastError();
        goto error;
    }

    old = (uint8_t *)malloc (targetFileSize + 1);
	if (!old)
	{
		ret = -6;
		goto error;
	}

    if (!(ReadFile (hTarget, old, targetFileSize, &read, NULL) || read != targetFileSize))
    {
        ret = GetLastError();
        goto error;
    }

    CloseHandle (hPatch);
    hPatch = INVALID_HANDLE_VALUE;

    CloseHandle (hTarget);
    hTarget = INVALID_HANDLE_VALUE;

    //prepare new file
    newData = (uint8_t *)malloc (newsize + 1);

    //open patch for bzip2
    _tfopen_s (&f, patchFile, _T("rb"));
    if (!f)
    {
        ret = -8;
        goto error;
    }

    _fseeki64(f, sizeof(header), SEEK_SET);

    if (NULL == (bz2 = BZ2_bzReadOpen(&bz2err, f, 0, 0, NULL, 0)))
    {
        ret = -10;
        goto error;
    }

	stream.read = bz2_read;
	stream.opaque = bz2;

    //actually patch the data
	if (bspatch(old, targetFileSize, newData, newsize, &stream))
    {
        ret = -9;
        goto error;
    }

	BZ2_bzReadClose(&bz2err, bz2);

	fclose(f);
    f = NULL;

    //write new file
    DWORD wrote;

    hDest = CreateFile(targetFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hDest == INVALID_HANDLE_VALUE)
    {
        ret = GetLastError();
        goto error;
    }

    if (!WriteFile(hDest, newData, newsize, &wrote, NULL) || wrote != newsize)
    {
        ret = GetLastError();
        goto error;
    }

    CloseHandle(hDest);

    ret = 0;

error:

    if (old)
        free (old);

    if (newData)
        free (newData);

    if (hTarget != INVALID_HANDLE_VALUE)
        CloseHandle (hTarget);

    if (hPatch != INVALID_HANDLE_VALUE)
        CloseHandle (hPatch);

    if (f)
        fclose (f);

    return ret;
}

