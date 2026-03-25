/*
 * DataStream binary-compatible field accessors for NecroDancer (arm64 Mach-O).
 *
 * Field offsets derived from decompilation of DataStream methods.
 * See: necrodancer/decompiled/DataStream.c
 *
 * Layout (0x80 bytes total):
 *   0x00-0x10: std::vector<char> read buffer (start/end/capacity)
 *   0x18:      position (read cursor)
 *   0x20:      file_size (set by openInFile via fseek)
 *   0x28:      FILE* handle
 *   0x30-0x40: std::vector<char> write buffer
 *   0x48-0x5f: std::string filename (Apple alternate SSO, 24 bytes)
 *   0x60-0x6f: shared_ptr (lock)
 *   0x78:      flags byte (0x01 = has data, cleared after preload)
 */

#ifndef NECRODANCER_DATASTREAM_H
#define NECRODANCER_DATASTREAM_H

#include <stdint.h>
#include <stdio.h>

/* Read buffer vector (std::vector<char>) */
#define DS_BUF_START(ds)    (*(char **)     ((char *)(ds) + 0x00))
#define DS_BUF_END(ds)      (*(char **)     ((char *)(ds) + 0x08))
#define DS_BUF_CAP(ds)      (*(char **)     ((char *)(ds) + 0x10))

/* Stream state */
#define DS_POSITION(ds)     (*(uint64_t *)  ((char *)(ds) + 0x18))
#define DS_FILE_SIZE(ds)    (*(uint64_t *)  ((char *)(ds) + 0x20))
#define DS_FILE_PTR(ds)     (*(FILE **)     ((char *)(ds) + 0x28))

/* Write buffer vector */
#define DS_WBUF_START(ds)   (*(char **)     ((char *)(ds) + 0x30))
#define DS_WBUF_END(ds)     (*(char **)     ((char *)(ds) + 0x38))
#define DS_WBUF_CAP(ds)     (*(char **)     ((char *)(ds) + 0x40))

/* Filename (apple_string_t at +0x48) */
#define DS_FILENAME(ds)     ((void *)       ((char *)(ds) + 0x48))

/* Flags */
#define DS_FLAGS(ds)        (*(uint8_t *)   ((char *)(ds) + 0x78))

#endif /* NECRODANCER_DATASTREAM_H */
