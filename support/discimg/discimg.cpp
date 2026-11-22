// Disc and disk images: CHD (CD, DVD, hard disk), CUE/BIN and single raw data tracks.

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "../../file_io.h"
#include "../../user_io.h"
#include "../chd/mister_chd.h"
#include "discimg.h"

#define DVD_TAG CHD_MAKE_TAG('D','V','D',' ')   // chdman createdvd's metadata
#define HDD_TAG CHD_MAKE_TAG('G','D','D','D')   // chdman createhd's geometry

uint32_t discimg_data_offset(const cd_track_t *k)
{
	if (k->sector_size == 2048) return 0;
	if (k->sector_size == 2336) return 8;               // MODE2 form 1, no sync/header
	return k->type == TT_MODE2 ? 24 : 16;              // raw: sync, header (, subheader)
}

int discimg_data_track(const discimg_t *d)
{
	for (int i = 0; i < d->toc.last; i++)
	{
		if (d->toc.tracks[i].type != TT_CDDA) return i;
	}
	return -1;
}

void discimg_close(discimg_t *d)
{
	if (d->is_chd && d->toc.chd_f) chd_close(d->toc.chd_f);
	for (int i = 0; i < d->toc.last; i++)
	{
		if (d->toc.tracks[i].f.opened()) FileClose(&d->toc.tracks[i].f);
	}
	if (d->hunkbuf) free(d->hunkbuf);
	const char *tag = d->tag;
	memset(d, 0, sizeof(*d));
	d->tag = tag;
}

static int open_chd(discimg_t *d, const char *name)
{
	if (mister_load_chd(name, &d->toc) != CHDERR_NONE || !d->toc.chd_f)
	{
		d->toc.chd_f = NULL;
		d->toc.last = 0;
		printf("%s: failed to load chd %s\n", d->tag, name);
		return DISCIMG_REJECT;
	}
	d->is_chd = 1;
	d->hunkbuf = (uint8_t *)malloc(d->toc.chd_hunksize);
	if (!d->hunkbuf) return DISCIMG_REJECT;
	d->hunknum = -1;

	if (d->toc.last > 0)
	{
		d->kind = DISCIMG_CD;
		return DISCIMG_OPENED;
	}

	if (chd_get_metadata(d->toc.chd_f, DVD_TAG, 0, NULL, 0, NULL, NULL, NULL) == CHDERR_NONE)
	{
		d->kind = DISCIMG_DVD;
		d->bytes = chd_get_header(d->toc.chd_f)->logicalbytes;
		return DISCIMG_OPENED;
	}

	char gddd[256] = {};
	unsigned cyls, heads, secs, bps;
	if (chd_get_metadata(d->toc.chd_f, HDD_TAG, 0, gddd, sizeof(gddd) - 1, NULL, NULL, NULL) == CHDERR_NONE &&
	    sscanf(gddd, "CYLS:%u,HEADS:%u,SECS:%u,BPS:%u", &cyls, &heads, &secs, &bps) == 4)
	{
		d->kind = DISCIMG_HDD;
		d->bytes = chd_get_header(d->toc.chd_f)->logicalbytes;
		d->cyls = cyls;
		d->heads = heads;
		d->secs = secs;
		d->bps = bps;
		return DISCIMG_OPENED;
	}

	printf("%s: %s is neither a CD, a DVD nor a hard disk CHD\n", d->tag, name);
	return DISCIMG_REJECT;
}

static uint32_t msf_field(const char *line)
{
	int m = 0, s = 0, f = 0;
	if (sscanf(line, "%*s %*d %d:%d:%d", &m, &s, &f) != 3) return 0;
	return (uint32_t)(m * 60 + s) * 75 + f;
}

