#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "../../file_io.h"
#include "../../user_io.h"
#include "../../hardware.h"
#include "../discimg/discimg.h"
#include "next_cdrom.h"
#include "next_cdrom_play.h"

struct next_cdrom_state
{
	int      active;
	discimg_t img;
	next_cd_toc ntoc;
	int      trk;
	uint32_t data_soff;
	uint64_t sectors;
	uint8_t  aframe[2352];
	int64_t  aframe_lba;
};

static next_cdrom_state cd = {};
static next_cd_play play;
static int play_inited;

static next_cd_play *P(void)
{
	if (!play_inited) { next_cd_play_init(&play); play_inited = 1; }
	return &play;
}

void next_cdrom_unmount(int index)
{
	if (index != NEXT_CDROM_SLOT) return;
	discimg_close(&cd.img);
	memset(&cd, 0, sizeof(cd));
	cd.aframe_lba = -1;
	next_cd_play_set_toc(P(), NULL);
}

uint64_t next_cdrom_size(int index)
{
	if (!next_cdrom_active(index)) return 0;
	return cd.sectors * 2048ULL;
}

int next_cdrom_active(int index)
{
	return index == NEXT_CDROM_SLOT && cd.active;
}

const next_cd_toc *next_cdrom_toc(void)
{
	return cd.active ? &cd.ntoc : NULL;
}

void next_cdrom_pos(next_cd_pos *pos)
{
	next_cd_play_pos(P(), pos);
}

const uint8_t *next_cdrom_ports(void)
{
	return P()->ports;
}

static void build_toc(void)
{
	next_cd_toc *t = &cd.ntoc;
	memset(t, 0, sizeof(*t));
	t->n = (cd.img.toc.last > NEXT_CD_MAX_TRACKS) ? NEXT_CD_MAX_TRACKS : cd.img.toc.last;
	for (int i = 0; i < t->n; i++)
	{
		t->ctrl[i]   = (cd.img.toc.tracks[i].type == TT_CDDA) ? 0x10 : 0x14;
		t->start[i]  = (uint32_t)cd.img.toc.tracks[i].start;
		t->pregap[i] = (uint32_t)cd.img.toc.tracks[i].pregap;
	}
	t->leadout  = (uint32_t)cd.img.toc.end;
	t->data_trk = cd.trk;
}

static void log_toc(const char *src)
{
	printf("NeXT CD: %s, %d track(s), leadout %d, data track %d (%llu sectors)\n",
	       src, cd.img.toc.last, cd.img.toc.end, cd.trk + 1, (unsigned long long)cd.sectors);
	for (int i = 0; i < cd.img.toc.last; i++)
		printf("NeXT CD:   track %2d %s start %6d end %6d raw %4d pregap %d\n",
		       i + 1, cd.img.toc.tracks[i].type == TT_CDDA ? "audio" : "data ",
		       cd.img.toc.tracks[i].start, cd.img.toc.tracks[i].end,
		       cd.img.toc.tracks[i].sector_size, cd.img.toc.tracks[i].pregap);
}

static int finish_mount(const char *src)
{
	cd.trk = -1;
	for (int i = 0; i < cd.img.toc.last; i++)
		if (cd.img.toc.tracks[i].type != TT_CDDA) { cd.trk = i; break; }
	if (cd.trk < 0)
	{
		cd.data_soff = 0;
		cd.sectors   = (uint64_t)cd.img.toc.end;
	}
	else
	{
		cd.data_soff = discimg_data_offset(&cd.img.toc.tracks[cd.trk]);
		cd.sectors   = (uint64_t)(cd.img.toc.tracks[cd.trk].end - cd.img.toc.tracks[cd.trk].start);
	}
	cd.aframe_lba = -1;
	build_toc();
	next_cd_play_set_toc(P(), &cd.ntoc);
	log_toc(src);
	return NEXT_CDROM_HANDLED;
}

