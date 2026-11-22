// The save-file format and its recovery rules are those of MiSTer-devel/Main_MiSTer PR 1380's
// chd_hdd_overlay: a 64-byte header ("MiSTCOW1", the disk's bytes, the image identity, CRC32), then
// records of "SECT", the sector count, the first sector, a CRC32 of those 16 bytes, the sectors and
// a CRC32 of the whole record. A torn last record is cut off when the file is next opened writable.

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <new>
#include <string>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

#include "../../lib/miniz/miniz.h"
#include "journal.h"

static const unsigned HEADER_SIZE        = 64;
static const unsigned RECORD_HEADER_SIZE = 20;
static const unsigned RECORD_SIZE        = RECORD_HEADER_SIZE + JOURNAL_MAX_SECTORS * 512 + 4;

struct journal_sector
{
	__off64_t offset;
	uint32_t  crc;
};

struct discimg_journal
{
	int         fd = -1;
	bool        writable = false;
	bool        failed = false;
	uint64_t    size = 0;
	__off64_t   end = HEADER_SIZE;
	uint8_t     header[HEADER_SIZE] = {};
	std::unordered_map<uint64_t, journal_sector> sectors;
	std::string missing;   // the save file, while it does not exist yet
	std::string root;
};

// Journals open on a save file that does not exist yet, in this process. A reader there would not
// see a writer's sectors, so the two exclude each other.
struct missing_users
{
	unsigned readers;
	bool     writer;
};
static std::unordered_map<std::string, missing_users> missing;

static uint64_t get_le(const uint8_t *p, unsigned bytes)
{
	uint64_t value = 0;
	for (unsigned i = 0; i < bytes; ++i)
	{
		value |= (uint64_t)p[i] << (i * 8);
	}
	return value;
}

static void put_le(uint8_t *p, uint64_t value, unsigned bytes)
{
	for (unsigned i = 0; i < bytes; ++i)
	{
		p[i] = (uint8_t)(value >> (i * 8));
	}
}

static int error(int code)
{
	errno = code;
	return -1;
}

static int transfer(int fd, void *data, size_t length, __off64_t offset, bool write)
{
	uint8_t *p = (uint8_t *)data;
	while (length)
	{
		ssize_t n = write ? pwrite64(fd, p, length, offset) : pread64(fd, p, length, offset);
		if (n < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			return -1;
		}
		if (!n)
		{
			return error(EIO);
		}
		p += n;
		offset += n;
		length -= n;
	}
	return 0;
}

static int sync_dir(const std::string &dir)
{
	int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
	{
		return -1;
	}
	int ret = fsync(fd);
	int saved = errno;
	close(fd);
	errno = saved;
	return ret;
}

// fsync each directory from path's parent up to root, so a new file's entry is durable
static int sync_ancestors(const std::string &path, const std::string &root)
{
	std::string dir(path);
	std::string top(root);
	while (top.size() > 1 && top.back() == '/')
	{
		top.pop_back();
	}
	do
	{
		size_t slash = dir.rfind('/');
		dir = slash == std::string::npos ? "." : dir.substr(0, slash ? slash : 1);
		if (sync_dir(dir))
		{
			return -1;
		}
	} while (!top.empty() && dir.size() > top.size() && !dir.compare(0, top.size() + 1, top + "/"));
	return 0;
}

