#include "drive_hw.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "disk.h"
#include "via.h"
#include "cpu6502_drive.h"
#include "../roms/1541.h"

// Clock domains: the drive runs at 1.0 MHz, the C64 at PAL phi2 985248 Hz. They
// are asynchronous; drive_run_phi2 scales elapsed C64 cycles into drive cycles
// with an integer accumulator so the ratio is exact over time without float.
// DRIVE_HZ / C64_PHI2_HZ are in drive.h so the tests assert against one definition.

// Address decoding (1541, partial): 2 KB RAM mirrors across $0000-$17FF; VIA1
// mirrors every 16 bytes in $1800-$1BFF, VIA2 in $1C00-$1FFF; the 16 KB ROM is
// at $C000-$FFFF; $2000-$BFFF is unmapped (open-bus stub, reads 0).
#define DRIVE_RAM_SIZE 0x0800u
#define DRIVE_ROM_SIZE 0x4000u

// IEC lines pulled low by an external device (the C64 in Phase 6c); the drive
// composes its own drive with these. The bit convention matches the C64 side's
// IEC_PULL_CLK/DATA/ATN (cia.h) so the shared wired-AND bus passes masks straight
// through. Zero when nothing is attached, so the open-collector lines idle high.
#define IEC_CLK 0x01u
#define IEC_DATA 0x02u
#define IEC_ATN 0x04u

// VIA1 port B ($1800), the serial bus (1541 schematic): PB0 DATA in, PB1 DATA
// out, PB2 CLK in, PB3 CLK out, PB4 ATNA out, PB5/PB6 device-number jumpers, PB7
// ATN in. A driven OUT bit pulls its line low (open-collector). The serial IN
// lines pass through inverting buffers, so PB0/PB2/PB7 read 1 when their line is
// low; ATN also reaches CA1 (inverted), where the DOS interrupts on the rising
// edge (ATN going low). Intact device jumpers ground PB5/PB6, so device 8 reads
// PB5=PB6=0; cutting a jumper raises the bit and the device number.
#define V1_DATA_OUT 0x02u
#define V1_CLK_OUT 0x08u
#define V1_ATNA 0x10u     // PB4 ATN acknowledge
#define V1_JUMPERS 0x60u  // PB5,PB6 device-number jumpers

static CPU6502 drive;
static uint8_t drive_ram[DRIVE_RAM_SIZE];
static const uint8_t *drive_rom = rom_1541; // flash/const — no 16KB BSS copy
static VIA6522 via1;  // $1800 serial bus
static VIA6522 via2;  // $1C00 mechanism
static uint8_t iec_ext;      // external pulls (Phase 6c); 0 here
static int head_halftrack;   // stepper position, no surface yet (Phase 6d)
static uint8_t step_phase;   // last VIA2 PB0-1 stepper phase
static bool rom_loaded;
static uint64_t cycle_count;
static uint64_t phi2_acc;

// Disk read head (Phase 6d). As the disk turns, the head sees the track's GCR bit
// ring; the drive shifts bits in and, every eight bit-cells, BYTE READY asserts:
// it pulls VIA2 CA1 low and the CPU SO line low so the DOS read loop (BVC on the
// overflow flag) takes the byte from VIA2 PA. SYNC is a run of >=10 one-bits, wired
// to VIA2 PB7 (active low). The bit-cell rate is per zone. Sources: 1541 schematic
// read electronics (UE7 bit counter, UD3 shift register) and the disk format.
static unsigned head_bit;      // bit index into the current track's GCR ring
static unsigned bitcell_acc;   // bit-cell accumulator, nbits per 200000-cycle rev
static uint32_t read_shift;    // bits shifted in, for byte assembly and SYNC
static unsigned ones_run;      // consecutive one-bits seen (SYNC when >= 10)
static unsigned bit_in_byte;   // bits accumulated toward the current GCR byte
static bool sync_active;        // head is currently over a SYNC mark
static uint8_t read_byte;        // last assembled GCR byte, presented on VIA2 PA
static bool byte_ready;          // BYTE READY asserted on this drive cycle
static bool write_prev;          // head was in write mode on the previous step

