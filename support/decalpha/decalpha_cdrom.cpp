// DEC Alpha CD-ROM slot: the disc from its first track's start as 2048-byte sectors (every data
// track's, multi-session discs included), plus the TOC and CD-audio windows. Images through
// support/discimg.

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "../../user_io.h"
#include "../discimg/discimg.h"
#include "decalpha.h"
#include "decalpha_cdrom.h"

struct decalpha_cdrom_state
{
	int       active;
	discimg_t img;

	int       trk;
	int       lba0;
	int       ltrk;
	uint64_t  sectors;

	int64_t   aframe_lba;
	uint8_t   aframe[2352];
};

static decalpha_cdrom_state cd = {};

static uint32_t flat_sectors = 0;

void decalpha_cdrom_unmount(int index)
{
	if (index != decalpha_cdrom_slot()) return;
	flat_sectors = 0;
	discimg_close(&cd.img);
	memset(&cd, 0, sizeof(cd));
}

uint64_t decalpha_cdrom_size(int index)
{
	if (!decalpha_cdrom_active(index)) return 0;
	return cd.sectors * 2048ULL;
}

int decalpha_cdrom_active(int index)
{
	return index == decalpha_cdrom_slot() && cd.active;
}

static void log_toc(const char *src)
{
	const toc_t *t = &cd.img.toc;
	printf("DECALPHA CD: %s, %d track(s), leadout %d, first data track %d (%llu sectors)\n",
	       src, t->last, t->end, cd.trk + 1, (unsigned long long)cd.sectors);
	for (int i = 0; i < t->last; i++)
	{
		printf("DECALPHA CD:   track %2d %s start %6d end %6d raw %4d pregap %d\n",
		       i + 1, t->tracks[i].type == TT_CDDA ? "audio" : "data ",
		       t->tracks[i].start, t->tracks[i].end, t->tracks[i].sector_size, t->tracks[i].pregap);
	}
}