static int open_cue(discimg_t *d, const char *name)
{
	fileTYPE cue = {};
	if (!FileOpen(&cue, name)) return DISCIMG_REJECT;
	int sz = cue.size > 65535 ? 65535 : (int)cue.size;
	char *txt = (char *)malloc(sz + 1);
	if (!txt)
	{
		FileClose(&cue);
		return DISCIMG_REJECT;
	}
	FileReadAdv(&cue, txt, sz);
	FileClose(&cue);
	txt[sz] = 0;

	toc_t *t = &d->toc;
	char fpath[1024] = {};
	uint64_t fsize = 0;
	uint32_t file_base = 0;       // disc LBA of the current FILE's frame 0
	uint32_t synth = 0;           // PREGAP frames so far, in no file
	uint32_t pending_pregap = 0;
	int      cur_type = -1;
	uint32_t cur_ss = 0;
	int      index0 = -1;
	int      prev_in_file = -1;   // last track of the current FILE
	int      cur_session = 1;
	int      bad = 0;

	char *line = strtok(txt, "\r\n");
	while (line && !bad)
	{
		while (*line == ' ' || *line == '\t') line++;

		if (!strncasecmp(line, "FILE", 4))
		{
			// the previous FILE's last track runs to its end
			if (prev_in_file >= 0)
			{
				cd_track_t *p = &t->tracks[prev_in_file];
				uint32_t ffr = (uint32_t)(fsize / p->sector_size);
				uint32_t infile1 = (uint32_t)(p->start + p->offset);
				p->end = p->start + (ffr > infile1 ? ffr - infile1 : 0);
				file_base = p->end - synth;
			}
			char *q1 = strchr(line, '"');
			char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
			if (!q1 || !q2 || (q2 - q1) <= 1 || !strcasestr(q2 + 1, "BINARY"))
			{
				printf("%s: cue FILE unsupported: %s\n", d->tag, line);
				bad = 1;
				break;
			}
			*q2 = 0;
			strncpy(fpath, name, sizeof(fpath) - 1);
			char *slash = strrchr(fpath, '/');
			if (slash) strncpy(slash + 1, q1 + 1, sizeof(fpath) - (slash - fpath) - 2);
			else strncpy(fpath, q1 + 1, sizeof(fpath) - 1);
			fsize = 0;
			prev_in_file = -1;
		}
		else if (!strncasecmp(line, "TRACK", 5))
		{
			if (t->last >= 99) break;
			cur_type = -1;
			cur_ss = 0;
			index0 = -1;
			if (strcasestr(line, "AUDIO"))           { cur_type = TT_CDDA;  cur_ss = 2352; }
			else if (strcasestr(line, "MODE1/2352")) { cur_type = TT_MODE1; cur_ss = 2352; }
			else if (strcasestr(line, "MODE1/2048")) { cur_type = TT_MODE1; cur_ss = 2048; }
			else if (strcasestr(line, "MODE2/2352")) { cur_type = TT_MODE2; cur_ss = 2352; }
			else if (strcasestr(line, "MODE2/2336")) { cur_type = TT_MODE2; cur_ss = 2336; }
			else
			{
				printf("%s: cue TRACK unsupported: %s\n", d->tag, line);
				bad = 1;
			}
		}
		else if (!strncasecmp(line, "REM SESSION", 11))
		{
			int ses = 0;
			if (sscanf(line + 11, "%d", &ses) == 1 && ses > 0 && ses < 100) cur_session = ses;
		}
		else if (!strncasecmp(line, "PREGAP", 6))
		{
			int m = 0, s = 0, f = 0;
			if (sscanf(line, "%*s %d:%d:%d", &m, &s, &f) == 3) pending_pregap += (uint32_t)(m * 60 + s) * 75 + f;
		}
		else if (!strncasecmp(line, "INDEX", 5) && cur_type >= 0)
		{
			int idx = 0;
			sscanf(line, "%*s %d", &idx);
			if (idx == 0)
			{
				index0 = (int)msf_field(line);
			}
			else if (idx == 1)
			{
				uint32_t index1 = msf_field(line);
				cd_track_t *k = &t->tracks[t->last];
				memset(k, 0, sizeof(*k));
				if (!fpath[0] || !FileOpen(&k->f, fpath))
				{
					printf("%s: cue references missing bin: %s\n", d->tag, fpath[0] ? fpath : "?");
					bad = 1;
					break;
				}
				if (!fsize) fsize = k->f.size;

				synth += pending_pregap;
				k->type        = (TrackType)cur_type;
				k->sector_size = (int)cur_ss;
				k->start       = (int)(file_base + synth + index1);
				k->offset      = (int)index1 - k->start;   // file frame = disc LBA + offset
				k->pregap      = (int)((index0 >= 0 ? index1 - (uint32_t)index0 : 0) + pending_pregap);
				k->indexes[0]  = (index0 >= 0) ? index0 : (int)index1;
				k->indexes[1]  = (int)index1;
				k->index_num   = 2;
				pending_pregap = 0;
				d->session[t->last] = (uint8_t)cur_session;

				// the previous track in the same FILE ends where this one's gap begins
				if (prev_in_file >= 0)
				{
					cd_track_t *p = &t->tracks[prev_in_file];
					uint32_t bound = (index0 >= 0) ? (uint32_t)index0 : index1;
					uint32_t p_in1 = (uint32_t)(p->start + p->offset);
					p->end = p->start + (bound > p_in1 ? bound - p_in1 : 0);
				}
				prev_in_file = t->last;
				t->last++;
			}
		}
		line = strtok(NULL, "\r\n");
	}
	free(txt);

	// the last FILE's last track runs to its end
	if (!bad && prev_in_file >= 0)
	{
		cd_track_t *p = &t->tracks[prev_in_file];
		uint32_t ffr = (uint32_t)(fsize / p->sector_size);
		uint32_t infile1 = (uint32_t)(p->start + p->offset);
		p->end = p->start + (ffr > infile1 ? ffr - infile1 : 0);
	}
	if (bad || !t->last)
	{
		if (!bad) printf("%s: cue parse found no tracks (%s)\n", d->tag, name);
		return DISCIMG_REJECT;
	}
	t->end = t->tracks[t->last - 1].end;
	d->kind = DISCIMG_CD;
	return DISCIMG_OPENED;
}

