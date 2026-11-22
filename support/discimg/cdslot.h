// A CD-ROM slot served as a flat disc of its data track's 2048-byte sectors, plus two windows past
// the data: the track table at toc_blk and CD-audio frame f (2352 + 208 bytes) at audio_blk + 5f.
// The cores that use it (Sun, HP 9000, IBM RS/6000) wrap one cdslot_t each.

#ifndef CDSLOT_H
#define CDSLOT_H

#include <stdint.h>
#include "discimg.h"

enum
{
	CDSLOT_PASSTHRU = 0, // a flat image: the generic path serves it
	CDSLOT_HANDLED  = 1, // cdslot_fill serves it
	CDSLOT_REJECT   = 2, // fail the mount
};

struct cdslot_t
{
	int       active;
	discimg_t img;
	int       trk;            // the data track, or -1
	uint32_t  data_soff;      // user data offset in its stored sector
	uint64_t  sectors;
	uint32_t  flat_sectors;   // a flat image on the generic path, for the TOC window (0: none)
	int64_t   aframe_lba;     // the audio frame in aframe (-1: none)
	uint8_t   aframe[2352];
};

int      cdslot_mount(cdslot_t *s, const char *name, const char *tag);
void     cdslot_unmount(cdslot_t *s);
uint64_t cdslot_size(const cdslot_t *s);

// lba in 512-byte units; sz a multiple of 512, up to UIO_BUFFER_SIZE.
void     cdslot_fill(cdslot_t *s, uint64_t lba, uint8_t *buf, int sz);

// 1 (buf filled) when lba is in the TOC or audio window, else 0.
int      cdslot_window(cdslot_t *s, uint64_t audio_blk, uint64_t toc_blk, uint64_t lba, uint8_t *buf, int sz);

#endif
