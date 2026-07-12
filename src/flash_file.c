/*
 * flash_file.c — File-based persistence for the Octopus Linux port.
 *
 * Replaces the in-memory flash buffer with file-backed storage.
 * The firmware's flash-block.c uses flash_read/flash_erase/flash_program
 * which are already stubbed to in-memory operations in hal_linux.c.
 *
 * This module adds load/save to disk, so state persists across restarts.
 * The file format is a flat binary image of the in-memory flash buffer.
 */

#include "hal_linux.h"

/* The in-memory flash buffer from hal_linux.c */
extern unsigned char *hal_flash_base;

/* Flash buffer size (must match hal_linux.c) */
#define HAL_FLASH_SIZE  (1024 * 1024)

/* Initialize from file — load state into the in-memory flash buffer */
void flash_file_init(const char *filepath) {
    if (!filepath || !hal_flash_base) return;

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "flash_file: no state file '%s', starting fresh\n", filepath);
        return;
    }

    size_t n = fread(hal_flash_base, 1, HAL_FLASH_SIZE, f);
    fclose(f);
    fprintf(stderr, "flash_file: loaded %zu bytes from '%s'\n", n, filepath);
}

/* Save the in-memory flash buffer to file */
void flash_file_save(const char *filepath) {
    if (!filepath || !hal_flash_base) return;

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        fprintf(stderr, "flash_file: cannot write '%s': %s\n", filepath, strerror(errno));
        return;
    }

    size_t n = fwrite(hal_flash_base, 1, HAL_FLASH_SIZE, f);
    fclose(f);
    fprintf(stderr, "flash_file: saved %zu bytes to '%s'\n", n, filepath);
}

/* Load a raw PersistentV2 binary (from hardware sysex dump or factory restore) */
void flash_file_load(const char *filepath) {
    flash_file_init(filepath);
}

/* Import a bank file — wrapper for OSC /octopus/load */
void flash_file_import_bank(const char *filepath) {
    flash_file_init(filepath);
}
