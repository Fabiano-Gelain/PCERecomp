/*
 * SCSI controller and CD drive. See scsi.h.
 */
#include "cd/scsi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cd/cdrom.h"

#define MASTER_CLOCK_RATE 21477273u
#define RESPONSE_BYTE_CYCLES 900
#define PHASE_CHANGE_CYCLES 1500
#define SECTOR_TRANSFER_CYCLES (MASTER_CLOCK_RATE / 75) /* 1x: 75 sectors per second */

enum {
    CMD_TEST_UNIT_READY = 0x00, CMD_REQUEST_SENSE = 0x03, CMD_READ = 0x08,
    CMD_AUDIO_START_POSITION = 0xD8, CMD_AUDIO_STOP_POSITION = 0xD9, CMD_AUDIO_PAUSE = 0xDA,
    CMD_READ_SUBCODE_Q = 0xDD, CMD_READ_TOC = 0xDE
};

uint32_t cd_time_to_cycles(uint32_t us)
{
    return (uint32_t)(((uint64_t)us * MASTER_CLOCK_RATE) / 1000000u);
}

static uint8_t dec_to_bcd(uint32_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static uint8_t bcd_to_dec(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

/* ---- Seek time ----
 * The disc spins more slowly toward the outside; sectors form groups with the same number of
 * sectors per revolution. Table and formula: measurements used by Geargrafx (cdrom_media.cpp). */

typedef struct SeekGroup {
    uint32_t sectors_per_revolution;
    uint32_t start, end;
    double rotation_ms;
} SeekGroup;

static const SeekGroup SEEK_GROUPS[] = {
    {10, 0, 12572, 133.47},      {11, 12573, 30244, 146.82},  {12, 30245, 49523, 160.17},
    {13, 49524, 70408, 173.51},  {14, 70409, 92900, 186.86},  {15, 92901, 116998, 200.21},
    {16, 116999, 142703, 213.56}, {17, 142704, 170014, 226.90}, {18, 170015, 198932, 240.25},
    {19, 198933, 229456, 253.60}, {20, 229457, 261587, 266.95}, {21, 261588, 295324, 280.29},
    {22, 295325, 330668, 293.64}, {23, 330669, 333012, 306.99},
};
#define SEEK_GROUP_COUNT (sizeof(SEEK_GROUPS) / sizeof(SEEK_GROUPS[0]))

static uint32_t seek_group(uint32_t lba)
{
    for (uint32_t i = 0; i < SEEK_GROUP_COUNT; i++)
        if (lba >= SEEK_GROUPS[i].start && lba <= SEEK_GROUPS[i].end)
            return i;
    return 0;
}

uint32_t cd_seek_time_ms(uint32_t start_lba, uint32_t end_lba)
{
    uint32_t si = seek_group(start_lba), ti = seek_group(end_lba);
    uint32_t lba_difference = (uint32_t)abs((int)end_lba - (int)start_lba);
    const SeekGroup *s = &SEEK_GROUPS[si], *t = &SEEK_GROUPS[ti];
    double tracks;
    /* Integer divisions are integer divisions, as in the reference. */
    if (ti == si) {
        tracks = lba_difference / t->sectors_per_revolution;
    } else if (ti > si) {
        tracks = (s->end - start_lba) / s->sectors_per_revolution;
        tracks += (end_lba - t->start) / t->sectors_per_revolution;
        tracks += 1606.48 * (ti - si - 1);
    } else {
        tracks = (start_lba - s->start) / s->sectors_per_revolution;
        tracks += (t->end - end_lba) / t->sectors_per_revolution;
        tracks += 1606.48 * (si - ti - 1);
    }
    if (lba_difference < 2)
        return 9 * 1000 / 60;
    if (lba_difference < 5)
        return (uint32_t)((9 * 1000 / 60) + (t->rotation_ms / 2));
    if (tracks <= 80)
        return (uint32_t)((18 * 1000 / 60) + (t->rotation_ms / 2));
    if (tracks <= 160)
        return (uint32_t)((22 * 1000 / 60) + (t->rotation_ms / 2));
    if (tracks <= 644)
        return (uint32_t)((22 * 1000 / 60) + (t->rotation_ms / 2) + ((tracks - 161) * 16.66 / 80));
    return (uint32_t)((48 * 1000 / 60) + ((tracks - 644) * 16.66 / 195));
}

/* ---- Signals and phases ---- */

int scsi_signal(const Scsi *s, uint16_t signal)
{
    return (s->signals & signal) != 0;
}

void scsi_set_signal(Scsi *s, uint16_t signals)
{
    s->previous_signals = s->signals;
    s->signals |= signals;
    if (s->previous_signals != s->signals) {
        s->bus_changed = 1;
        s->previous_signals = s->signals;
    }
}

void scsi_clear_signal(Scsi *s, uint16_t signals)
{
    s->previous_signals = s->signals;
    s->signals &= (uint16_t)~signals;
    if (s->previous_signals != s->signals) {
        s->bus_changed = 1;
        s->previous_signals = s->signals;
    }
}

static void update_irqs(Scsi *s)
{
    if (scsi_signal(s, SCSI_BSY) && scsi_signal(s, SCSI_IO) && scsi_signal(s, SCSI_REQ)) {
        cdrom_set_irq(s->cd, scsi_signal(s, SCSI_CD) ? CD_IRQ_STATUS_AND_MSG_IN : CD_IRQ_DATA_IN);
    } else {
        cdrom_clear_irq(s->cd, CD_IRQ_STATUS_AND_MSG_IN);
        cdrom_clear_irq(s->cd, CD_IRQ_DATA_IN);
    }
}

static void next_event(Scsi *s, ScsiEvent event, uint32_t cycles)
{
    s->next_event = event;
    s->next_event_cycles = (int32_t)cycles;
}

static void set_data(Scsi *s, const uint8_t *data, uint32_t size)
{
    memcpy(s->data, data, size);
    s->data_size = size;
    s->data_offset = 0;
}

static void set_phase(Scsi *s, ScsiPhase phase)
{
    if (s->phase == phase)
        return;
    scsi_clear_signal(s, SCSI_BSY | SCSI_REQ | SCSI_CD | SCSI_MSG | SCSI_IO);
    s->phase = phase;
    switch (phase) {
    case SCSI_PHASE_COMMAND:
        s->next_load_cycles = 0;
        s->data_size = 0;
        scsi_set_signal(s, SCSI_BSY | SCSI_CD | SCSI_REQ);
        break;
    case SCSI_PHASE_DATA_IN:
        scsi_set_signal(s, SCSI_BSY | SCSI_IO);
        break;
    case SCSI_PHASE_MESSAGE_IN:
        scsi_set_signal(s, SCSI_BSY | SCSI_CD | SCSI_IO | SCSI_MSG | SCSI_REQ);
        break;
    case SCSI_PHASE_STATUS:
        scsi_set_signal(s, SCSI_BSY | SCSI_CD | SCSI_IO | SCSI_REQ);
        break;
    case SCSI_PHASE_BUSY:
        scsi_set_signal(s, SCSI_BSY);
        break;
    default:
        break;
    }
}

void scsi_start_status(Scsi *s, uint8_t status, uint8_t length)
{
    memset(s->data, status, length);
    s->data_size = length;
    s->data_offset = 0;
    s->db = status;
    s->db_origin = 0;
    set_phase(s, SCSI_PHASE_STATUS);
}

void scsi_audio_seek_completed(Scsi *s)
{
    if (s->phase == SCSI_PHASE_BUSY)
        scsi_start_status(s, SCSI_STATUS_GOOD, 1);
}

void scsi_reset(Scsi *s, int keep_rst_signal)
{
    s->db = 0;
    s->db_origin = 0;
    if (keep_rst_signal)
        s->signals &= SCSI_RST;
    else
        s->signals = 0;
    s->phase = SCSI_PHASE_BUS_FREE;
    s->next_event = SCSI_EVENT_NONE;
    s->next_event_cycles = 0;
    s->next_load_cycles = 0;
    s->load_sector = 0;
    s->load_sector_count = 0;
    s->auto_ack_cycles = 0;
    if (!keep_rst_signal)
        s->initial_read_phase_cycles = 0;
    s->command_size = 0;
    s->data_size = 0;
    s->data_offset = 0;
    s->bus_changed = 0;
    s->previous_signals = s->signals;
    s->data_bus_latch = 0;
    update_irqs(s);
    cdaudio_stop(&s->cd->audio);
    s->current_sector = 0;
}

void scsi_start_selection(Scsi *s)
{
    if (s->phase != SCSI_PHASE_DATA_IN) {
        next_event(s, SCSI_EVENT_SET_COMMAND_PHASE, 75000);
    } else {
        scsi_start_status(s, SCSI_STATUS_GOOD, 8);
        next_event(s, SCSI_EVENT_SET_COMMAND_PHASE, 900000);
    }
}

uint8_t scsi_read_data(Scsi *s)
{
    if (s->phase == SCSI_PHASE_DATA_IN || s->phase == SCSI_PHASE_STATUS || s->phase == SCSI_PHASE_MESSAGE_IN)
        return s->db;
    return s->data_bus_latch;
}

void scsi_write_data(Scsi *s, uint8_t value)
{
    s->data_bus_latch = value;
}

uint8_t scsi_get_status(Scsi *s)
{
    return (uint8_t)(s->signals & 0xF8);
}

void scsi_auto_ack(Scsi *s)
{
    if (scsi_signal(s, SCSI_REQ) && scsi_signal(s, SCSI_IO) && !scsi_signal(s, SCSI_CD)) {
        s->auto_ack_cycles = (int32_t)cd_time_to_cycles(1);
        scsi_set_signal(s, SCSI_ACK);
    }
}

/* ---- Disc ---- */

static int disc_ready(const Scsi *s)
{
    return s->cd->image != NULL;
}

static uint32_t sector_count(const Scsi *s)
{
    return s->cd->image ? s->cd->image->sector_count : 0;
}

static uint32_t first_sector_of_track(const Scsi *s, int track)
{
    const CdImage *img = s->cd->image;
    if (track < img->track_count)
        return img->tracks[track].start_lba;
    if (track > 0 && track == img->track_count)
        return img->tracks[track - 1].end_lba + 1;
    return 0;
}

/* ---- Commands ---- */

static void command_read(Scsi *s)
{
    uint32_t lba = ((uint32_t)(s->command[1] & 0x1F) << 16) | ((uint32_t)s->command[2] << 8) | s->command[3];
    uint32_t count = s->command[4];
    if (count == 0)
        count = 256;
    if (lba >= sector_count(s)) {
        scsi_start_status(s, SCSI_STATUS_GOOD, 1);
        return;
    }
    uint32_t seek_cycles = cd_time_to_cycles(cd_seek_time_ms(s->current_sector, lba) * 1000);
    if (s->initial_read_phase_cycles > 0) {
        seek_cycles += s->initial_read_phase_cycles;
        s->initial_read_phase_cycles = 0;
    }
    s->next_load_cycles = (int32_t)(seek_cycles + SECTOR_TRANSFER_CYCLES);
    s->load_sector = lba;
    s->load_sector_count = count;
    set_phase(s, SCSI_PHASE_DATA_IN);
    cdaudio_set_idle(&s->cd->audio);
}

static uint32_t audio_lba(Scsi *s)
{
    switch (s->command[9] & 0xC0) {
    case 0x00:
        return ((uint32_t)s->command[3] << 16) | ((uint32_t)s->command[4] << 8) | s->command[5];
    case 0x40: {
        uint32_t lba = ((uint32_t)bcd_to_dec(s->command[2]) * 60 + bcd_to_dec(s->command[3])) * 75 +
                       bcd_to_dec(s->command[4]);
        return lba - 150;
    }
    case 0x80: {
        int track = bcd_to_dec(s->command[2]) - 1;
        return track >= 0 ? first_sector_of_track(s, track) : 0;
    }
    default:
        cdrom_unimplemented(s->cd, "SCSI: unknown audio address mode");
        return 0;
    }
}

static void command_audio_start(Scsi *s)
{
    uint32_t lba = audio_lba(s);
    if (lba >= sector_count(s)) {
        scsi_start_status(s, SCSI_STATUS_GOOD, 1);
        return;
    }
    if (!disc_ready(s)) {
        scsi_start_status(s, SCSI_STATUS_CHECK_CONDITION, 1);
        return;
    }
    /* Mode 0: only positions (paused); any other value: plays. Responds when the seek finishes. */
    uint8_t mode = s->command[1];
    cdaudio_start(&s->cd->audio, lba, mode == 0);
    if (mode == 0 && !cdaudio_is_seeking(&s->cd->audio))
        scsi_start_status(s, SCSI_STATUS_GOOD, 1);
    else
        set_phase(s, SCSI_PHASE_BUSY);
}

static void command_audio_stop(Scsi *s)
{
    if (!disc_ready(s)) {
        scsi_start_status(s, SCSI_STATUS_CHECK_CONDITION, 1);
        return;
    }
    /* Mode 0: stops immediately; 1: repeats on arrival; 2: stops and responds on arrival; 3: stops on arrival. */
    uint32_t lba = audio_lba(s);
    uint8_t mode = s->command[1];
    CdAudio *a = &s->cd->audio;
    switch (mode) {
    case 0:
        cdaudio_stop(a);
        break;
    case 1:
        cdaudio_set_stop_lba(a, lba, CD_AUDIO_STOP_EVENT_LOOP);
        break;
    case 2:
        cdaudio_set_stop_lba(a, lba, CD_AUDIO_STOP_EVENT_IRQ);
        break;
    case 3:
        cdaudio_set_stop_lba(a, lba, CD_AUDIO_STOP_EVENT_STOP);
        break;
    default:
        break;
    }
    if (mode == 1 || mode == 2)
        set_phase(s, SCSI_PHASE_BUSY);
    else
        scsi_start_status(s, SCSI_STATUS_GOOD, 1);
}

static void command_audio_pause(Scsi *s)
{
    if (!disc_ready(s)) {
        scsi_start_status(s, SCSI_STATUS_CHECK_CONDITION, 1);
        return;
    }
    cdaudio_pause(&s->cd->audio);
    scsi_start_status(s, SCSI_STATUS_GOOD, 1);
}

static void command_read_subcode_q(Scsi *s)
{
    if (!disc_ready(s)) {
        scsi_start_status(s, SCSI_STATUS_CHECK_CONDITION, 1);
        return;
    }
    const CdImage *img = s->cd->image;
    const CdAudio *a = &s->cd->audio;
    uint32_t lba = a->state == CD_AUDIO_IDLE ? s->current_sector : cdaudio_subcode_lba(a);
    int track = cd_track_of_lba(img, lba, 0);
    int data_track = track >= 0 && img->tracks[track].type != CD_TRACK_AUDIO;
    uint32_t rel = track >= 0 ? lba - first_sector_of_track(s, track) : 0;
    uint32_t abs_lba = lba + 150;
    uint8_t buf[10];
    buf[0] = (uint8_t)cdaudio_subcode_state(a);
    buf[1] = data_track ? 0x41 : 0x01;
    buf[2] = dec_to_bcd((uint32_t)(track + 1));
    buf[3] = 1;
    buf[4] = dec_to_bcd((uint8_t)(rel / 75 / 60));
    buf[5] = dec_to_bcd(rel / 75 % 60);
    buf[6] = dec_to_bcd(rel % 75);
    buf[7] = dec_to_bcd((uint8_t)(abs_lba / 75 / 60));
    buf[8] = dec_to_bcd(abs_lba / 75 % 60);
    buf[9] = dec_to_bcd(abs_lba % 75);
    set_data(s, buf, sizeof(buf));
    next_event(s, SCSI_EVENT_SET_DATA_IN_PHASE, 3000);
}

static void command_read_toc(Scsi *s)
{
    if (!disc_ready(s)) {
        scsi_start_status(s, SCSI_STATUS_CHECK_CONDITION, 1);
        return;
    }
    const CdImage *img = s->cd->image;
    uint8_t buf[4] = {0, 0, 0, 0};
    uint32_t length = img->sector_count + 150; /* total disc length */
    switch (s->command[1]) {
    case 0x00: /* first and last track */
        buf[0] = 0x01;
        buf[1] = dec_to_bcd((uint8_t)img->track_count);
        break;
    case 0x01: /* disc length */
        buf[0] = dec_to_bcd((uint8_t)(length / 75 / 60));
        buf[1] = dec_to_bcd(length / 75 % 60);
        buf[2] = dec_to_bcd(length % 75);
        break;
    case 0x02: { /* start of a track */
        int track = bcd_to_dec(s->command[2]);
        if (track == 0)
            track = 1;
        uint8_t type = 0x04;
        uint32_t start;
        if (track > img->track_count) {
            start = length;
            type = 0x00;
        } else {
            if (img->tracks[track - 1].type == CD_TRACK_AUDIO)
                type = 0x00;
            start = first_sector_of_track(s, track - 1) + 150;
        }
        buf[0] = dec_to_bcd((uint8_t)(start / 75 / 60));
        buf[1] = dec_to_bcd(start / 75 % 60);
        buf[2] = dec_to_bcd(start % 75);
        buf[3] = type;
        break;
    }
    default:
        return; /* unknown mode: the reference does not respond at all */
    }
    set_data(s, buf, sizeof(buf));
    next_event(s, SCSI_EVENT_SET_DATA_IN_PHASE, 9000);
}

static int command_length(uint8_t opcode)
{
    switch (opcode) {
    case CMD_TEST_UNIT_READY:
    case CMD_READ:
    case CMD_REQUEST_SENSE:
        return 6;
    case CMD_AUDIO_START_POSITION:
    case CMD_AUDIO_STOP_POSITION:
    case CMD_AUDIO_PAUSE:
    case CMD_READ_SUBCODE_Q:
    case CMD_READ_TOC:
        return 10;
    default:
        return 0;
    }
}

static void execute_command(Scsi *s)
{
    /* Diagnostics: with the environment variable PCE_CD_LOG=1, each command goes to stderr. */
    static int log = -1;
    if (log < 0)
        log = getenv("PCE_CD_LOG") != NULL;
    if (log) {
        fprintf(stderr, "[%.2f s] SCSI", (double)*s->cd->master_cycles / MASTER_CLOCK_RATE);
        for (int i = 0; i < command_length(s->command[0]) && i < s->command_size; i++)
            fprintf(stderr, " %02X", s->command[i]);
        fprintf(stderr, "  (audio: state %d, sector %u)\n", s->cd->audio.state, s->cd->audio.current_lba);
    }
    switch (s->command[0]) {
    case CMD_TEST_UNIT_READY:
        next_event(s, SCSI_EVENT_SET_GOOD_STATUS, 450000);
        break;
    case CMD_REQUEST_SENSE:
        next_event(s, SCSI_EVENT_SET_GOOD_STATUS, cd_time_to_cycles(21000));
        break;
    case CMD_READ:
        command_read(s);
        break;
    case CMD_AUDIO_START_POSITION:
        command_audio_start(s);
        break;
    case CMD_AUDIO_STOP_POSITION:
        command_audio_stop(s);
        break;
    case CMD_AUDIO_PAUSE:
        command_audio_pause(s);
        break;
    case CMD_READ_SUBCODE_Q:
        command_read_subcode_q(s);
        break;
    case CMD_READ_TOC:
        command_read_toc(s);
        break;
    default:
        break;
    }
}

/* ---- Phase progression (on every signal change) ---- */

static void update_command_phase(Scsi *s)
{
    if (scsi_signal(s, SCSI_REQ) && scsi_signal(s, SCSI_ACK)) {
        scsi_clear_signal(s, SCSI_REQ);
        if (s->command_size < SCSI_COMMAND_CAPACITY) {
            s->command[s->command_size++] = s->data_bus_latch;
        } else {
            scsi_start_status(s, SCSI_STATUS_GOOD, 1);
            s->command_size = 0;
        }
    } else if (!scsi_signal(s, SCSI_REQ) && !scsi_signal(s, SCSI_ACK) && s->command_size > 0) {
        int length = command_length(s->command[0]);
        if (length == 0) {
            scsi_start_status(s, SCSI_STATUS_GOOD, 1);
            s->command_size = 0;
        } else if (s->command_size >= length) {
            execute_command(s);
            s->command_size = 0;
        } else {
            next_event(s, SCSI_EVENT_SET_REQ_SIGNAL, 3000);
        }
    }
}

static void update_data_in_phase(Scsi *s)
{
    if (scsi_signal(s, SCSI_REQ) && scsi_signal(s, SCSI_ACK)) {
        scsi_clear_signal(s, SCSI_REQ);
    } else if (!scsi_signal(s, SCSI_REQ) && !scsi_signal(s, SCSI_ACK)) {
        if (s->data_size > 0) {
            int short_response = s->data_size < SCSI_DATA_CAPACITY;
            s->db = s->data[s->data_offset++];
            s->db_origin = short_response ? 0 : s->data_lba * 2048u + s->data_offset; /* position + 1 */
            if (s->data_offset == s->data_size)
                s->data_size = 0;
            if (short_response) {
                if (s->data_offset > 1)
                    next_event(s, SCSI_EVENT_SET_RESPONSE_REQ_SIGNAL, RESPONSE_BYTE_CYCLES);
                else
                    scsi_set_signal(s, SCSI_REQ);
            } else {
                scsi_set_signal(s, SCSI_REQ);
            }
        } else if (s->load_sector_count == 0) {
            next_event(s, SCSI_EVENT_SET_GOOD_STATUS, 3000);
        }
    }
}

static void update_status_phase(Scsi *s)
{
    if (scsi_signal(s, SCSI_REQ) && scsi_signal(s, SCSI_ACK)) {
        scsi_clear_signal(s, SCSI_REQ);
    } else if (!scsi_signal(s, SCSI_REQ) && !scsi_signal(s, SCSI_ACK) && s->data_size > 0) {
        s->db = s->data[s->data_offset++];
        if (s->data_offset == s->data_size) {
            s->data_size = 0;
            next_event(s, SCSI_EVENT_SET_MESSAGE_IN_PHASE, PHASE_CHANGE_CYCLES);
        } else {
            next_event(s, SCSI_EVENT_SET_RESPONSE_REQ_SIGNAL, PHASE_CHANGE_CYCLES);
        }
    }
}

static void update_message_in_phase(Scsi *s)
{
    if (scsi_signal(s, SCSI_REQ) && scsi_signal(s, SCSI_ACK))
        scsi_clear_signal(s, SCSI_REQ);
    else if (!scsi_signal(s, SCSI_REQ) && !scsi_signal(s, SCSI_ACK))
        next_event(s, SCSI_EVENT_SET_BUS_FREE_PHASE, PHASE_CHANGE_CYCLES);
}

static void update_scsi(Scsi *s)
{
    if (!s->bus_changed)
        return;
    s->bus_changed = 0;
    if (scsi_signal(s, SCSI_RST)) {
        scsi_reset(s, 1);
        return;
    }
    do {
        s->bus_changed = 0;
        switch (s->phase) {
        case SCSI_PHASE_COMMAND:
            update_command_phase(s);
            break;
        case SCSI_PHASE_DATA_IN:
            update_data_in_phase(s);
            break;
        case SCSI_PHASE_STATUS:
            update_status_phase(s);
            break;
        case SCSI_PHASE_MESSAGE_IN:
            update_message_in_phase(s);
            break;
        default:
            break;
        }
    } while (s->bus_changed);
    update_irqs(s);
}

static void run_event(Scsi *s)
{
    switch (s->next_event) {
    case SCSI_EVENT_SET_COMMAND_PHASE:
        set_phase(s, SCSI_PHASE_COMMAND);
        break;
    case SCSI_EVENT_SET_REQ_SIGNAL:
    case SCSI_EVENT_SET_RESPONSE_REQ_SIGNAL:
        scsi_set_signal(s, SCSI_REQ);
        break;
    case SCSI_EVENT_SET_GOOD_STATUS:
        scsi_start_status(s, SCSI_STATUS_GOOD, 1);
        break;
    case SCSI_EVENT_SET_DATA_IN_PHASE:
        set_phase(s, SCSI_PHASE_DATA_IN);
        break;
    case SCSI_EVENT_SET_MESSAGE_IN_PHASE:
        set_phase(s, SCSI_PHASE_MESSAGE_IN);
        break;
    case SCSI_EVENT_SET_BUS_FREE_PHASE:
        set_phase(s, SCSI_PHASE_BUS_FREE);
        break;
    default:
        break;
    }
    next_event(s, SCSI_EVENT_NONE, 0);
}

/* A sector has finished passing under the head: it goes into the buffer, if the previous one was already read. */
static void load_sector(Scsi *s)
{
    if (s->data_size == 0) {
        if (s->load_sector >= sector_count(s) ||
            cd_read_sector(s->cd->image, s->load_sector, s->data) != 0) {
            s->data_size = 0;
            s->data_offset = 0;
            s->load_sector_count = 0;
            s->next_load_cycles = 0;
            scsi_start_status(s, SCSI_STATUS_CHECK_CONDITION, 1);
            return;
        }
        s->data_size = SCSI_DATA_CAPACITY;
        s->data_offset = 0;
        s->data_lba = s->load_sector;
        /* Head position: the sector after the one read (like CdRomCueBinImage::ReadSector). */
        s->current_sector = s->load_sector + 1 < sector_count(s) ? s->load_sector + 1 : sector_count(s) - 1;
        s->load_sector = (s->load_sector + 1) & 0x1FFFFF;
        s->load_sector_count--;
        s->next_load_cycles = s->load_sector_count == 0 ? 0 : (int32_t)SECTOR_TRANSFER_CYCLES;
        s->bus_changed = 1;
    } else {
        /* The program has not read the previous sector yet: the drive makes another revolution. */
        s->next_load_cycles = (int32_t)cd_time_to_cycles(290000);
    }
}

void scsi_clock(Scsi *s, uint32_t cycles)
{
    if (s->next_event == SCSI_EVENT_NONE && s->next_load_cycles <= 0 && !s->bus_changed && s->auto_ack_cycles <= 0)
        return;
    if (s->next_event != SCSI_EVENT_NONE) {
        s->next_event_cycles -= (int32_t)cycles;
        if (s->next_event_cycles <= 0)
            run_event(s);
    }
    if (s->next_load_cycles > 0) {
        s->next_load_cycles -= (int32_t)cycles;
        if (s->next_load_cycles <= 0) {
            s->next_load_cycles = 0;
            load_sector(s);
        }
    }
    update_scsi(s);
    if (s->auto_ack_cycles > 0) {
        s->auto_ack_cycles -= (int32_t)cycles;
        if (s->auto_ack_cycles <= 0) {
            s->auto_ack_cycles = 0;
            scsi_clear_signal(s, SCSI_ACK);
        }
    }
}