static int replay(discimg_journal *j, __off64_t size)
{
	uint8_t record[RECORD_SIZE];
	while (j->end < size)
	{
		if (size - j->end < RECORD_HEADER_SIZE)
		{
			break;
		}
		if (transfer(j->fd, record, RECORD_HEADER_SIZE, j->end, false))
		{
			return -1;
		}
		if (get_le(record + 16, 4) != mz_crc32(0, record, 16))
		{
			return error(EILSEQ);
		}
		unsigned count = (unsigned)get_le(record + 4, 4);
		uint64_t sector = get_le(record + 8, 8);
		if (memcmp(record, "SECT", 4) || !count || count > JOURNAL_MAX_SECTORS ||
		    sector >= j->size / 512 || count > j->size / 512 - sector)
		{
			return error(EINVAL);
		}
		unsigned bytes = RECORD_HEADER_SIZE + count * 512 + 4;
		if (size - j->end < bytes)
		{
			break;
		}
		if (transfer(j->fd, record + RECORD_HEADER_SIZE, bytes - RECORD_HEADER_SIZE, j->end + RECORD_HEADER_SIZE, false))
		{
			return -1;
		}
		if (get_le(record + bytes - 4, 4) != mz_crc32(0, record, bytes - 4))
		{
			return error(EILSEQ);
		}
		for (unsigned i = 0; i < count; ++i)
		{
			j->sectors[sector + i] = {j->end + RECORD_HEADER_SIZE + i * 512,
			                          (uint32_t)mz_crc32(0, record + RECORD_HEADER_SIZE + i * 512, 512)};
		}
		j->end += bytes;
	}
	if (j->end != size && j->writable)
	{
		if (ftruncate64(j->fd, j->end) || fdatasync(j->fd))
		{
			return -1;
		}
	}
	return 0;
}

static void forget_missing(discimg_journal *j)
{
	if (j->missing.empty())
	{
		return;
	}
	auto m = missing.find(j->missing);
	if (m != missing.end())
	{
		if (j->writable)
		{
			m->second.writer = false;
		}
		else if (m->second.readers)
		{
			m->second.readers--;
		}
		if (!m->second.writer && !m->second.readers)
		{
			missing.erase(m);
		}
	}
	j->missing.clear();
}

void discimg_journal_close(discimg_journal *j)
{
	if (!j)
	{
		return;
	}
	if (j->fd >= 0)
	{
		close(j->fd);
	}
	forget_missing(j);
	delete j;
}

int discimg_journal_open(const char *path, const char *root, uint64_t size, const uint8_t identity[20],
                         uint8_t identity_type, bool writable, discimg_journal **journal)
{
	*journal = nullptr;
	discimg_journal *j = new (std::nothrow) discimg_journal;
	if (!j)
	{
		return error(ENOMEM);
	}
	j->writable = writable;
	j->size = size;
	memcpy(j->header, "MiSTCOW1", 8);
	put_le(j->header + 8, size, 8);
	memcpy(j->header + 16, identity, 20);
	j->header[36] = identity_type;
	put_le(j->header + 60, mz_crc32(0, j->header, 60), 4);

	j->fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (j->fd < 0 && errno == ENOENT)
	{
		try
		{
			auto m = missing.find(path);
			if (m != missing.end() && (writable || m->second.writer))
			{
				delete j;
				return error(EBUSY);
			}
			missing_users &u = missing[path];
			if (writable)
			{
				u.writer = true;
			}
			else
			{
				u.readers++;
			}
			j->missing = path;
			j->root = root ? root : "";
		}
		catch (const std::bad_alloc &)
		{
			discimg_journal_close(j);
			return error(ENOMEM);
		}
		*journal = j;
		return 0;
	}

	int ret = -1;
	if (j->fd >= 0)
	{
		struct stat64 st;
		uint8_t actual[HEADER_SIZE];
		if (flock(j->fd, (writable ? LOCK_EX : LOCK_SH) | LOCK_NB) || fstat64(j->fd, &st))
		{
		}
		else if (!S_ISREG(st.st_mode) || st.st_size < HEADER_SIZE)
		{
			error(EINVAL);
		}
		else if (!transfer(j->fd, actual, sizeof(actual), 0, false))
		{
			if (memcmp(actual, j->header, sizeof(actual)))
			{
				error(EINVAL);
			}
			else
			{
				try
				{
					ret = replay(j, st.st_size);
				}
				catch (const std::bad_alloc &)
				{
					error(ENOMEM);
				}
			}
		}
	}
	if (ret)
	{
		int saved = errno;
		discimg_journal_close(j);
		errno = saved;
		return -1;
	}
	*journal = j;
	return 0;
}

