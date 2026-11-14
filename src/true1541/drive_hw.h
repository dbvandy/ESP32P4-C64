#ifndef TRUE1541_DRIVE_HW_H
#define TRUE1541_DRIVE_HW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRIVE_HZ 1000000u
#define C64_PHI2_HZ 985248u
#define IEC_CLK 0x01u
#define IEC_DATA 0x02u
#define IEC_ATN 0x04u

void drive_init(void);
void drive_reset(void);
bool drive_load_rom(const char *path);
bool drive_present(void);
void drive_tick(void);
void drive_run_phi2(uint32_t phi2_cycles);

uint8_t drive_iec_out(void);
void drive_set_iec_ext(uint8_t mask);
void drive_bus_poke(uint16_t addr, uint8_t val);
uint8_t drive_ram_peek(uint16_t addr);
bool drive_activity(void); // VIA2 motor or activity LED on
bool drive_activity_led(void); // VIA2 PB3 activity LED only
bool drive_motor_on(void);
void drive_force_idle(void); // clear motor + activity LED (post-spinUp)

#ifdef __cplusplus
}
#endif

#endif
