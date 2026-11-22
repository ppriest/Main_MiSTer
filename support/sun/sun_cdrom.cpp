// Sun CD-ROM slot: CUE/CHD/raw-sector images served as a flat disc of 2048-byte sectors.

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "../../file_io.h"
#include "../../user_io.h"
#include "../../hardware.h"
#include "../discimg/discimg.h"
#include "sun.h"
#include "sun_cdrom.h"

struct sun_cdrom_state
{
	int       active;
	discimg_t img;

	int       trk;          // the data track, or -1
	uint32_t  data_soff;    // user data offset in its stored sector
	uint64_t  sectors;

	int64_t   aframe_lba;   // the audio frame in aframe (-1: none)
	uint8_t   aframe[2352];
};

static sun_cdrom_state cd = {};

// sectors of a flat ISO on the generic path, for the TOC window (0: none)
static uint32_t flat_sectors = 0;

void sun_cdrom_unmount(int index)
{
	if (index != sun_cdrom_slot()) return;
	flat_sectors = 0;
	discimg_close(&cd.img);
	memset(&cd, 0, sizeof(cd));
}

uint64_t sun_cdrom_size(int index)
{
	if (!sun_cdrom_active(index)) return 0;
	return cd.sectors * 2048ULL;
}

int sun_cdrom_active(int index)
{
	return index == sun_cdrom_slot() && cd.active;
}

static void log_toc(const char *src)
{
	const toc_t *t = &cd.img.toc;
	printf("Sun CD: %s, %d track(s), leadout %d, data track %d (%llu sectors)\n",
	       src, t->last, t->end, cd.trk + 1, (unsigned long long)cd.sectors);
	for (int i = 0; i < t->last; i++)
		printf("Sun CD:   track %2d %s start %6d end %6d raw %4d pregap %d\n",
		       i + 1, t->tracks[i].type == TT_CDDA ? "audio" : "data ",
		       t->tracks[i].start, t->tracks[i].end,
		       t->tracks[i].sector_size, t->tracks[i].pregap);
}

int sun_cdrom_mount(int index, const char *name)
{
	if (index != sun_cdrom_slot()) return SUN_CDROM_PASSTHRU;
	sun_cdrom_unmount(index);

	int r = discimg_open(&cd.img, name, "Sun CD");
	if (r == DISCIMG_FLAT)
	{
		// flat 2048: the generic path serves it
		uint32_t n = (uint32_t)(cd.img.bytes / 2048);
		discimg_close(&cd.img);
		flat_sectors = n;
		return SUN_CDROM_PASSTHRU;
	}
	if (r != DISCIMG_OPENED || cd.img.kind != DISCIMG_CD)
	{
		if (r == DISCIMG_OPENED) printf("Sun CD: %s is not a CD\n", name);
		sun_cdrom_unmount(index);
		return SUN_CDROM_REJECT;
	}

	const toc_t *t = &cd.img.toc;
	cd.trk = discimg_data_track(&cd.img);
	if (cd.trk < 0)
	{
		// audio-only: reads return zeros, but a non-zero capacity keeps the disc visible
		cd.data_soff = 0;
		cd.sectors   = (uint64_t)t->end;
	}
	else
	{
		cd.data_soff = discimg_data_offset(&t->tracks[cd.trk]);
		cd.sectors   = (uint64_t)(t->tracks[cd.trk].end - t->tracks[cd.trk].start);
	}
	int len = strlen(name);
	log_toc(cd.img.is_chd ? "chd" : (len > 4 && !strcasecmp(name + len - 4, ".cue")) ? "cue" : "raw image");

	cd.active = 1;
	cd.aframe_lba = -1;
	return SUN_CDROM_HANDLED;
}

// the TOC and audio windows

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
	int n = 0;
	if (cd.active)
	{
		for (int i = 0; i < cd.img.toc.last && i < 99; i++, n++)
			toc_entry(buf + 8 + 8 * n, cd.img.toc.tracks[i].type == TT_CDDA ? 0x10 : 0x14,
			          i + 1, (uint32_t)cd.img.toc.tracks[i].start);
		if (n == 0) return;
		toc_entry(buf + 8 + 8 * n,
		          cd.img.toc.tracks[n - 1].type == TT_CDDA ? 0x10 : 0x14, 0xAA,
		          (uint32_t)cd.img.toc.end);
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

int sun_cdrom_window(int index, uint64_t lba, uint8_t *buf, int sz)
{
	if (index != sun_cdrom_slot() || lba < SUN_CDROM_AUDIO_BLK) return 0;
	memset(buf, 0, sz);
	if (lba >= SUN_CDROM_TOC_BLK)
	{
		if (lba == SUN_CDROM_TOC_BLK) fill_toc(buf, sz);
		return 1;
	}
	// the audio window: frame f at block 5f, 2560 bytes each
	uint64_t off = (lba - SUN_CDROM_AUDIO_BLK) * 512;
	for (int pos = 0; pos < sz; )
	{
		uint32_t f = (uint32_t)(off / 2560);
		uint32_t fo = (uint32_t)(off % 2560);
		int n = 2560 - fo;
		if (n > sz - pos) n = sz - pos;
		if (fo < 2352 && cd.active && audio_frame(f))
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

void sun_cdrom_fill(int index, uint64_t lba, uint8_t *buf, int sz)
{
	memset(buf, 0, sz);
	if (index != sun_cdrom_slot() || !cd.active) return;
	if (sz < 512 || (sz & 511) || sz > UIO_BUFFER_SIZE) return;
	if (cd.trk < 0) return;
	cd_track_t *k = &cd.img.toc.tracks[cd.trk];

	uint64_t byte_pos = lba * 512ULL;
	for (int pos = 0; pos < sz; )
	{
		uint64_t cd_lba = byte_pos >> 11;
		uint32_t off    = byte_pos & 2047;
		int      n      = 2048 - off;
		if (n > sz - pos) n = sz - pos;

		if (cd_lba >= cd.sectors) break;      // past the end: zeros

		discimg_read(&cd.img, cd.trk, (int)cd_lba + k->start, cd.data_soff + off, n, buf + pos);
		pos      += n;
		byte_pos += n;
	}
}