void drive_set_halftrack(int halftrack);  // defined below; used by job trap

// A DATA or CLK line is low if the drive's OUT pin state is high (driven high, or
// input floating into the 7406) or the external bus pulls it; ATN is driven only
// by the controller. The serial IN lines are
// inverted (PB0/PB2/PB7 read 1 when their line is low), and the device jumpers
// ground PB5/PB6 (device 8). Source: 1541 schematic serial-bus wiring.
static void compose_via1_ports(void) {
    // ChronoCoders/1541: float-high into 7406 pulls the line; IN bits are inverted.
    uint8_t out = (uint8_t)((via1.orb & via1.ddrb) | (uint8_t)~via1.ddrb);
    bool data_low = ((out & V1_DATA_OUT) != 0) || (iec_ext & IEC_DATA);
    bool clk_low = ((out & V1_CLK_OUT) != 0) || (iec_ext & IEC_CLK);
    bool atn_low = (iec_ext & IEC_ATN) != 0;
    uint8_t pb = 0x00u;
    if (data_low) { pb |= 0x01u; }  // PB0 DATA in (inverted: 1 when line low)
    if (clk_low) { pb |= 0x04u; }   // PB2 CLK in (inverted)
    if (atn_low) { pb |= 0x80u; }   // PB7 ATN in (inverted)
    pb = (uint8_t)(pb & ~V1_JUMPERS);  // jumpers grounded low = default device 8
    via1.pb_in = pb;
}

// VIA2 inputs. PB7 is SYNC (active low: 0 while the head is over a SYNC mark), PB4
// is write protect (high = not protected; reads are always allowed). Port A carries
// the GCR byte last assembled off the surface. With no disk the port reads idle
// high and SYNC stays inactive. Source: 1541 schematic VIA2 wiring.
static void compose_via2_ports(void) {
    uint8_t pb = 0xFFu;
    if (sync_active) { pb &= (uint8_t)~0x80u; }  // PB7 SYNC, active low
    via2.pb_in = pb;
    via2.pa_in = disk_present() ? read_byte : 0xFFu;
}

// Decode the VIA2 stepper: cycling PB0-1 through the phase sequence walks the
// head one half-track per phase step, forward or back. Source: 1541 schematic
// stepper wiring.
//
// head_bit is a bit index into the current track's GCR ring, but the ring length
// (bits per revolution) differs by density zone. The disk keeps turning during a
// radial seek, so the angular position is physically continuous across it: on a
// track change that crosses a zone boundary we rescale head_bit by the length
// ratio so the same angle keeps the same fraction of a revolution. Within a zone
// the ratio is 1 (no-op). Round-to-nearest; the read loop re-frames on the next
// SYNC so any sub-bit rounding cannot misalign a sector.
static void update_stepper(void) {
    uint8_t phase = (uint8_t)(via2.orb & via2.ddrb & 0x03u);
    uint8_t diff = (uint8_t)((phase - step_phase) & 0x03u);
    int old_halftrack = head_halftrack;
    if (diff == 1) {
        if (head_halftrack < 83) { head_halftrack++; }
    } else if (diff == 3) {
        if (head_halftrack > 0) { head_halftrack--; }
    }
    step_phase = phase;
    unsigned old_track = (unsigned)(old_halftrack / 2) + 1u;
    unsigned new_track = (unsigned)(head_halftrack / 2) + 1u;
    if (new_track != old_track) {
        unsigned old_nbits = 0, new_nbits = 0;
        disk_track_gcr(old_track, &old_nbits);
        disk_track_gcr(new_track, &new_nbits);
        if (old_nbits != 0u && new_nbits != 0u && old_nbits != new_nbits) {
            head_bit = (unsigned)(((uint64_t)head_bit * new_nbits + old_nbits / 2u) /
                                  old_nbits);
            if (head_bit >= new_nbits) { head_bit = new_nbits - 1u; }
        }
    }
}

