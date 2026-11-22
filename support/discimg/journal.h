// A hard disk image's writes, kept in a save file beside it: an append-only log of checksummed
// sector records, replayed into an index when it is opened. The image itself is never written.

#ifndef DISCIMG_JOURNAL_H
#define DISCIMG_JOURNAL_H

#include <stdint.h>

#define JOURNAL_MAX_SECTORS 32   // per record, and so per discimg_journal_write

struct discimg_journal;

// path: the save file; root: the directory above which its parents need no fsync; size: the
// disk's bytes; identity: the image's SHA1 (type 1) or an MD5 of the file (type 2), held in the
// header so a save is never replayed onto another image. A writable journal creates its file at
// the first write. 0, or -1 with errno.
int  discimg_journal_open(const char *path, const char *root, uint64_t size, const uint8_t identity[20],
                          uint8_t identity_type, bool writable, discimg_journal **journal);
void discimg_journal_close(discimg_journal *j);

// 1 with the sector's last write in data, 0 if it was never written, -1 with errno
int  discimg_journal_read(discimg_journal *j, uint64_t sector, uint8_t data[512]);

// whole sectors, at most JOURNAL_MAX_SECTORS, durable on return. 0, or -1 with errno.
int  discimg_journal_write(discimg_journal *j, uint64_t offset, const void *data, uint32_t length);

#endif
