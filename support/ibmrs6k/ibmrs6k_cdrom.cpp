// IBM RS/6000 43P 7248 CD-ROM slot: a cdslot (support/discimg/cdslot.h) on ibmrs6k_cdrom_slot().

#include <stdint.h>
#include <string.h>

#include "../discimg/cdslot.h"
#include "ibmrs6k.h"
#include "ibmrs6k_cdrom.h"

static cdslot_t cd = {};

void ibmrs6k_cdrom_unmount(int index)
{
	if (index != ibmrs6k_cdrom_slot()) return;
	cdslot_unmount(&cd);
}

uint64_t ibmrs6k_cdrom_size(int index)
{
	if (!ibmrs6k_cdrom_active(index)) return 0;
	return cdslot_size(&cd);
}

int ibmrs6k_cdrom_active(int index)
{
	return index == ibmrs6k_cdrom_slot() && cd.active;
}

int ibmrs6k_cdrom_mount(int index, const char *name)
{
	if (index != ibmrs6k_cdrom_slot()) return IBMRS6K_CDROM_PASSTHRU;
	return cdslot_mount(&cd, name, "IBMRS6K CD");
}

int ibmrs6k_cdrom_window(int index, uint64_t lba, uint8_t *buf, int sz)
{
	if (index != ibmrs6k_cdrom_slot()) return 0;
	return cdslot_window(&cd, IBMRS6K_CDROM_AUDIO_BLK, IBMRS6K_CDROM_TOC_BLK, lba, buf, sz);
}

void ibmrs6k_cdrom_fill(int index, uint64_t lba, uint8_t *buf, int sz)
{
	if (index != ibmrs6k_cdrom_slot())
	{
		memset(buf, 0, sz);
		return;
	}
	cdslot_fill(&cd, lba, buf, sz);
}
