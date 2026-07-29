/*--------------------------------------------------------------------------
| P-1 stage 1 save file transfer
|
| P-1 factoring runs in two stages.  Stage 1 needs little memory; stage 2 wants
| tens of gigabytes and finds most of the factors.  A worker that finishes
| stage 1 and cannot afford stage 2 reports a B1-only result and throws the
| stage 1 residue away, so whoever gets that exponent next redoes stage 1 from
| scratch.
|
| These routines hand that residue to a "ppp" proxy instead, which passes it to
| a machine with enough memory to finish the job.  Both directions are modelled
| on the PRP proof endpoints:
|
|	uploads   mirror proof_upload.c  (chunked multipart POST, resumable)
|	downloads mirror proof_getdata.c (plain GET, streamed to disk)
|
| None of this engages when talking to PrimeNet directly.  The proxy announces
| itself by adding AcceptsP1Uploads=1 to the t=uc reply; with no proxy in the
| path that key is absent, the setting is cleared, and the client behaves
| exactly as it always has.
+--------------------------------------------------------------------------*/

#include "cJSON.h"

/* Staged residues are named p1send.<exponent>.
|
| Deliberately not the mNNNNNNN save file name: the uploader scans the working
| directory, and anything matching a save file pattern risks picking up a file
| belonging to work that is still running.  The plain decimal exponent also
| saves us reversing tempFileName's character mangling to recover it. */

#define P1_STAGING_PREFIX	"p1send."

/* Largest residue we will deal with.  A PM1_STATE_DONE save file is one giant,
| roughly n/8 bytes, so this covers any exponent PrimeNet hands out with room to
| spare.  It exists to reject a nonsensical FileSize from a misbehaving proxy
| rather than to constrain real work. */

#define P1_MAX_FILE_SIZE	((uint64_t) 1000000000)

/* Build the staging file name for an exponent */

void p1StagingFileName (
	unsigned long exponent,
	char	*buf)
{
	sprintf (buf, "%s%lu", P1_STAGING_PREFIX, exponent);
}

/* Recover the exponent from a staging file name, or zero if it is not one */

unsigned long p1ExponentFromStagingName (
	const char *filename)
{
	const char *p;

	if (strncmp (filename, P1_STAGING_PREFIX, strlen (P1_STAGING_PREFIX)) != 0) return (0);
	p = filename + strlen (P1_STAGING_PREFIX);
	if (*p == 0) return (0);
	for (const char *q = p; *q; q++) if (*q < '0' || *q > '9') return (0);
	return (strtoul (p, NULL, 10));
}

/* Move a just-written stage 1 save file to the staging name so the uploader can
| find it.
|
| Called from pminus1 immediately after pm1_save, and only when a proxy has said
| it accepts residues.  Renaming rather than copying keeps this free and atomic;
| the save file has already served its purpose for this work unit, which is
| about to complete. */

void stageP1SaveFileForUpload (
	int	thread_num,
	struct work_unit *w)
{
	char	savefile[32], staging[64], buf[256];

/* Work out both names.  tempFileName plus the 'm' first letter is how pminus1
| names its save file (the same two steps it performs itself). */

	tempFileName (w, savefile);
	savefile[0] = 'm';
	p1StagingFileName (w->n, staging);

/* A residue from an earlier attempt at this exponent is superseded by this one */

	_unlink (staging);

	if (rename (savefile, staging) != 0) {
		sprintf (buf, "Unable to stage P-1 stage 1 save file %s for upload\n", savefile);
		OutputBoth (thread_num, buf);
		_unlink (savefile);
		return;
	}

	sprintf (buf, "Stage 1 residue for M%lu saved for upload to the P-1 proxy.\n", w->n);
	OutputStr (thread_num, buf);

/* Wake the uploader.  It would find the file on its next hourly sweep anyway,
| but the result for this exponent has just been queued and there is no reason
| to make the proxy wait an hour for the residue that goes with it. */

	gwevent_signal (&P1_UPLOAD_EVENT);
}

/* Return the base URL for P-1 transfers, or an empty string if none is configured.
|
| P1UploadIP defaults to MersenneIP because the proxy is the same host serving
| the v5 API -- pointing a client at a proxy should not require configuring it
| twice.  Unlike the hard-coded www.mersenne.org of the PRP proof endpoints, this
| has to be configurable: there is no well-known host for a private proxy. */

