#ifndef FILES_H
#define FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { ROM_MISSING, ROM_WRONG, ROM_OK } RomStatus;
typedef enum { BIOS_MISSING, BIOS_INVALID, BIOS_OK } BiosStatus;

uint8_t *file_read(const char *path, size_t *size, size_t max);   /* malloc'd */
bool file_write(const char *path, const void *data, size_t size);
bool file_exists(const char *path);
uint32_t crc32_buf(const uint8_t *p, size_t n);

RomStatus rom_check(const char *path, uint32_t *crc_out);
BiosStatus bios_check(const char *path);
char *find_default_rom(void);    /* malloc'd path or NULL */
char *find_default_bios(void);   /* malloc'd path or NULL */
const char *default_rom_name(void);

/* directories (UTF-8 paths) */
typedef void (*DirCallback)(const char *name, bool is_dir, void *user);
bool dir_list(const char *dir, DirCallback cb, void *user);
bool make_dirs(const char *path);

#endif