static uint8_t drive_read(void *ctx, uint16_t addr) {
    (void)ctx;
    if (addr >= 0xC000) {
        return drive_rom[addr - 0xC000];
    }
    if (addr >= 0x1C00 && addr < 0x2000) {
        compose_via2_ports();
        return via_read(&via2, (uint8_t)(addr & 0x0Fu));
    }
    if (addr >= 0x1800 && addr < 0x1C00) {
        compose_via1_ports();
        return via_read(&via1, (uint8_t)(addr & 0x0Fu));
    }
    if (addr < 0x1800) {
        return drive_ram[addr & (DRIVE_RAM_SIZE - 1)];
    }
    return 0x00;  // $2000-$BFFF unmapped
}

static void service_one_job(unsigned i);

static void drive_write(void *ctx, uint16_t addr, uint8_t val) {
    (void)ctx;
    if (addr < 0x1800) {
        uint16_t a = (uint16_t)(addr & (DRIVE_RAM_SIZE - 1));
        drive_ram[a] = val;
        // Complete sector jobs in the same write that posts them so the FDC
        // IRQ never starts a live GCR read (which was overwriting our $01 with
        // read errors and causing the 1s motor/LED retry loop).
        if (a < 5u && (val & 0x80u) != 0u) {
            service_one_job(a);
        }
        return;
    }
    if (addr >= 0x1800 && addr < 0x1C00) {
        uint8_t r = (uint8_t)(addr & 0x0Fu);
        via_write(&via1, r, val);
        return;
    }
    if (addr >= 0x1C00 && addr < 0x2000) {
        via_write(&via2, (uint8_t)(addr & 0x0Fu), val);
        update_stepper();  // a port B write may have cycled the stepper phase
        return;
    }
    // ROM and unmapped: writes ignored.
}

void drive_bus_poke(uint16_t addr, uint8_t val) {
    drive_write(NULL, addr, val);
}

// Read-head state clears on init/reset; the disk (if any) is left mounted.
static void reset_read_state(void) {
    head_bit = 0;
    bitcell_acc = 0;
    read_shift = 0;
    ones_run = 0;
    bit_in_byte = 0;
    sync_active = false;
    read_byte = 0xFFu;
    byte_ready = false;
    write_prev = false;
}

void drive_init(void) {
    memset(drive_ram, 0, sizeof(drive_ram));
    via_reset(&via1);
    via_reset(&via2);
    iec_ext = 0;
    head_halftrack = 17 * 2;
    step_phase = 0;
    drive_rom = rom_1541;
    rom_loaded = true;
    cycle_count = 0;
    phi2_acc = 0;
    reset_read_state();
    cpu6502_init(&drive, NULL, drive_read, drive_write);
}

void drive_reset(void) {
    via_reset(&via1);
    via_reset(&via2);
    step_phase = 0;
    cycle_count = 0;
    phi2_acc = 0;
    reset_read_state();
    // Clear "last loaded" so LOAD"*" means first file (not a stale T/S).
    drive_ram[0x7E] = 0;                                    // PRGTRK
    drive_ram[0x26E & (DRIVE_RAM_SIZE - 1)] = 0;             // PRGDRV
    drive_ram[0x26F & (DRIVE_RAM_SIZE - 1)] = 0;             // PRGSEC
    cpu6502_reset(&drive);  // PC from $FFFC/$FFFD in the DOS ROM
}

bool drive_load_rom(const char *path) {
    (void)path;
    drive_rom = rom_1541;
    rom_loaded = true;
    return true;
}

bool drive_present(void) { return rom_loaded; }

// Fire BYTE READY for this cycle: latch nothing more, just pull VIA2 CA1 and the
// CPU SO line low for one drive cycle (released at the top of the next step).
static void byte_ready_pulse(void) {
    byte_ready = true;
    drive.so_line = 0;            // negative edge on SO sets V (the DOS branches on it)
    via_set_ca1(&via2, false);    // BYTE READY low -> VIA2 CA1 edge
}