void p1BaseURL (
	char	*buf,
	unsigned int bufsize)
{
	char	host[128];

	IniSectionGetString (INI_FILE, SEC_PrimeNet, KEY_P1UploadIP, host, sizeof (host), NULL);
	if (host[0] == 0)
		IniSectionGetString (INI_FILE, SEC_PrimeNet, "MersenneIP", host, sizeof (host), NULL);
	if (host[0] == 0) { buf[0] = 0; return; }

/* Accept a bare host, a host:port, or a full URL, so the setting can be shared
| with MersenneIP (which is always a bare host) without special-casing it. */

	if (strncmp (host, "http://", 7) == 0 || strncmp (host, "https://", 8) == 0)
		snprintf (buf, bufsize, "%s", host);
	else
		snprintf (buf, bufsize, "http://%s", host);

/* Trim a trailing slash so callers can append a rooted path unconditionally */

	{
		size_t len = strlen (buf);
		while (len > 0 && buf[len-1] == '/') buf[--len] = 0;
	}
}

/*--------------------------------------------------------------------------
| Uploading a stage 1 residue
+--------------------------------------------------------------------------*/

/* Assemble the proxy's response to an upload request.  Same shape as the
| equivalent in proof_upload.c: a small JSON body accumulated into a fixed
| buffer. */

struct p1_upload_data {
	char	*buf;
	int	bufsize;
	int	buflen;
};

size_t p1UploadWriteMemoryCallback (void *ptr, size_t size, size_t nmemb, void *data)
{
	struct p1_upload_data *p = (struct p1_upload_data *) data;
	size_t	bytes = size * nmemb;

	if (p->buflen + (int) bytes > p->bufsize) bytes = p->bufsize - p->buflen;
	if (bytes) {
		memcpy (p->buf + p->buflen, ptr, bytes);
		p->buflen += (int) bytes;
		p->buf[p->buflen] = 0;
	}
	return (size * nmemb);
}

/* Upload one staged stage 1 residue.
|
| The wire protocol is the PRP proof upload protocol, because the client side is
| already proven and the proxy implements it exactly:
|
|   1. GET  <base>/p1_upload/?UserID=&Exponent=&FileSize=&FileMD5=
|          -> {"URLToUse": ..., "need": {"<start>": <inclusive end>}}
|   2. POST <URLToUse>&FileMD5=&DataOffset=&DataSize=&DataMD5=
|          multipart/form-data, one part named "Data"
|          -> {"need": {...}} until {"FileUploaded": ...}
|
| On success the staged file is deleted.  On any failure it is left alone and
| retried on the next sweep -- the residue is only useful for as long as the
| proxy still holds the matching result, and the proxy expires it on its own
| schedule, so there is no value in retrying harder than hourly. */

