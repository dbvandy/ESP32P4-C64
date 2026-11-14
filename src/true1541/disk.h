/* 1541 D64 surface as GCR bit rings (adapted from ChronoCoders/C64, MIT). */
#ifndef TRUE1541_DISK_H
#define TRUE1541_DISK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISK_TRACKS 35u
#define DISK_TOTAL_SECTORS 683u
#define D64_STD_SIZE 174848u
#define D64_ERR_SIZE (D64_STD_SIZE + DISK_TOTAL_SECTORS)

unsigned disk_sectors_in_track(unsigned track);
unsigned disk_zone_of_track(unsigned track);
unsigned disk_zone_byte_cycles(unsigned zone);
unsigned disk_track_byte_cycles(unsigned track);

bool disk_psram_init(void);
bool disk_mount(const char *path);
bool disk_mount_image(const uint8_t *data, size_t len);
void disk_unmount(void);
bool disk_present(void);

const uint8_t *disk_track_gcr(unsigned track, unsigned *nbits);
void gcr_encode(const uint8_t *in, unsigned in_len, uint8_t *out);
bool gcr_decode(const uint8_t *in, unsigned in_len, uint8_t *out);
uint8_t disk_header_checksum(uint8_t sector, uint8_t track, uint8_t id2,
                             uint8_t id1);
uint8_t disk_data_checksum(const uint8_t *data256);
void disk_write_gcr_byte(unsigned track, unsigned bit_index, uint8_t value);
bool disk_read_sector(unsigned track, unsigned sector, uint8_t out[256]);
bool disk_image_read_sector(unsigned track, unsigned sector, uint8_t out[256]);
bool disk_image_write_sector(unsigned track, unsigned sector,
                             const uint8_t data[256]);
void disk_get_id(uint8_t *id1, uint8_t *id2);
bool disk_is_dirty(void);
void disk_mark_clean(void);
const uint8_t *disk_image_bytes(size_t *len_out);
bool disk_writeback(void);
// Pull any live-GCR sector writes back into the D64 image[] before flush.
void disk_sync_image_from_gcr(void);

#ifdef __cplusplus
}
#endif

#endif
