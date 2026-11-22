// Konami Firebeat arcade core: CD-ROMs (CHD, or .cue and .bin) in the image slots, served as their raw .bin followed by
// a TOC block (the core's rtl/board/fb_cd_slot.sv reads it); a DVD CHD (Pop'n Music's data DVD) as
// its 2048-byte sectors back to back (rtl/spu/fb_dvd_slot.sv); a hard disk CHD (Beatmania III's)
// as its 512-byte sectors back to back, then a geometry block (rtl/spu/fb_atahdd.sv reads it).

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "../../file_io.h"
#include "../../user_io.h"
#include "../../spi.h"
#include "../discimg/discimg.h"
#include "firebeat.h"

#define FB_SLOTS     4
#define FB_FRAME     2352

struct fb_slot
{
	int       active;
	discimg_t img;
	uint64_t  frames;         // CD: frames in the raw image, every track's back to back
	uint64_t  first[100];     // CD: raw-image frame each track starts at
	uint64_t  data_bytes;     // the image's bytes (CD: padded to a 512-byte block), then blk
	uint8_t   blk[1024];      // CD: the TOC block; hard disk: 512 bytes of geometry
	int       blk_len;
};

static fb_slot slot[FB_SLOTS] = {};

char is_firebeat()
{
	// the core's own name (CONF_STR), not the .mra's setname: Firebeat and its test
	// revision FirebeatCD
	return !strncasecmp(user_io_get_core_name(1), "Firebeat", 8);
}

void firebeat_unmount(int index)
{
	if (index < 0 || index >= FB_SLOTS) return;
	fb_slot *s = &slot[index];
	discimg_close(&s->img);
	memset(s, 0, sizeof(*s));
}

static void put32(uint8_t *p, uint32_t v)
{
	for (int b = 0; b < 4; b++) p[b] = (uint8_t)(v >> (8 * b));
}

int firebeat_mount_hook(int index, const char *name, fileTYPE *f, int *writable)
{
	if (!is_firebeat()) return 1;
	sd_service = firebeat_sd_service;
	firebeat_unmount(index);

	int len = strlen(name);
	if (index >= FB_SLOTS || len < 4 || (strcasecmp(name + len - 4, ".chd") && strcasecmp(name + len - 4, ".cue"))) return 1;

	fb_slot *s = &slot[index];
	if (discimg_open(&s->img, name, "Firebeat") != DISCIMG_OPENED)
	{
		firebeat_unmount(index);
		FileClose(f);
		return 0;
	}

	if (s->img.kind == DISCIMG_DVD)
	{
		s->data_bytes = s->img.bytes;
		s->blk_len = 0;
		printf("Firebeat: %s in slot %d, a DVD of %llu bytes\n", name, index, (unsigned long long)s->data_bytes);
	}
	else if (s->img.kind == DISCIMG_HDD)
	{
		// A hard disk: "FBHD", version 1, then cylinders, heads, sectors per track and bytes per
		// sector, each 32 bits little-endian, from the CHD's GDDD metadata (MAME's IDENTIFY uses them).
		if (s->img.bps != 512)
		{
			printf("Firebeat: %s has %u-byte sectors, not 512\n", name, s->img.bps);
			firebeat_unmount(index);
			FileClose(f);
			return 0;
		}
		s->data_bytes = s->img.bytes;
		memcpy(s->blk, "FBHD", 4);
		s->blk[4] = 1;
		put32(s->blk + 8, s->img.cyls);
		put32(s->blk + 12, s->img.heads);
		put32(s->blk + 16, s->img.secs);
		put32(s->blk + 20, s->img.bps);
		s->blk_len = 512;
		printf("Firebeat: %s in slot %d, a hard disk of %llu bytes, %u/%u/%u\n", name, index,
		       (unsigned long long)s->data_bytes, s->img.cyls, s->img.heads, s->img.secs);
	}
	else
	{
		const toc_t *t = &s->img.toc;
		s->frames = 0;
		for (int i = 0; i < t->last; i++)
		{
			s->first[i] = s->frames;
			s->frames += (uint64_t)(t->tracks[i].end - t->tracks[i].start);
		}
		// The TOC block: "FBTC", the track count, version 1, then for each track and the lead-out its
		// start frame (little-endian), bit 0 of byte 4 for audio, byte 5 the control flags. The frames
		// served are MAME's logical frames for a disc without pregaps (cdrom_file: logframeofs equals
		// physframeofs then), which the Firebeat discs are.
		memcpy(s->blk, "FBTC", 4);
		s->blk[4] = (uint8_t)t->last;
		s->blk[5] = 1;
		for (int i = 0; i <= t->last; i++)
		{
			uint8_t *e = s->blk + 8 + 8 * i;
			put32(e, (uint32_t)(i < t->last ? s->first[i] : s->frames));
			e[4] = (i < t->last && t->tracks[i].type == TT_CDDA) ? 1 : 0;
		}
		s->blk_len = sizeof(s->blk);
		s->data_bytes = ((s->frames * FB_FRAME + 511) / 512) * 512;
		printf("Firebeat: %s in slot %d, %d track(s), %llu frames, %llu bytes as a raw image\n", name, index,
		       t->last, (unsigned long long)s->frames, (unsigned long long)(s->data_bytes + s->blk_len));
	}

	s->active = 1;
	*writable = 0;
	f->size = (int64_t)(s->data_bytes + s->blk_len);
	return 1;
}