// the save file of a writable journal, at its first write
static int create(discimg_journal *j)
{
	j->fd = open(j->missing.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
	if (j->fd < 0)
	{
		return -1;
	}
	if (flock(j->fd, LOCK_EX | LOCK_NB) || transfer(j->fd, j->header, HEADER_SIZE, 0, true) ||
	    fdatasync(j->fd) || sync_ancestors(j->missing, j->root))
	{
		int saved = errno;
		close(j->fd);
		j->fd = -1;
		unlink(j->missing.c_str());
		errno = saved;
		return -1;
	}
	forget_missing(j);
	return 0;
}

int discimg_journal_read(discimg_journal *j, uint64_t sector, uint8_t data[512])
{
	if (!j)
	{
		return 0;
	}
	auto found = j->sectors.find(sector);
	if (found == j->sectors.end())
	{
		return 0;
	}
	if (transfer(j->fd, data, 512, found->second.offset, false))
	{
		return -1;
	}
	if (mz_crc32(0, data, 512) != found->second.crc)
	{
		return error(EILSEQ);
	}
	return 1;
}

int discimg_journal_write(discimg_journal *j, uint64_t offset, const void *data, uint32_t length)
{
	if (!j || !j->writable)
	{
		return error(EROFS);
	}
	if (j->failed)
	{
		return error(EIO);
	}
	if (!length || offset % 512 || length % 512 || length / 512 > JOURNAL_MAX_SECTORS ||
	    offset > j->size || length > j->size - offset)
	{
		return error(EINVAL);
	}
	if (j->end > LLONG_MAX - RECORD_HEADER_SIZE - 4 - length)
	{
		return error(EFBIG);
	}
	if (j->fd < 0 && create(j))
	{
		j->failed = true;
		return -1;
	}

	unsigned count = length / 512;
	uint64_t sector = offset / 512;
	journal_sector *entries[JOURNAL_MAX_SECTORS];
	bool inserted[JOURNAL_MAX_SECTORS] = {};
	unsigned prepared = 0;
	try
	{
		for (; prepared < count; ++prepared)
		{
			auto entry = j->sectors.emplace(sector + prepared, journal_sector{});
			entries[prepared] = &entry.first->second;
			inserted[prepared] = entry.second;
		}
	}
	catch (const std::bad_alloc &)
	{
		for (unsigned i = 0; i < prepared; ++i)
		{
			if (inserted[i])
			{
				j->sectors.erase(sector + i);
			}
		}
		return error(ENOMEM);
	}

	uint8_t record[RECORD_SIZE];
	memcpy(record, "SECT", 4);
	put_le(record + 4, count, 4);
	put_le(record + 8, sector, 8);
	put_le(record + 16, mz_crc32(0, record, 16), 4);
	memcpy(record + RECORD_HEADER_SIZE, data, length);
	put_le(record + RECORD_HEADER_SIZE + length, mz_crc32(0, record, RECORD_HEADER_SIZE + length), 4);
	if (transfer(j->fd, record, RECORD_HEADER_SIZE + 4 + length, j->end, true) || fdatasync(j->fd))
	{
		int saved = errno;
		for (unsigned i = 0; i < count; ++i)
		{
			if (inserted[i])
			{
				j->sectors.erase(sector + i);
			}
		}
		j->failed = true;
		return error(saved);
	}
	for (unsigned i = 0; i < count; ++i)
	{
		*entries[i] = {j->end + RECORD_HEADER_SIZE + i * 512,
		               (uint32_t)mz_crc32(0, record + RECORD_HEADER_SIZE + i * 512, 512)};
	}
	j->end += RECORD_HEADER_SIZE + 4 + length;
	return 0;
}
