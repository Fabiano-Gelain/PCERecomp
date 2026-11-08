/*
 * SCSI controller of the CD-ROM² reader and the drive behind it.
 *
 * The CPU talks to the drive over a simplified SCSI bus: it writes a command
 * (6 or 10 bytes) byte by byte, using the REQ/ACK signal pair, and receives the response
 * (data, status, message) also byte by byte. Each step of the conversation is a
 * "phase"; changing phase takes some time (events delayed by a number of master cycles).
 *
 * Commands: TEST UNIT READY, REQUEST SENSE, READ (2048-byte sectors, with seek time
 * and 75 sectors/s), audio PLAY/STOP/PAUSE, READ SUBCODE Q, READ TOC.
 *
 * Behavior reference: reference/Geargrafx/src/scsi_controller*, cdrom_media.cpp (seek time).
 */
#ifndef PCE_SCSI_H
#define PCE_SCSI_H

#include <stdint.h>

#include "cd/cdimage.h"

struct CdRom;

/* Bus signals (bits 7..3 = what the CPU reads at $1800) */
enum {
    SCSI_SEL = 0x01, SCSI_ACK = 0x02, SCSI_ATN = 0x04, SCSI_IO = 0x08,
    SCSI_CD = 0x10, SCSI_MSG = 0x20, SCSI_REQ = 0x40, SCSI_BSY = 0x80,
    SCSI_RST = 0x100
};

typedef enum ScsiPhase {
    SCSI_PHASE_BUS_FREE, SCSI_PHASE_SELECTION, SCSI_PHASE_MESSAGE_OUT, SCSI_PHASE_COMMAND,
    SCSI_PHASE_DATA_IN, SCSI_PHASE_DATA_OUT, SCSI_PHASE_MESSAGE_IN, SCSI_PHASE_STATUS, SCSI_PHASE_BUSY
} ScsiPhase;

typedef enum ScsiEvent {
    SCSI_EVENT_NONE, SCSI_EVENT_SET_COMMAND_PHASE, SCSI_EVENT_SET_REQ_SIGNAL, SCSI_EVENT_SET_GOOD_STATUS,
    SCSI_EVENT_SET_DATA_IN_PHASE, SCSI_EVENT_SET_MESSAGE_IN_PHASE, SCSI_EVENT_SET_BUS_FREE_PHASE,
    SCSI_EVENT_SET_RESPONSE_REQ_SIGNAL
} ScsiEvent;

#define SCSI_STATUS_GOOD 0x00
#define SCSI_STATUS_CHECK_CONDITION 0x02

#define SCSI_COMMAND_CAPACITY 16
#define SCSI_DATA_CAPACITY 2048

typedef struct Scsi {
    struct CdRom *cd;
    uint8_t db;        /* byte on the data bus (coming from the drive) */
    uint32_t db_origin; /* disc position + 1 of the byte in db, if it is sector data (0 = it is not); see origin.h */
    uint32_t data_lba;  /* sector that is in the data buffer */
    uint16_t signals;
    ScsiPhase phase;
    ScsiEvent next_event;
    int32_t next_event_cycles;
    int32_t next_load_cycles; /* > 0: this much remains until the next sector is ready */
    uint32_t load_sector, load_sector_count;
    int32_t auto_ack_cycles;
    uint32_t initial_read_phase_cycles; /* extra delay of the 1st read (0 = like gg_trace) */
    uint8_t command[SCSI_COMMAND_CAPACITY];
    int command_size;
    uint8_t data[SCSI_DATA_CAPACITY];
    uint32_t data_size, data_offset;
    int bus_changed;
    uint16_t previous_signals;
    uint8_t data_bus_latch; /* last byte written by the CPU */
    uint32_t current_sector;  /* read head position */
} Scsi;

void scsi_reset(Scsi *s, int keep_rst_signal);
void scsi_clock(Scsi *s, uint32_t cycles);
uint8_t scsi_read_data(Scsi *s);
void scsi_write_data(Scsi *s, uint8_t value);
uint8_t scsi_get_status(Scsi *s);
void scsi_set_signal(Scsi *s, uint16_t signals);
void scsi_clear_signal(Scsi *s, uint16_t signals);
int scsi_signal(const Scsi *s, uint16_t signal);
void scsi_auto_ack(Scsi *s);
void scsi_start_selection(Scsi *s);
void scsi_start_status(Scsi *s, uint8_t status, uint8_t length);
void scsi_audio_seek_completed(Scsi *s);

/* Head seek time (ms) between two sectors. */
uint32_t cd_seek_time_ms(uint32_t start_lba, uint32_t end_lba);
/* Microseconds -> master cycles. */
uint32_t cd_time_to_cycles(uint32_t us);

#endif