// Advance the head by the bits that elapse in one drive (1 MHz) cycle. A bit-cell
// lasts (byte_cycles*2) periods of the 16 MHz clock; one drive cycle is 16 of those,
// so the accumulator advances the head at the zone's exact bit rate with integer
// arithmetic. The disk turns only while the motor (VIA2 PB2) runs. VIA2 Port A drives
// the head: with DDRA all-outputs the DOS is writing, so each byte the DOS put in the
// port is laid into the ring; otherwise the head reads, assembling bytes and framing
// on SYNC. Either way BYTE READY fires every eight bit-cells to pace the DOS.
static bool live_gcr_needed(void) {
    // Only $D0/$E0 (execute-in-buffer) still need the bit ring; sector jobs are
    // trapped. Running GCR every drive cycle with the motor on starves the C64
    // (no keypresses while the activity light is on).
    for (unsigned i = 0; i < 5u; i++) {
        uint8_t job = drive_ram[i];
        if ((job & 0x80u) == 0u) { continue; }
        uint8_t code = (uint8_t)(job & 0xF0u);
        if (code == 0xD0u || code == 0xE0u) { return true; }
    }
    return false;
}

static void disk_head_step(void) {
    if (byte_ready) {                     // release the previous cycle's pulse
        byte_ready = false;
        drive.so_line = 1;
        via_set_ca1(&via2, true);
    }
    bool motor_on = (via2.orb & via2.ddrb & 0x04u) != 0;
    if (!disk_present() || !motor_on || !live_gcr_needed()) {
        sync_active = false;
        write_prev = false;
        return;
    }
    unsigned track = (unsigned)(head_halftrack / 2) + 1u;
    unsigned nbits = 0;
    const uint8_t *ring = disk_track_gcr(track, &nbits);
    if (ring == NULL || nbits == 0u) {    // half-track gap or beyond track 35
        sync_active = false;
        write_prev = false;
        return;
    }
    bool write_mode = (via2.ddra == 0xFFu);  // Port A all-outputs: the DOS is writing
    if (write_mode && !write_prev) { bit_in_byte = 0; }  // fresh byte framing on entry
    write_prev = write_mode;
    // nbits bit-cells per exactly 200000 drive cycles: every zone's revolution
    // is exactly 200000 cycles, so the head angle never drifts.
    bitcell_acc += nbits;
    while (bitcell_acc >= 200000u) {
        bitcell_acc -= 200000u;
        head_bit++;
        if (head_bit >= nbits) { head_bit = 0; }
        if (write_mode) {
            sync_active = false;
            ones_run = 0;
            if (++bit_in_byte >= 8u) {     // a whole byte has clocked out of the port
                bit_in_byte = 0;
                unsigned start = (head_bit + nbits - 8u) % nbits;
                disk_write_gcr_byte(track, start, via2.ora);
                byte_ready_pulse();
            }
            continue;
        }
        unsigned prev = (head_bit == 0u) ? (nbits - 1u) : (head_bit - 1u);
        unsigned bit = (ring[prev >> 3] >> (7u - (prev & 7u))) & 1u;
        read_shift = (read_shift << 1) | bit;
        if (bit) { ones_run++; } else { ones_run = 0; }
        if (ones_run >= 10u) {
            sync_active = true;
            bit_in_byte = 0;
        } else {
            if (sync_active) { sync_active = false; bit_in_byte = 1u; }
            else { bit_in_byte++; }
            if (bit_in_byte >= 8u) {
                bit_in_byte = 0;
                read_byte = (uint8_t)(read_shift & 0xFFu);
                byte_ready_pulse();
            }
        }
    }
}

