// Disc and disk images for cores whose drives Main serves as translated images rather than as the
// file itself: a CD as a CHD, a .cue with its BINARY files, or a single raw data track (2048, 2336
// or 2352 bytes a sector, told apart by its ISO 9660 / High Sierra volume descriptor); a DVD or a
// hard disk as a CHD. The core-specific part (which slot, what layout the core reads) stays in the
// core's support directory. cdslot.cpp is the CD-ROM slot sun, hp9k and ibmrs6k share;
// support/firebeat/firebeat.cpp shows a core with its own layout.

#ifndef DISCIMG_H
#define DISCIMG_H

#include <stdint.h>
#include "../../cd.h"

enum discimg_kind_t
{
	DISCIMG_NONE,
	DISCIMG_CD,      // toc holds the tracks
	DISCIMG_DVD,     // bytes of 2048-byte sectors (chdman createdvd)
	DISCIMG_HDD,     // bytes of sectors, geometry from the GDDD metadata (chdman createhd)
};

enum discimg_open_t
{
	DISCIMG_OPENED = 0,
	DISCIMG_FLAT   = 1, // a plain 2048-byte-sector file: left closed for the generic path; bytes is its size
	DISCIMG_REJECT = 2, // unreadable or unsupported, the reason logged
};

// discimg_open flags
#define DISCIMG_FLAT_TRACK 1   // a flat file opens as a MODE1/2048 track instead of returning DISCIMG_FLAT
#define DISCIMG_JOURNAL    2   // a hard disk CHD reads through the writes saved for it (journal.h)
#define DISCIMG_WRITABLE   4   // DISCIMG_JOURNAL, and a hard disk CHD takes writes into it; others open read-only

struct discimg_journal;

struct discimg_t
{
	discimg_kind_t kind;
	int       is_chd;
	toc_t     toc;          // CD: disc LBAs (no +150); a track's frame lba is lba + offset in its file or the CHD
	uint8_t   session[100]; // CD: each track's session, 1 unless a .cue's REM SESSION says otherwise
	uint8_t  *hunkbuf;      // CHD: one hunk
	int       hunknum;      // the hunk in hunkbuf, or -1
	uint64_t  bytes;        // DVD, HDD: the CHD's logical bytes; FLAT: the file's size
	uint32_t  cyls, heads, secs, bps;   // HDD
	discimg_journal *journal;           // HDD opened with DISCIMG_JOURNAL or DISCIMG_WRITABLE
	int       writable;                 // HDD opened with DISCIMG_WRITABLE
	const char *tag;        // log prefix
};

// .chd: a CD, DVD or hard disk; .cue: its tracks; anything else: a single raw data track.
// Close with discimg_close whatever it returns.
int  discimg_open(discimg_t *d, const char *name, const char *tag, int flags = 0);
void discimg_close(discimg_t *d);

// the first track that is not audio, or -1
int  discimg_data_track(const discimg_t *d);

// the user data's offset in a track's stored sector: 0, 8 (MODE2/2336), 16 (raw MODE1), 24 (raw MODE2)
uint32_t discimg_data_offset(const cd_track_t *k);

// bytes [off, off + n) of disc LBA lba of track trk, as the track stores its sector (sector_size
// bytes; CHD audio byte-swapped). 0 and dst zeroed on a read error.
int  discimg_read(discimg_t *d, int trk, int lba, uint32_t off, int n, uint8_t *dst);

// disc LBA lba of track trk as the 2352 bytes of a .bin: audio little-endian; a 2048- or 2336-byte
// sector inside the sync and header it was stored without (EDC and ECC left zero).
int  discimg_read_frame(discimg_t *d, int trk, int lba, uint8_t *frame);

// DVD, HDD: bytes [pos, pos + n), a hard disk's saved writes included. 0 and dst zeroed on a read
// error or past the end.
int  discimg_read_bytes(discimg_t *d, uint64_t pos, int n, uint8_t *dst);

// a writable HDD: whole 512-byte sectors [pos, pos + n) into its journal, durable on return. 0 on
// an error, errno set.
int  discimg_write_bytes(discimg_t *d, uint64_t pos, uint32_t n, const uint8_t *src);

#endif
