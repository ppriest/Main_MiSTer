// HP 9000 CD-ROM slot: a cdslot (support/discimg/cdslot.h) on hp9k_cdrom_slot().

#include <stdint.h>
#include <string.h>

#include "../discimg/cdslot.h"
#include "hp9k.h"
#include "hp9k_cdrom.h"

static cdslot_t cd = {};

void hp9k_cdrom_unmount(int index)
{
	if (index != hp9k_cdrom_slot()) return;
	cdslot_unmount(&cd);
}

uint64_t hp9k_cdrom_size(int index)
{
	if (!hp9k_cdrom_active(index)) return 0;
	return cdslot_size(&cd);
}

int hp9k_cdrom_active(int index)
{
	return index == hp9k_cdrom_slot() && cd.active;
}

int hp9k_cdrom_mount(int index, const char *name)
{
	if (index != hp9k_cdrom_slot()) return HP9K_CDROM_PASSTHRU;
	return cdslot_mount(&cd, name, "HP9K CD");
}

int hp9k_cdrom_window(int index, uint64_t lba, uint8_t *buf, int sz)
{
	if (index != hp9k_cdrom_slot()) return 0;
	return cdslot_window(&cd, HP9K_CDROM_AUDIO_BLK, HP9K_CDROM_TOC_BLK, lba, buf, sz);
}

void hp9k_cdrom_fill(int index, uint64_t lba, uint8_t *buf, int sz)
{
	if (index != hp9k_cdrom_slot())
	{
		memset(buf, 0, sz);
		return;
	}
	cdslot_fill(&cd, lba, buf, sz);
}