// 1541 DOS job queue ($00-$04): bit7 set = pending. Track/sector at $06+2i,
// buffer at $0300+i*$100. Live GCR BYTE READY / SO timing stalls READ under
// burst; complete sector jobs from the D64 image instead. Leave $D0/$E0
// (execute-in-buffer) for the real 6502 path (copy-protection).
static void service_one_job(unsigned i) {
    if (i >= 5u) { return; }
    uint8_t job = drive_ram[i];
    if ((job & 0x80u) == 0u) { return; }
    uint8_t code = (uint8_t)(job & 0xF0u);
    if (code == 0xD0u || code == 0xE0u) { return; }

    if (!disk_present()) {
        drive_ram[i] = 0x0Fu;  // drive not ready
        return;
    }

    uint8_t track = drive_ram[0x06u + i * 2u];
    uint8_t sector = drive_ram[0x07u + i * 2u];
    uint8_t *buf = &drive_ram[0x0300u + i * 0x100u];

    switch (code) {
    case 0x80u:  // READ
        if (track >= 1u && track <= DISK_TRACKS) {
            drive_set_halftrack((int)(track - 1u) * 2);
        }
        if (disk_image_read_sector(track, sector, buf)) {
            uint8_t id1 = 0, id2 = 0;
            disk_get_id(&id1, &id2);
            drive_ram[0x16] = id1;
            drive_ram[0x17] = id2;
            drive_ram[0x18] = track;
            drive_ram[0x19] = sector;
            drive_ram[0x12] = id1;
            drive_ram[0x13] = id2;
            // Do not touch BP at $99+buf — real job $80 leaves it alone.
            // OPEN "#" sets BP=1; U1 ROM sets BP=0 after the read. Forcing 0
            // here diverged from a real 1541 when callers rely on OPEN "#".
            drive_ram[i] = 0x01u;
        } else {
            drive_ram[i] = 0x04u;
        }
        break;
    case 0x90u:  // WRITE
        if (track >= 1u && track <= DISK_TRACKS) {
            drive_set_halftrack((int)(track - 1u) * 2);
        }
        drive_ram[i] = disk_image_write_sector(track, sector, buf) ? 0x01u : 0x03u;
        break;
    case 0xA0u:  // VERIFY
        drive_ram[i] = disk_image_read_sector(track, sector, buf) ? 0x01u : 0x05u;
        break;
    case 0xB0u:  // SEEK (find header) — updates HEADER + DSKID
    case 0xF0u:  // read header only
        if (track >= 1u && track <= DISK_TRACKS) {
            drive_set_halftrack((int)(track - 1u) * 2);
        }
        {
            uint8_t id1 = 0, id2 = 0;
            disk_get_id(&id1, &id2);
            drive_ram[0x16] = id1;
            drive_ram[0x17] = id2;
            drive_ram[0x18] = track;
            drive_ram[0x19] = sector;
            drive_ram[0x1A] = (uint8_t)(id1 ^ id2 ^ track ^ sector);
            drive_ram[0x12] = id1;
            drive_ram[0x13] = id2;
        }
        drive_ram[i] = 0x01u;
        break;
    case 0xC0u:  // BUMP (recalibrate to track 1)
        drive_set_halftrack(0);
        drive_ram[i] = 0x01u;
        break;
    default:
        break;
    }
}

static void service_job_queue(void) {
    for (unsigned i = 0; i < 5u; i++) {
        service_one_job(i);
    }
}

void drive_tick(void) {
    service_job_queue();  // backup if a job was posted without a plain store
    disk_head_step();  // the surface under the head, ahead of sampling this cycle
    // ATN reaches VIA1 CA1 inverted: CA1 is high when ATN is asserted (the C64
    // pulls it low), so the C64 asserting ATN is a rising edge, which the DOS
    // interrupts on to leave its idle loop.
    via_set_ca1(&via1, (iec_ext & IEC_ATN) != 0);
    via_step(&via1);
    via_step(&via2);
    drive.irq_line = (via_irq(&via1) || via_irq(&via2)) ? 0 : 1;  // active-low
    drive.nmi_line = 1;
    cpu6502_tick(&drive);
    cycle_count++;
}

void drive_run_phi2(uint32_t phi2_cycles) {
    if (!rom_loaded) {
        return;  // no drive attached: the C64 runs alone
    }
    // drive cycles = phi2 cycles * DRIVE_HZ / C64_PHI2_HZ; keep the remainder in
    // phi2_acc (units of C64_PHI2_HZ) so the ratio is exact over time.
    phi2_acc += (uint64_t)phi2_cycles * DRIVE_HZ;
    uint64_t ticks = phi2_acc / C64_PHI2_HZ;
    phi2_acc %= C64_PHI2_HZ;
    for (uint64_t i = 0; i < ticks; i++) {
        drive_tick();
    }
}