int decalpha_cdrom_mount(int index, const char *name)
{
	if (index != decalpha_cdrom_slot()) return DECALPHA_CDROM_PASSTHRU;
	decalpha_cdrom_unmount(index);

	int r = discimg_open(&cd.img, name, "DECALPHA CD");
	if (r == DISCIMG_FLAT)
	{
		uint32_t n = (uint32_t)(cd.img.bytes / 2048);
		discimg_close(&cd.img);
		flat_sectors = n;
		return DECALPHA_CDROM_PASSTHRU;
	}
	if (r != DISCIMG_OPENED || cd.img.kind != DISCIMG_CD)
	{
		if (r == DISCIMG_OPENED) printf("DECALPHA CD: %s is not a CD\n", name);
		decalpha_cdrom_unmount(index);
		return DECALPHA_CDROM_REJECT;
	}

	const toc_t *t = &cd.img.toc;
	cd.trk     = discimg_data_track(&cd.img);
	cd.lba0    = t->last ? t->tracks[0].start : 0;
	cd.ltrk    = cd.trk < 0 ? 0 : cd.trk;
	cd.sectors = (uint64_t)(t->end - cd.lba0);
	int len = strlen(name);
	log_toc(cd.img.is_chd ? "chd" : (len > 4 && !strcasecmp(name + len - 4, ".cue")) ? "cue" : "raw image");

	cd.active = 1;
	cd.aframe_lba = -1;
	return DECALPHA_CDROM_HANDLED;
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

static void fill_toc(uint8_t *buf, int sz)
{
	if (sz < 1024) return;
	int n = 0, lses = 1, lfirst = 1;
	if (cd.active)
	{
		for (int i = 0; i < cd.img.toc.last && i < 99; i++, n++)
		{
			toc_entry(buf + 8 + 8 * n, cd.img.toc.tracks[i].type == TT_CDDA ? 0x10 : 0x14,
			          i + 1, (uint32_t)(cd.img.toc.tracks[i].start - cd.lba0));
			if (cd.img.session[i] > lses) { lses = cd.img.session[i]; lfirst = i + 1; }
		}
		if (n == 0) return;
		toc_entry(buf + 8 + 8 * n,
		          cd.img.toc.tracks[n - 1].type == TT_CDDA ? 0x10 : 0x14, 0xAA,
		          (uint32_t)(cd.img.toc.end - cd.lba0));
	}
	else if (flat_sectors)
	{
		toc_entry(buf + 8, 0x14, 1, 0);
		toc_entry(buf + 16, 0x14, 0xAA, flat_sectors);
		n = 1;
	}
	else return;
	buf[0] = 1;
	buf[1] = (uint8_t)n;
	buf[2] = (uint8_t)(n + 1);
	buf[3] = 1;
	buf[4] = (uint8_t)lfirst;
	buf[5] = (uint8_t)lses;
}

static int audio_frame(uint32_t disc_lba)
{
	if (cd.aframe_lba == (int64_t)disc_lba) return 1;

	int ti = -1;
	for (int i = 0; i < cd.img.toc.last; i++)
	{
		if (cd.img.toc.tracks[i].type == TT_CDDA &&
		    (int)disc_lba >= cd.img.toc.tracks[i].start && (int)disc_lba < cd.img.toc.tracks[i].end)
			{ ti = i; break; }
	}
	if (ti < 0) return 0;
	if (!discimg_read_frame(&cd.img, ti, (int)disc_lba, cd.aframe)) return 0;
	cd.aframe_lba = disc_lba;
	return 1;
}

int decalpha_cdrom_window(int index, uint64_t lba, uint8_t *buf, int sz)
{
	if (index != decalpha_cdrom_slot() || lba < DECALPHA_CDROM_AUDIO_BLK) return 0;
	memset(buf, 0, sz);
	if (lba >= DECALPHA_CDROM_TOC_BLK)
	{
		if (lba == DECALPHA_CDROM_TOC_BLK) fill_toc(buf, sz);
		return 1;
	}
	uint64_t off = (lba - DECALPHA_CDROM_AUDIO_BLK) * 512;
	for (int pos = 0; pos < sz; )
	{
		uint32_t f = (uint32_t)(off / 2560);
		uint32_t fo = (uint32_t)(off % 2560);
		int n = 2560 - fo;
		if (n > sz - pos) n = sz - pos;
		if (fo < 2352 && cd.active && audio_frame(f + (uint32_t)cd.lba0))
		{
			int m = 2352 - fo;
			if (m > n) m = n;
			memcpy(buf + pos, cd.aframe + fo, m);
		}
		pos += n;
		off += n;
	}
	return 1;
}

static int data_track(int x)
{
	cd_track_t *k = &cd.img.toc.tracks[cd.ltrk];
	if (k->type != TT_CDDA && x >= k->start && x < k->end) return cd.ltrk;
	for (int i = 0; i < cd.img.toc.last; i++)
	{
		k = &cd.img.toc.tracks[i];
		if (k->type != TT_CDDA && x >= k->start && x < k->end) return cd.ltrk = i;
	}
	return -1;
}

void decalpha_cdrom_fill(int index, uint64_t lba, uint8_t *buf, int sz)
{
	memset(buf, 0, sz);
	if (index != decalpha_cdrom_slot() || !cd.active) return;
	if (sz < 512 || (sz & 511) || sz > UIO_BUFFER_SIZE) return;
	if (cd.trk < 0) return;

	uint64_t byte_pos = lba * 512ULL;
	for (int pos = 0; pos < sz; )
	{
		uint64_t cd_lba = byte_pos >> 11;
		uint32_t off    = byte_pos & 2047;
		int      n      = 2048 - off;
		if (n > sz - pos) n = sz - pos;

		if (cd_lba >= cd.sectors) break;
		int x = (int)cd_lba + cd.lba0;
		int t = data_track(x);
		if (t >= 0)
		{
			discimg_read(&cd.img, t, x, discimg_data_offset(&cd.img.toc.tracks[t]) + off, n, buf + pos);
		}
		pos      += n;
		byte_pos += n;
	}
}