static int probe_pvd(fileTYPE *f, uint32_t sec_size, uint32_t off)
{
	uint8_t pvd[8];
	if (!FileSeek(f, (uint64_t)16 * sec_size + off, SEEK_SET)) return 0;
	if (FileReadAdv(f, pvd, sizeof(pvd)) != sizeof(pvd)) return 0;
	if (pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5)) return 1;
	if (!memcmp(pvd, "\x01\x43\x44\x52\x4f\x4d", 6)) return 1;  // "\1CDROM"
	return 0;
}

static int open_raw(discimg_t *d, const char *name, int flags)
{
	cd_track_t *k = &d->toc.tracks[0];
	if (!FileOpen(&k->f, name)) return DISCIMG_REJECT;

	static const struct { uint32_t ss, off; int m2; } cand[] = {
		{ 2048,  0, 0 },   // flat
		{ 2352, 16, 0 },   // MODE1 raw
		{ 2336,  8, 1 },   // MODE2 form 1
		{ 2352, 24, 1 },   // MODE2 raw
	};
	int found = -1;
	for (unsigned i = 0; i < sizeof(cand) / sizeof(cand[0]); i++)
	{
		if (probe_pvd(&k->f, cand[i].ss, cand[i].off))
		{
			found = (int)i;
			break;
		}
	}

	if (found < 0)
	{
		// no volume descriptor (a UFS-only disc, say): raw 2352 if it starts with a sync pattern, else flat
		static const uint8_t sync[12] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
		uint8_t hdr[12] = {};
		FileSeek(&k->f, 0, SEEK_SET);
		FileReadAdv(&k->f, hdr, sizeof(hdr));
		if (!memcmp(hdr, sync, sizeof(sync))) found = 1;
	}

	if (found < 0 && (flags & DISCIMG_FLAT_TRACK)) found = 0;
	if (found < 0 || (found == 0 && !(flags & DISCIMG_FLAT_TRACK)))
	{
		d->bytes = (uint64_t)k->f.size;
		FileClose(&k->f);
		return DISCIMG_FLAT;
	}

	k->type        = cand[found].m2 ? TT_MODE2 : TT_MODE1;
	k->sector_size = (int)cand[found].ss;
	k->start       = 0;
	k->offset      = 0;
	k->end         = (int)(k->f.size / k->sector_size);
	k->indexes[0]  = k->indexes[1] = 0;
	k->index_num   = 2;
	d->toc.last    = 1;
	d->toc.end     = k->end;
	d->kind        = DISCIMG_CD;
	return DISCIMG_OPENED;
}