int next_cdrom_mount(int index, const char *name)
{
	if (index != NEXT_CDROM_SLOT) return NEXT_CDROM_REJECT;
	next_cdrom_unmount(index);

	int r = NEXT_CDROM_REJECT;
	if (discimg_open(&cd.img, name, "NeXT CD", DISCIMG_FLAT_TRACK) == DISCIMG_OPENED)
	{
		if (cd.img.kind == DISCIMG_CD)
		{
			int len = strlen(name);
			r = finish_mount(cd.img.is_chd ? "chd" : (len > 4 && !strcasecmp(name + len - 4, ".cue")) ? "cue" : "raw image");
		}
		else
		{
			printf("NeXT CD: %s is not a CD\n", name);
		}
	}

	if (r == NEXT_CDROM_HANDLED) cd.active = 1;
	else next_cdrom_unmount(index);
	return r;
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

void next_cdrom_fill(int index, uint64_t lba, uint8_t *buf, int sz)
{
	memset(buf, 0, sz);
	if (index != NEXT_CDROM_SLOT || !cd.active) return;
	if (sz < 512 || (sz & 511) || sz > 4096) return;
	if (lba >= NEXT_DATA_LIMIT) return;

	next_cd_play_stop(P());

	if (!cd.img.is_chd && cd.trk >= 0 &&
	    cd.img.toc.tracks[cd.trk].sector_size == 2048 && cd.data_soff == 0)
	{
		cd_track_t *k = &cd.img.toc.tracks[cd.trk];
		uint64_t byte_pos = lba * 512ULL;
		uint64_t end      = cd.sectors * 2048ULL;
		if (byte_pos >= end) return;
		int n = (end - byte_pos < (uint64_t)sz) ? (int)(end - byte_pos) : sz;
		discimg_read(&cd.img, cd.trk, k->start, (uint32_t)byte_pos, n, buf);
		return;
	}

	for (int pos = 0; pos < sz; pos += 512, lba++)
	{
		uint64_t byte_pos = lba * 512ULL;
		uint64_t cd_lba   = byte_pos >> 11;
		uint32_t off      = byte_pos & 2047;

		if (cd_lba >= cd.sectors) break;
		if (cd.trk < 0) break;

		cd_track_t *k = &cd.img.toc.tracks[cd.trk];
		discimg_read(&cd.img, cd.trk, (int)cd_lba + k->start, cd.data_soff + off, 512, buf + pos);
	}
}

void next_cdrom_frame(uint8_t *buf, int sz)
{
	memset(buf, 0, sz);
	if (sz < 2358) return;
	next_cd_play *p = P();
	uint32_t flba = 0;
	int have = next_cd_play_frame(p, &flba);
	if (have && cd.active && audio_frame(flba))
	{
		memcpy(buf, cd.aframe, 2352);
		next_cd_play_scale(p, (int16_t *)buf, 588);
	}
	buf[2352] = next_cd_play_ast(p);
	buf[2353] = (uint8_t)have;
	buf[2354] = (uint8_t)p->flush_gen;         buf[2355] = (uint8_t)(p->flush_gen >> 8);
	buf[2356] = (uint8_t)(p->flush_gen >> 16); buf[2357] = (uint8_t)(p->flush_gen >> 24);
}

void next_cdrom_command(uint32_t lba, const uint8_t *buf, int sz)
{
	if (sz < 512) return;
	next_cd_play *p = P();
	uint8_t op = (uint8_t)(lba >> 8);
	const uint8_t *cdb = buf + NEXT_CMD_CDB;
	switch (op)
	{
	case 0xFF: next_cd_play_init(p); break;
	case 0xFE: next_cd_play_stop(p); break;
	case 0x15: next_cd_play_command(p, cdb, buf, cdb[4]); break;
	default:
		next_cd_play_command(p, cdb, NULL, 0);
		printf("NeXT CD: cmd %02X %02X %02X%02X%02X%02X %02X%02X%02X%02X -> st %d cur %u stop %u\n",
		       cdb[0], cdb[1], cdb[2], cdb[3], cdb[4], cdb[5], cdb[6], cdb[7], cdb[8], cdb[9],
		       p->state, p->cur, p->stop);
		break;
	}
}
