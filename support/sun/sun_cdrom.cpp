// Sun CD-ROM slot: a cdslot (support/discimg/cdslot.h) on sun_cdrom_slot().

#include <stdint.h>
#include <string.h>

#include "../discimg/cdslot.h"
#include "sun.h"
#include "sun_cdrom.h"

static cdslot_t cd = {};

void sun_cdrom_unmount(int index)
{
	if (index != sun_cdrom_slot()) return;
	cdslot_unmount(&cd);
}

uint64_t sun_cdrom_size(int index)
{
	if (!sun_cdrom_active(index)) return 0;
	return cdslot_size(&cd);
}

int sun_cdrom_active(int index)
{
	return index == sun_cdrom_slot() && cd.active;
}

int sun_cdrom_mount(int index, const char *name)
{
	if (index != sun_cdrom_slot()) return SUN_CDROM_PASSTHRU;
	return cdslot_mount(&cd, name, "Sun CD");
}

int sun_cdrom_window(int index, uint64_t lba, uint8_t *buf, int sz)
{
	if (index != sun_cdrom_slot()) return 0;
	return cdslot_window(&cd, SUN_CDROM_AUDIO_BLK, SUN_CDROM_TOC_BLK, lba, buf, sz);
}

void sun_cdrom_fill(int index, uint64_t lba, uint8_t *buf, int sz)
{
	if (index != sun_cdrom_slot())
	{
		memset(buf, 0, sz);
		return;
	}
	cdslot_fill(&cd, lba, buf, sz);
}