// bytes [off, off + n) of raw-image frame frame
static void fill_frame(fb_slot *s, uint64_t frame, uint32_t off, int n, uint8_t *dst)
{
	int t = 0;
	while (t + 1 < s->img.toc.last && frame >= s->first[t + 1]) t++;
	cd_track_t *k = &s->img.toc.tracks[t];
	int lba = (int)(frame - s->first[t]) + k->start;

	if (k->sector_size == FB_FRAME && k->type != TT_CDDA)
	{
		discimg_read(&s->img, t, lba, off, n, dst);
		return;
	}
	uint8_t fr[FB_FRAME];
	discimg_read_frame(&s->img, t, lba, fr);
	memcpy(dst, fr + off, n);
}

static void firebeat_fill(fb_slot *s, uint64_t lba, uint8_t *buf, int sz)
{
	uint64_t pos = lba * 512ULL;
	for (int done = 0; done < sz; )
	{
		int n = sz - done;
		if (pos >= s->data_bytes)
		{
			uint64_t o = pos - s->data_bytes;
			for (int i = 0; i < n; i++) buf[done + i] = (o + i < (uint64_t)s->blk_len) ? s->blk[o + i] : 0;
		}
		else if (s->img.kind != DISCIMG_CD)
		{
			if (n > (int)(s->data_bytes - pos)) n = (int)(s->data_bytes - pos);
			discimg_read_bytes(&s->img, pos, n, buf + done);
		}
		else
		{
			uint64_t frame = pos / FB_FRAME;
			uint32_t off = (uint32_t)(pos % FB_FRAME);
			if (n > FB_FRAME - (int)off) n = FB_FRAME - off;
			if (frame >= s->frames)
			{
				// the padding to a 512-byte block
				if (n > (int)(s->data_bytes - pos)) n = (int)(s->data_bytes - pos);
				memset(buf + done, 0, n);
			}
			else fill_frame(s, frame, off, n, buf + done);
		}
		done += n;
		pos += n;
	}
}

int firebeat_sd_service(int disk, fileTYPE *, int op, uint64_t lba, int sz, int ack)
{
	if (!op || !is_firebeat() || disk < 0 || disk >= FB_SLOTS || !slot[disk].active) return 0;

	static uint8_t buf[UIO_BUFFER_SIZE];
	if (!(op & 1) || sz > (int)sizeof(buf)) return -1;      // read-only

	diskled_on();
	firebeat_fill(&slot[disk], lba, buf, sz);

	EnableIO();
	spi_w(UIO_SECTOR_RD | ack);
	spi_block_write(buf, user_io_get_width(), sz);
	DisableIO();
	return 1;
}