void P1Upload (
	char	*filename)
{
	CURL	*curl;
	CURLcode res;
	FILE	*fd = NULL;
	char	*chunk = NULL;
	char	base_url[512], url[1024], curlbuf[3000], errbuf[CURL_ERROR_SIZE], buf[512];
	char	fileMD5[33], chunkMD5[33];
	uint64_t filesize, chunk_start, chunk_end;
	unsigned long exponent;
	int	max_chunk_size, datasize, debug;
	double	bandwidth_rate_limit_flt, max_chunk_size_flt;
	uint64_t bandwidth_rate_limit;
	struct p1_upload_data info;
	cJSON	*json = NULL, *item;

	debug = IniSectionGetInt (INI_FILE, SEC_PrimeNet, KEY_Debug, 0);

	exponent = p1ExponentFromStagingName (filename);
	if (exponent == 0) return;

/* Chunk sizing follows proof uploads exactly, including the 8MB ceiling the
| server side enforces */

	bandwidth_rate_limit_flt = IniSectionGetFloat (INI_FILE, SEC_PrimeNet, KEY_UploadRateLimit, 1.0);
	if (bandwidth_rate_limit_flt < 0.0) bandwidth_rate_limit_flt = 0.0;
	if (bandwidth_rate_limit_flt > 10000.0) bandwidth_rate_limit_flt = 10000.0;
	if (bandwidth_rate_limit_flt == 0.0 || bandwidth_rate_limit_flt >= 100.0) max_chunk_size_flt = 7.0;
	else if (bandwidth_rate_limit_flt >= 8.0) max_chunk_size_flt = 4.0;
	else if (bandwidth_rate_limit_flt >= 1.0) max_chunk_size_flt = 2.0;
	else max_chunk_size_flt = 1.0;
	max_chunk_size_flt = IniSectionGetFloat (INI_FILE, SEC_PrimeNet, KEY_UploadChunkSize, max_chunk_size_flt);
	if (max_chunk_size_flt <= 1.0) max_chunk_size_flt = 1.0;
	if (max_chunk_size_flt >= 8.0) max_chunk_size_flt = 8.0;
	max_chunk_size = (int) (max_chunk_size_flt * 1048576.0);
	bandwidth_rate_limit = (uint64_t) (bandwidth_rate_limit_flt * 131072.0);

	p1BaseURL (base_url, sizeof (base_url));
	if (base_url[0] == 0) return;

/* Measure and hash the residue */

	fd = fopen (filename, "rb");
	if (fd == NULL) return;
	fseek (fd, 0, SEEK_END);
	filesize = ftell (fd);
	fseek (fd, 0, SEEK_SET);

	if (filesize == 0 || filesize > P1_MAX_FILE_SIZE) {
		sprintf (buf, "Staged P-1 residue %s has an implausible size, discarding\n", filename);
		OutputBoth (COMM_THREAD_NUM, buf);
		fclose (fd);
		_unlink (filename);
		return;
	}

	md5_hexdigest_file (fileMD5, filename);

	chunk = (char *) malloc (max_chunk_size);
	if (chunk == NULL) goto end;

/* Ask the proxy where to send the data and how much of it it still needs */

	sprintf (url, "%s/p1_upload/?UserID=%s&Exponent=%lu&FileSize=%" PRIu64 "&FileMD5=%s",
		 base_url, USERID, exponent, filesize, fileMD5);

	curl = curl_easy_init ();
	if (curl == NULL) goto end;
	curl_easy_setopt (curl, CURLOPT_NOPROGRESS, 1);
	curl_easy_setopt (curl, CURLOPT_URL, url);
	curl_easy_setopt (curl, CURLOPT_SSL_VERIFYPEER, FALSE);
	if (debug) {
		sprintf (buf, "URL: %s\n", url);
		LogMsg (buf);
	}

	info.buf = curlbuf;
	info.bufsize = sizeof (curlbuf) - 1;
	info.buflen = 0;
	curlbuf[0] = 0;
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, &p1UploadWriteMemoryCallback);
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, &info);
	curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt (curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, 180);
	curl_easy_setopt (curl, CURLOPT_TIMEOUT, 180);
	curl_easy_setopt (curl, CURLOPT_ERRORBUFFER, errbuf);

	res = curl_easy_perform (curl);
	curl_easy_cleanup (curl);
	if (res != CURLE_OK) {
		if (debug) {
			sprintf (buf, "P-1 upload handshake failed: %s\n", errbuf);
			LogMsg (buf);
		}
		goto end;
	}

	json = cJSON_Parse (curlbuf);
	if (json == NULL) goto end;

	item = cJSON_GetObjectItem (json, "error_status");
	if (item != NULL) {
		int	status = (int) cJSON_GetNumberValue (item);
		/* 409 means the proxy already has this residue and 401 that it is not
		| expecting one; either way keeping the file achieves nothing. */
		if (status == 409 || status == 401) {
			sprintf (buf, "P-1 proxy does not need the stage 1 residue for M%lu (status %d), discarding\n",
				 exponent, status);
			OutputStr (COMM_THREAD_NUM, buf);
			fclose (fd);
			fd = NULL;
			_unlink (filename);
		} else if (debug) {
			sprintf (buf, "P-1 upload refused for M%lu, status %d\n", exponent, status);
			LogMsg (buf);
		}
		goto end;
	}

	item = cJSON_GetObjectItem (json, "URLToUse");
	if (item == NULL || cJSON_GetStringValue (item) == NULL) goto end;
	snprintf (base_url, sizeof (base_url), "%s", cJSON_GetStringValue (item));

	item = cJSON_GetObjectItem (json, "need");
	if (item == NULL) goto end;
	item = cJSON_GetArrayItem (item, 0);
	if (item == NULL || item->string == NULL) goto end;
	sscanf (item->string, "%" SCNu64, &chunk_start);
	chunk_end = (uint64_t) cJSON_GetNumberValue (item);
	if (chunk_start > chunk_end || chunk_end >= filesize) goto end;

	sprintf (buf, "Sending stage 1 residue for M%lu to the P-1 proxy (%" PRIu64 " bytes)\n", exponent, filesize);
	OutputStr (COMM_THREAD_NUM, buf);
	if (chunk_start) {
		sprintf (buf, "Resuming from offset %" PRIu64 "\n", chunk_start);
		OutputStr (COMM_THREAD_NUM, buf);
	}

	cJSON_Delete (json);
	json = NULL;

