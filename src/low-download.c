/*
 *  Low: a yum-like package manager
 *
 *  Download code adopted from razor:
 *  http://github.com/krh/razor/wikis
 *
 *  Copyright (C) 2008 - 2010 James Bowes <jbowes@repl.ca>
 *  Copyright (C) 2008 Devan Goodwin <dgoodwin@dangerouslyinc.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 *  02110-1301  USA
 */

#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <curl/curl.h>
#include <nss3/nss.h>
#include <nss3/sechash.h>

#include "low-download.h"
#include "low-debug.h"

static char *
create_file_url (const char *baseurl, const char *relative_file)
{
	char *full_url;
	uint size = strlen (baseurl) + strlen (relative_file) + 1;

	if (baseurl[strlen (baseurl) - 1] != '/') {
		size++;
		full_url = malloc (sizeof (char) * size);
		strcpy (full_url, baseurl);
		strcat (full_url, "/");
	} else {
		full_url = malloc (sizeof (char) * size);
		strcpy (full_url, baseurl);
	}

	strcat (full_url, relative_file);

	return full_url;
}

static CURL *
init_curl (char *error, const char *basename, LowDownloadCallback callback)
{
	CURL *curl;

	curl = curl_easy_init ();
	if (curl == NULL) {
		return curl;
	}

	curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt (curl, CURLOPT_ERRORBUFFER, error);
	curl_easy_setopt (curl, CURLOPT_NOPROGRESS, 0);
	curl_easy_setopt (curl, CURLOPT_PROGRESSFUNCTION, callback);
	curl_easy_setopt (curl, CURLOPT_PROGRESSDATA, basename);

	return curl;
}

int
low_download (const char *url, const char *file, const char *basename,
	      LowDownloadCallback callback)
{
	CURL *curl;
	char error[256];
	FILE *fp;
	CURLcode res;
	long response;

	curl = init_curl (error, basename, callback);
	if (curl == NULL) {
		return 1;
	}

	fp = fopen (file, "w");
	if (fp == NULL) {
		fprintf (stderr, "failed to open %s for writing\n", file);
		return -1;
	}
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, fp);
	curl_easy_setopt (curl, CURLOPT_URL, url);
	res = curl_easy_perform (curl);
	fclose (fp);
	if (res != CURLE_OK) {
		fprintf (stderr, "curl error: %s\n", error);
		unlink (file);
		return -1;
	}
	res = curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &response);
	if (res != CURLE_OK) {
		fprintf (stderr, "curl error: %s\n", error);
		unlink (file);
		return -1;
	}
	if (response != 200) {
		if (!(response == 226 && strncmp ("ftp", url, 3) == 0)) {
			fprintf (stderr, " - failed %ld\n", response);
			unlink (file);
			return -1;
		}
	}
	printf ("\n");

	curl_easy_cleanup (curl);

	return 0;
}

int
low_download_from_mirror (LowMirrorList *mirrors, const char *relative_path,
			  const char *file, const char *basename,
			  LowDownloadCallback callback)
{
	CURL *curl;
	char *url;
	const char *baseurl;
	char error[256];
	FILE *fp;
	CURLcode res;
	long response;

	curl = init_curl (error, basename, callback);
	if (curl == NULL) {
		return 1;
	}

	fp = fopen (file, "w");
	if (fp == NULL) {
		fprintf (stderr, "failed to open %s for writing\n", file);
		return -1;
	}

	while (1) {
		fseek (fp, 0, SEEK_SET);

		baseurl = low_mirror_list_lookup_random_mirror (mirrors);
		if (baseurl == NULL) {
			return -1;
		}

		url = create_file_url (baseurl, relative_path);

		curl_easy_setopt (curl, CURLOPT_WRITEDATA, fp);
		curl_easy_setopt (curl, CURLOPT_URL, url);

		res = curl_easy_perform (curl);
		if (res != CURLE_OK) {
			low_debug ("curl error: %s for url %s. marking as bad",
				   error, baseurl);

			low_mirror_list_mark_as_bad (mirrors, baseurl);
			free (url);
			continue;
		}

		res = curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE,
					 &response);
		if (res != CURLE_OK) {
			low_debug ("curl error: %s for url %s. marking as bad",
				   error, baseurl);

			low_mirror_list_mark_as_bad (mirrors, baseurl);
			free (url);
			continue;
		}

		if (response != 200 &&
		    !(response == 226 && strncmp ("ftp", baseurl, 3) == 0)) {
			low_debug ("error: %ld for url %s. marking as bad",
				   response, baseurl);

			low_mirror_list_mark_as_bad (mirrors, baseurl);
			free (url);
			continue;
		}

		free (url);
		break;
	}

	printf ("\n");

	fclose (fp);

	curl_easy_cleanup (curl);

	return 0;
}