const CPU6502 *drive_core(void) { return &drive; }

uint64_t drive_cycles(void) { return cycle_count; }

uint8_t drive_ram_peek(uint16_t addr) { return drive_ram[addr & (DRIVE_RAM_SIZE - 1)]; }

static VIA6522 *via_of(unsigned n) { return (n == 2) ? &via2 : &via1; }

uint8_t drive_via_orb(unsigned n) { return via_of(n)->orb; }
uint8_t drive_via_ddrb(unsigned n) { return via_of(n)->ddrb; }
uint8_t drive_via_ier(unsigned n) { return via_of(n)->ier; }
uint8_t drive_via_ifr(unsigned n) { return via_of(n)->ifr; }
uint8_t drive_via_pcr(unsigned n) { return via_of(n)->pcr; }

uint8_t drive_via_pb(unsigned n) {
    if (n == 1) { compose_via1_ports(); } else { compose_via2_ports(); }
    VIA6522 *v = via_of(n);
    return (uint8_t)((v->orb & v->ddrb) | (v->pb_in & (uint8_t)~v->ddrb));
}

int drive_head_halftrack(void) { return head_halftrack; }

unsigned drive_head_bit(void) { return head_bit; }
bool drive_sync(void) { return sync_active; }
uint8_t drive_read_byte(void) { return read_byte; }
void drive_set_halftrack(int halftrack) {
    if (halftrack < 0) { halftrack = 0; }
    if (halftrack > 83) { halftrack = 83; }
    head_halftrack = halftrack;
    head_bit = 0;
}

// The lines the drive pulls low, in the shared IEC_PULL_* convention: DATA out
// (VIA1 PB1) and CLK out (PB3), plus the ATN acknowledge hardware, which pulls
// DATA low when ATN is asserted and the drive has not yet acknowledged via ATNA
// (PB4). The drive never drives ATN. Source: 1541 schematic serial-bus wiring.
uint8_t drive_iec_out(void) {
    // Match ChronoCoders: float-high pulls; ATN auto-ack only while ATNA clear.
    uint8_t out = (uint8_t)((via1.orb & via1.ddrb) | (uint8_t)~via1.ddrb);
    uint8_t m = 0;
    if (out & V1_DATA_OUT) { m |= IEC_DATA; }
    if (out & V1_CLK_OUT) { m |= IEC_CLK; }
    bool atn_asserted = (iec_ext & IEC_ATN) != 0;
    bool atna = (out & V1_ATNA) != 0;
    if (atn_asserted && !atna) { m |= IEC_DATA; }
    return m;
}

// Set what the external bus (the C64) pulls low; the drive's VIA1 read and its
// ATN CA1 interrupt see this on the next tick.
void drive_set_iec_ext(uint8_t mask) {
    iec_ext = (uint8_t)(mask & (IEC_CLK | IEC_DATA | IEC_ATN));
}

bool drive_activity(void) {
    // VIA2 PB2 = motor, PB3 = activity LED.
    return (via2.orb & via2.ddrb & 0x0Cu) != 0;
}

bool drive_activity_led(void) {
    return (via2.orb & via2.ddrb & 0x08u) != 0;
}

bool drive_motor_on(void) {
    return (via2.orb & via2.ddrb & 0x04u) != 0;
}

void drive_force_idle(void) {
    // VIA2 PB2 = motor, PB3 = activity LED. Clear both so attach/spinUp cannot
    // leave a 1 Hz on/off thrash if DOS is still in a motor-timeout cycle.
    via2.orb = (uint8_t)(via2.orb & ~0x0Cu);
    via2.ddrb = (uint8_t)(via2.ddrb | 0x0Cu);
    drive_ram[0x20] = (uint8_t)(drive_ram[0x20] & (uint8_t)~0x30u); // DRVST motor bits
}

// The read-head framing state and clock accumulators, alongside drive RAM, the two
// VIAs, and the drive 6502 (data fields only; its bus callbacks are re-bound by
// init). One field list keeps save and restore in step.