/* Send the data, one chunk per request, until the proxy says it has it all */

	for ( ; ; ) {
		curl_mime *mime;
		curl_mimepart *part;
		uint64_t new_chunk_start;

		if (!inUploadWindow (NULL)) goto end;

		_fseeki64 (fd, chunk_start, SEEK_SET);
		datasize = (int) (chunk_end - chunk_start + 1);
		if (datasize > max_chunk_size) datasize = max_chunk_size;
		if ((int) fread (chunk, 1, datasize, fd) != datasize) goto end;

		md5_hexdigest_buffer (chunkMD5, chunk, datasize);

		snprintf (url, sizeof (url), "%s&FileMD5=%s&DataOffset=%" PRIu64 "&DataSize=%d&DataMD5=%s",
			  base_url, fileMD5, chunk_start, datasize, chunkMD5);

		curl = curl_easy_init ();
		if (curl == NULL) goto end;
		curl_easy_setopt (curl, CURLOPT_NOPROGRESS, 1);
		curl_easy_setopt (curl, CURLOPT_URL, url);
		curl_easy_setopt (curl, CURLOPT_SSL_VERIFYPEER, FALSE);
		if (debug) {
			sprintf (buf, "URL: %s\n", url);
			LogMsg (buf);
		}

		mime = curl_mime_init (curl);
		part = curl_mime_addpart (mime);
		curl_mime_name (part, "Data");
		curl_mime_data (part, chunk, datasize);
		curl_easy_setopt (curl, CURLOPT_MIMEPOST, mime);

		if (bandwidth_rate_limit)
			curl_easy_setopt (curl, CURLOPT_MAX_SEND_SPEED_LARGE, (curl_off_t) bandwidth_rate_limit);

		info.buflen = 0;
		curlbuf[0] = 0;
		curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, &p1UploadWriteMemoryCallback);
		curl_easy_setopt (curl, CURLOPT_WRITEDATA, &info);
		curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1);
		curl_easy_setopt (curl, CURLOPT_NOSIGNAL, 1);
		curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, 180);
		curl_easy_setopt (curl, CURLOPT_TIMEOUT, 180);
		curl_easy_setopt (curl, CURLOPT_ERRORBUFFER, errbuf);

		res = curl_easy_perform (curl);
		curl_mime_free (mime);
		curl_easy_cleanup (curl);
		if (res != CURLE_OK) {
			if (debug) {
				sprintf (buf, "P-1 chunk upload failed: %s\n", errbuf);
				LogMsg (buf);
			}
			goto end;
		}

		json = cJSON_Parse (curlbuf);
		if (json == NULL) goto end;

		item = cJSON_GetObjectItem (json, "error_status");
		if (item != NULL) goto end;

/* Presence alone means success; the value is not meaningful */

		item = cJSON_GetObjectItem (json, "FileUploaded");
		if (item != NULL) {
			sprintf (buf, "Stage 1 residue for M%lu uploaded to the P-1 proxy\n", exponent);
			OutputStr (COMM_THREAD_NUM, buf);
			fclose (fd);
			fd = NULL;
			_unlink (filename);
			goto end;
		}

		item = cJSON_GetObjectItem (json, "need");
		if (item == NULL) goto end;
		item = cJSON_GetArrayItem (item, 0);
		if (item == NULL || item->string == NULL) goto end;
		sscanf (item->string, "%" SCNu64, &new_chunk_start);

/* The offset must move forward or we would loop sending the same bytes */

		if (new_chunk_start <= chunk_start) goto end;
		chunk_start = new_chunk_start;
		chunk_end = (uint64_t) cJSON_GetNumberValue (item);
		if (chunk_start > chunk_end || chunk_end >= filesize) goto end;

		cJSON_Delete (json);
		json = NULL;
	}

end:	if (json != NULL) cJSON_Delete (json);
	if (chunk != NULL) free (chunk);
	if (fd != NULL) fclose (fd);
}

/* This routine runs in its own thread looking for staged residues to upload.
|
| Modelled on proofUploader: hourly sweeps, respecting the same upload time
| window, woken early when there is something new to send. */