#define BUF_SIZE 32

static void
debug_hashes (const char *expected, const unsigned char *calculated, size_t len)
{
	unsigned int i;

	char calculated_pretty[BUF_SIZE * 2 + 1];

	for (i = 0; i < len; i++) {
		sprintf (calculated_pretty + i * 2, "%.2x", calculated[i]);
	}
	calculated_pretty[i * 2 + 1] = '\0';

	low_debug ("digest mismatch:\nexpected:   %s\ncalculated: %s\n",
		   expected, calculated_pretty);
}

static unsigned short
char_to_short (char to_convert)
{
	/* 0 - 9 */
	if (to_convert >= 48 && to_convert <= 57) {
		return to_convert - 48;
	}

	/* A - F */
	if (to_convert >= 65 && to_convert <= 70) {
		return to_convert - 55;
	}

	/* a - f */
	if (to_convert >= 97 && to_convert <= 102) {
		return to_convert - 87;
	}

	return 0;
}

static bool
compare_digest (const char *file, const char *expected,
		LowDigestType digest_type)
{
	unsigned char result[BUF_SIZE];
	unsigned int size;
	unsigned int i;
	int fd;
	HASHContext *ctx;

	/*
	 * XXX rpm initializes and destroys NSS in rpmFreeRc,
	 * and there's no way to tell if NSS initialization is already done
	 * (we need NSS initialized for the hash functions). For now,
	 * low-download has an implicit dep on the rpmdb repo being 'live'
	 * during use.
	 */

	switch (digest_type) {
		case DIGEST_MD5:
			ctx = HASH_Create (HASH_AlgMD5);
			break;
		case DIGEST_SHA1:
			ctx = HASH_Create (HASH_AlgSHA1);
			break;
		case DIGEST_SHA256:
			ctx = HASH_Create (HASH_AlgSHA256);
			break;
		case DIGEST_UNKNOWN:
		case DIGEST_NONE:
		default:
			return false;
	}

	fd = open (file, O_RDONLY);

	HASH_Begin (ctx);

	while (1) {
		unsigned char buf[BUF_SIZE];
		int cnt = read (fd, &buf, BUF_SIZE);

		if (cnt == 0) {
			break;
		}

		HASH_Update (ctx, buf, cnt);
	}

	close (fd);
	HASH_End (ctx, result, &size, BUF_SIZE);
	HASH_Destroy (ctx);

	for (i = 0; i < strlen (expected); i += 2) {
		unsigned char e = 16 * char_to_short (expected[i]) +
			char_to_short (expected[i + 1]);

		if (e != result[i / 2]) {
			debug_hashes (expected, result, size);
			return false;
		}
	}

	return true;
}

bool
low_download_is_missing (const char *file, const char *digest,
			 LowDigestType digest_type, off_t size)
{
	struct stat buf;

	if (stat (file, &buf) < 0 || buf.st_size != size) {
		return true;
	}

	return !compare_digest (file, digest, digest_type);
}

int
low_download_if_missing (LowMirrorList *mirrors, const char *relative_path,
			 const char *file, const char *basename,
			 const char *digest, LowDigestType digest_type,
			 off_t size, LowDownloadCallback callback)
{
	int res;

	if (low_download_is_missing (file, digest, digest_type, size)) {
		res = low_download_from_mirror (mirrors, relative_path, file,
						basename, callback);
		if (res != 0) {
			unlink (file);
			return res;
		}
	}

	if (!compare_digest (file, digest, digest_type)) {
		unlink (file);
		return -1;
	}

	return 0;
}

/* vim: set ts=8 sw=8 noet: */
