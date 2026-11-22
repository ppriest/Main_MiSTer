// A CD-ROM slot as a flat 2048-byte-sector disc with TOC and CD-audio windows.

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "../../user_io.h"
#include "cdslot.h"

void cdslot_unmount(cdslot_t *s)
{
	discimg_close(&s->img);
	memset(s, 0, sizeof(*s));
}

uint64_t cdslot_size(const cdslot_t *s)
{
	return s->active ? s->sectors * 2048ULL : 0;
}

static void log_toc(const cdslot_t *s, const char *src)
{
	const toc_t *t = &s->img.toc;
	printf("%s: %s, %d track(s), leadout %d, data track %d (%llu sectors)\n",
	       s->img.tag, src, t->last, t->end, s->trk + 1, (unsigned long long)s->sectors);
	for (int i = 0; i < t->last; i++)
	{
		printf("%s:   track %2d %s start %6d end %6d raw %4d pregap %d\n",
		       s->img.tag, i + 1, t->tracks[i].type == TT_CDDA ? "audio" : "data ",
		       t->tracks[i].start, t->tracks[i].end, t->tracks[i].sector_size, t->tracks[i].pregap);
	}
}

int cdslot_mount(cdslot_t *s, const char *name, const char *tag)
{
	cdslot_unmount(s);

	int r = discimg_open(&s->img, name, tag);
	if (r == DISCIMG_FLAT)
	{
		uint32_t n = (uint32_t)(s->img.bytes / 2048);
		cdslot_unmount(s);
		s->flat_sectors = n;
		return CDSLOT_PASSTHRU;
	}
	if (r != DISCIMG_OPENED || s->img.kind != DISCIMG_CD)
	{
		if (r == DISCIMG_OPENED) printf("%s: %s is not a CD\n", tag, name);
		cdslot_unmount(s);
		return CDSLOT_REJECT;
	}

	const toc_t *t = &s->img.toc;
	s->trk = discimg_data_track(&s->img);
	if (s->trk < 0)
	{
		// audio-only: reads return zeros, but a non-zero capacity keeps the disc visible
		s->data_soff = 0;
		s->sectors   = (uint64_t)t->end;
	}
	else
	{
		s->data_soff = discimg_data_offset(&t->tracks[s->trk]);
		s->sectors   = (uint64_t)(t->tracks[s->trk].end - t->tracks[s->trk].start);
	}
	int len = strlen(name);
	log_toc(s, s->img.is_chd ? "chd" : (len > 4 && !strcasecmp(name + len - 4, ".cue")) ? "cue" : "raw image");

	s->active = 1;
	s->aframe_lba = -1;
	return CDSLOT_HANDLED;
}

static void toc_entry(uint8_t *e, int ctl, int trk, uint32_t lba)
{
	uint32_t a = lba + 150;
	e[0] = (uint8_t)ctl;
	e[1] = (uint8_t)trk;
	e[2] = (uint8_t)(a / 4500);
	e[3] = (uint8_t)(a / 75 % 60);
	e[4] = (uint8_t)(a % 75);
	e[5] = (uint8_t)(lba >> 16);
	e[6] = (uint8_t)(lba >> 8);
	e[7] = (uint8_t)lba;
}

static void fill_toc(const cdslot_t *s, uint8_t *buf, int sz)
{
	if (sz < 1024) return;
	int n = 0;
	const toc_t *t = &s->img.toc;
	if (s->active)
	{
		for (int i = 0; i < t->last && i < 99; i++, n++)
		{
			toc_entry(buf + 8 + 8 * n, t->tracks[i].type == TT_CDDA ? 0x10 : 0x14, i + 1, (uint32_t)t->tracks[i].start);
		}
		if (n == 0) return;
		toc_entry(buf + 8 + 8 * n, t->tracks[n - 1].type == TT_CDDA ? 0x10 : 0x14, 0xAA, (uint32_t)t->end);
	}
	else if (s->flat_sectors)
	{
		toc_entry(buf + 8, 0x14, 1, 0);
		toc_entry(buf + 16, 0x14, 0xAA, s->flat_sectors);
		n = 1;
	}
	else
	{
		return;
	}
	buf[0] = 1;
	buf[1] = (uint8_t)n;
	buf[2] = (uint8_t)(n + 1);
	buf[3] = 1;
}

static int audio_frame(cdslot_t *s, uint32_t disc_lba)
{
	if (s->aframe_lba == (int64_t)disc_lba) return 1;

	const toc_t *t = &s->img.toc;
	int ti = -1;
	for (int i = 0; i < t->last; i++)
	{
		if (t->tracks[i].type == TT_CDDA && (int)disc_lba >= t->tracks[i].start && (int)disc_lba < t->tracks[i].end)
		{
			ti = i;
			break;
		}
	}
	if (ti < 0) return 0;
	if (!discimg_read_frame(&s->img, ti, (int)disc_lba, s->aframe)) return 0;
	s->aframe_lba = disc_lba;
	return 1;
}

int cdslot_window(cdslot_t *s, uint64_t audio_blk, uint64_t toc_blk, uint64_t lba, uint8_t *buf, int sz)
{
	if (lba < audio_blk) return 0;
	memset(buf, 0, sz);
	if (lba >= toc_blk)
	{
		if (lba == toc_blk) fill_toc(s, buf, sz);
		return 1;
	}
	// the audio window: frame f at block 5f, 2560 bytes each
	uint64_t off = (lba - audio_blk) * 512;
	for (int pos = 0; pos < sz; )
	{
		uint32_t f = (uint32_t)(off / 2560);
		uint32_t fo = (uint32_t)(off % 2560);
		int n = 2560 - fo;
		if (n > sz - pos) n = sz - pos;
		if (fo < 2352 && s->active && audio_frame(s, f))
		{
			int m = 2352 - fo;
			if (m > n) m = n;
			memcpy(buf + pos, s->aframe + fo, m);
		}
		pos += n;
		off += n;
	}
	return 1;
}

void cdslot_fill(cdslot_t *s, uint64_t lba, uint8_t *buf, int sz)
{
	memset(buf, 0, sz);
	if (!s->active) return;
	if (sz < 512 || (sz & 511) || sz > UIO_BUFFER_SIZE) return;
	if (s->trk < 0) return;
	const cd_track_t *k = &s->img.toc.tracks[s->trk];

	uint64_t byte_pos = lba * 512ULL;
	for (int pos = 0; pos < sz; )
	{
		uint64_t cd_lba = byte_pos >> 11;
		uint32_t off    = byte_pos & 2047;
		int      n      = 2048 - off;
		if (n > sz - pos) n = sz - pos;

		if (cd_lba >= s->sectors) break;      // past the end: zeros

		discimg_read(&s->img, s->trk, (int)cd_lba + k->start, s->data_soff + off, n, buf + pos);
		pos      += n;
		byte_pos += n;
	}
}