void p1Uploader (void *arg)
{
	char	p1_files[50][512];
	int	i, minutes_to_wait, minutes_to_start, num_files;

	gwevent_init (&P1_UPLOAD_EVENT);

	for ( ; ; gwevent_wait (&P1_UPLOAD_EVENT, minutes_to_wait * 60)) {

		gwevent_reset (&P1_UPLOAD_EVENT);
		minutes_to_wait = 60;

		if (!USE_PRIMENET) continue;

/* Nothing to do unless a proxy has told us it wants these */

		if (!IniSectionGetInt (INI_FILE, SEC_PrimeNet, KEY_P1Uploads, 1)) continue;
		if (!IniSectionGetInt (INI_FILE, SEC_PrimeNet, KEY_AcceptsP1Uploads, 0)) continue;

		if (!inUploadWindow (&minutes_to_start)) {
			if (minutes_to_start >= 0 && minutes_to_start < 60) minutes_to_wait = minutes_to_start;
			continue;
		}

		num_files = P1FileNames (p1_files);
		if (num_files < 0) num_files = 0;

/* The proxy rejects a residue for an exponent whose result it has not seen yet,
| so give the results reporter a moment to get there first.  Same reasoning, and
| same remedy, as the sleep in proofUploader. */

		if (num_files) Sleep (60 * 1000);

		for (i = 0; i < num_files && inUploadWindow (NULL); i++) {
			P1Upload (p1_files[i]);
		}
	}
}

/*--------------------------------------------------------------------------
| Downloading a stage 1 residue
+--------------------------------------------------------------------------*/

/* Fetch a stage 1 residue and write it where pminus1 will find it.
|
| Returns TRUE if the residue is in place.  The body is the raw save file, with
| no MD5 prefix -- unlike proof_get_data, whose first 32 bytes are a digest.  No
| integrity check is needed here because the save file carries two of its own:
| read_header rejects a file whose k/b/n/c do not match the work unit, and
| pm1_restore rejects a checksum mismatch.  A corrupted download therefore costs
| a redundant stage 1, not a wrong answer.
|
| Failure is never fatal: without the residue the worker simply computes stage 1
| itself, which is what it would have done anyway. */

int P1Download (
	int	thread_num,
	struct work_unit *w,
	const char *url)
{
	CURL	*curl;
	CURLcode res;
	FILE	*fd;
	char	savefile[32], tempfile[64], buf[512], errbuf[CURL_ERROR_SIZE];
	double	rate_limit;
	long	http_code = 0;
	int	debug;

	if (url == NULL || url[0] == 0) return (FALSE);

	debug = IniSectionGetInt (INI_FILE, SEC_PrimeNet, KEY_Debug, 0);

/* Download to a temporary name and rename on success, so an interrupted
| transfer cannot leave a truncated file where a save file belongs */

	tempFileName (w, savefile);
	savefile[0] = 'm';
	sprintf (tempfile, "%s.p1dl", savefile);

	fd = fopen (tempfile, "wb");
	if (fd == NULL) return (FALSE);

	curl = curl_easy_init ();
	if (curl == NULL) {
		fclose (fd);
		_unlink (tempfile);
		return (FALSE);
	}

	sprintf (buf, "Downloading P-1 stage 1 residue for M%lu\n", w->n);
	OutputStr (thread_num, buf);
	if (debug) {
		sprintf (buf, "URL: %s\n", url);
		LogMsg (buf);
	}

	curl_easy_setopt (curl, CURLOPT_NOPROGRESS, 1);
	curl_easy_setopt (curl, CURLOPT_URL, url);
	curl_easy_setopt (curl, CURLOPT_SSL_VERIFYPEER, FALSE);
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, NULL);	/* default: fwrite to WRITEDATA */
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, fd);
	curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt (curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, 180);
	curl_easy_setopt (curl, CURLOPT_TIMEOUT, 3600);		/* residues are large */
	curl_easy_setopt (curl, CURLOPT_ERRORBUFFER, errbuf);

	rate_limit = IniSectionGetFloat (INI_FILE, SEC_PrimeNet, KEY_DownloadRateLimit, 0.0);
	if (rate_limit > 0.0)
		curl_easy_setopt (curl, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t) (rate_limit * 131072.0));

	res = curl_easy_perform (curl);
	curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_easy_cleanup (curl);
	fclose (fd);

	if (res != CURLE_OK || http_code != 200) {
		if (debug) {
			sprintf (buf, "P-1 residue download failed (curl %d, http %ld): %s\n", (int) res, http_code, errbuf);
			LogMsg (buf);
		}
		_unlink (tempfile);
		sprintf (buf, "Could not fetch the stage 1 residue for M%lu; computing stage 1 locally\n", w->n);
		OutputStr (thread_num, buf);
		return (FALSE);
	}

/* Replace any existing save file: the residue we were pointed at is the one
| matching the B1 in this assignment */

	_unlink (savefile);
	if (rename (tempfile, savefile) != 0) {
		_unlink (tempfile);
		return (FALSE);
	}

	sprintf (buf, "Stage 1 residue for M%lu is in place; resuming at stage 2\n", w->n);
	OutputStr (thread_num, buf);
	return (TRUE);
}