int discimg_open(discimg_t *d, const char *name, const char *tag, int flags)
{
	memset(d, 0, sizeof(*d));
	d->tag = tag;
	memset(d->session, 1, sizeof(d->session));
	int len = strlen(name);
	if (len > 4 && !strcasecmp(name + len - 4, ".chd")) return open_chd(d, name);
	if (len > 4 && !strcasecmp(name + len - 4, ".cue")) return open_cue(d, name);
	return open_raw(d, name, flags);
}

int discimg_read(discimg_t *d, int trk, int lba, uint32_t off, int n, uint8_t *dst)
{
	cd_track_t *k = &d->toc.tracks[trk];
	if (d->is_chd)
	{
		if (mister_chd_read_sector(d->toc.chd_f, lba + k->offset, 0, off, n, dst, d->hunkbuf, &d->hunknum) == CHDERR_NONE) return 1;
	}
	else
	{
		diskled_on();
		if (FileSeek(&k->f, (uint64_t)(lba + k->offset) * k->sector_size + off, SEEK_SET) &&
		    FileReadAdv(&k->f, dst, n) == n)
		{
			return 1;
		}
	}
	memset(dst, 0, n);
	return 0;
}

static uint8_t bcd(uint32_t v)
{
	return (uint8_t)((v / 10) << 4 | v % 10);
}

int discimg_read_frame(discimg_t *d, int trk, int lba, uint8_t *frame)
{
	cd_track_t *k = &d->toc.tracks[trk];
	if (k->sector_size == 2352)
	{
		if (!discimg_read(d, trk, lba, 0, 2352, frame)) return 0;
		if (d->is_chd && k->type == TT_CDDA)
		{
			// CHD stores CD audio byte-swapped; a .bin has it little-endian
			for (int i = 0; i < 2352; i += 2)
			{
				uint8_t x = frame[i];
				frame[i] = frame[i + 1];
				frame[i + 1] = x;
			}
		}
		return 1;
	}

	memset(frame, 0, 2352);
	memset(frame + 1, 0xff, 10);
	uint32_t a = (uint32_t)lba + 150;
	frame[12] = bcd(a / (75 * 60));
	frame[13] = bcd((a / 75) % 60);
	frame[14] = bcd(a % 75);
	frame[15] = k->type == TT_MODE2 ? 2 : 1;
	return discimg_read(d, trk, lba, 0, k->sector_size, frame + 16);
}

int discimg_read_bytes(discimg_t *d, uint64_t pos, int n, uint8_t *dst)
{
	uint32_t hb = d->toc.chd_hunksize;
	int ok = 1;
	while (n > 0)
	{
		int h = (int)(pos / hb);
		uint32_t off = (uint32_t)(pos % hb);
		int m = (int)(hb - off);
		if (m > n) m = n;
		if (pos >= d->bytes)
		{
			memset(dst, 0, m);
			ok = 0;
		}
		else if (h != d->hunknum && chd_read(d->toc.chd_f, h, d->hunkbuf) != CHDERR_NONE)
		{
			memset(dst, 0, m);
			ok = 0;
		}
		else
		{
			d->hunknum = h;
			memcpy(dst, d->hunkbuf + off, m);
		}
		dst += m;
		pos += m;
		n -= m;
	}
	return ok;
}
