/*
 * Origin of RAM bytes. See origin.h.
 */
#include "cd/origin.h"

#include <string.h>

#include "cd/cdrom.h"
#include "pce/pce.h"

enum { REG_A, REG_X, REG_Y };

void origin_init(Origin *o)
{
    memset(o, 0, sizeof(*o));
    o->store_reg = -1;
}

int origin_index(uint32_t phys)
{
    uint8_t bank = (uint8_t)(phys >> 13);
    uint32_t offset = phys & 0x1FFF;
    if (bank >= 0x80 && bank <= 0x87)
        return (int)((bank - 0x80) * 0x2000u + offset);
    if (bank >= PCE_RAM_BANK && bank <= 0xFB)
        return (int)(0x10000u + offset);
    return -1;
}

/* Register stored by a store instruction, or -1. */
static int stored_register(uint8_t op)
{
    switch (op) {
    case 0x85: case 0x95: case 0x8D: case 0x9D: case 0x99: case 0x81: case 0x91: case 0x92: case 0x48:
        return REG_A;
    case 0x86: case 0x96: case 0x8E: case 0xDA:
        return REG_X;
    case 0x84: case 0x94: case 0x8C: case 0x5A:
        return REG_Y;
    default:
        return -1;
    }
}

/* Register loaded from memory by an instruction, or -1. */
static int loaded_register(uint8_t op)
{
    switch (op) {
    case 0xA9: case 0xA5: case 0xB5: case 0xAD: case 0xBD: case 0xB9: case 0xA1: case 0xB1: case 0xB2: case 0x68:
        return REG_A;
    case 0xA2: case 0xA6: case 0xB6: case 0xAE: case 0xBE: case 0xFA:
        return REG_X;
    case 0xA0: case 0xA4: case 0xB4: case 0xAC: case 0xBC: case 0x7A:
        return REG_Y;
    default:
        return -1;
    }
}

static void current_registers(const struct Pce *pce, uint8_t r[3])
{
    r[REG_A] = pce->cpu.a;
    r[REG_X] = pce->cpu.x;
    r[REG_Y] = pce->cpu.y;
}

void origin_before(Origin *o, struct Pce *pce, uint8_t opcode)
{
    (void)pce;
    o->opcode = opcode;
    o->store_reg = stored_register(opcode);
    o->has_read = 0;
}

void origin_after(Origin *o, struct Pce *pce)
{
    uint8_t r[3];
    current_registers(pce, r);
    uint32_t old_tag[3] = {o->reg_tag[0], o->reg_tag[1], o->reg_tag[2]};
    uint8_t old_val[3] = {o->reg_value[0], o->reg_value[1], o->reg_value[2]};

    int loaded = loaded_register(o->opcode);
    if (loaded >= 0) {
        o->reg_tag[loaded] = o->has_read ? o->read_tag : 0;
        o->reg_value[loaded] = r[loaded];
    }
    /* Transfers between registers carry the origin along. */
    switch (o->opcode) {
    case 0xAA: o->reg_tag[REG_X] = old_tag[REG_A]; o->reg_value[REG_X] = old_val[REG_A]; break; /* TAX */
    case 0x8A: o->reg_tag[REG_A] = old_tag[REG_X]; o->reg_value[REG_A] = old_val[REG_X]; break; /* TXA */
    case 0xA8: o->reg_tag[REG_Y] = old_tag[REG_A]; o->reg_value[REG_Y] = old_val[REG_A]; break; /* TAY */
    case 0x98: o->reg_tag[REG_A] = old_tag[REG_Y]; o->reg_value[REG_A] = old_val[REG_Y]; break; /* TYA */
    case 0x22: /* SAX */
        o->reg_tag[REG_A] = old_tag[REG_X]; o->reg_value[REG_A] = old_val[REG_X];
        o->reg_tag[REG_X] = old_tag[REG_A]; o->reg_value[REG_X] = old_val[REG_A];
        break;
    case 0x42: /* SAY */
        o->reg_tag[REG_A] = old_tag[REG_Y]; o->reg_value[REG_A] = old_val[REG_Y];
        o->reg_tag[REG_Y] = old_tag[REG_A]; o->reg_value[REG_Y] = old_val[REG_A];
        break;
    case 0x02: /* SXY */
        o->reg_tag[REG_X] = old_tag[REG_Y]; o->reg_value[REG_X] = old_val[REG_Y];
        o->reg_tag[REG_Y] = old_tag[REG_X]; o->reg_value[REG_Y] = old_val[REG_X];
        break;
    default:
        break;
    }
    /* The origin only holds while the value is the same one that was loaded/transferred. */
    for (int i = 0; i < 3; i++)
        if (o->reg_value[i] != r[i])
            o->reg_tag[i] = 0;
}

void origin_read(Origin *o, struct Pce *pce, uint32_t phys, uint8_t value)
{
    uint32_t tag = 0;
    int idx = origin_index(phys);
    if (idx >= 0) {
        tag = o->ram[idx];
    } else if ((phys >> 13) == PCE_IO_BANK && pce->cd) {
        uint16_t reg = (uint16_t)(phys & 0x1FFF);
        if ((reg & 0x1C00) == 0x1800 && ((reg & 0x3FF) == 0x01 || (reg & 0x3FF) == 0x08) &&
            pce->cd->scsi.phase == SCSI_PHASE_DATA_IN)
            tag = pce->cd->scsi.db_origin;
    }
    o->has_read = 1;
    o->read_value = value;
    o->read_tag = tag;
}

void origin_write(Origin *o, struct Pce *pce, uint32_t phys, uint8_t value)
{
    int idx = origin_index(phys);
    if (idx < 0)
        return;
    uint32_t tag = 0;
    if (pce->cpu.in_block_transfer) {
        if (o->has_read && o->read_value == value)
            tag = o->read_tag;
    } else if (o->store_reg >= 0 && o->reg_value[o->store_reg] == value) {
        tag = o->reg_tag[o->store_reg];
    }
    o->ram[idx] = tag;
}

uint32_t origin_data_tag(struct Pce *pce)
{
    return pce->cd && pce->cd->scsi.phase == SCSI_PHASE_DATA_IN ? pce->cd->scsi.db_origin : 0;
}

void origin_mark(Origin *o, uint32_t phys, uint32_t tag)
{
    int idx = origin_index(phys);
    if (idx >= 0)
        o->ram[idx] = tag;
}

uint32_t origin_of_instruction(Origin *o, struct Pce *pce, uint16_t pc, int size)
{
    int first = origin_index(huc6280_phys(&pce->cpu, pc));
    if (first < 0 || o->ram[first] == 0)
        return ORIGIN_NONE;
    uint32_t start = o->ram[first] - 1;
    for (int i = 1; i < size; i++) {
        int idx = origin_index(huc6280_phys(&pce->cpu, (uint16_t)(pc + i)));
        if (idx < 0 || o->ram[idx] != start + 1 + (uint32_t)i)
            return ORIGIN_NONE;
    }
    /* Check: are the bytes in RAM the ones from the disc? */
    for (int i = 0; i < size; i++) {
        uint32_t pos = start + (uint32_t)i;
        uint32_t lba = pos / 2048;
        if (!o->cached_valid || o->cached_lba != lba) {
            o->cached_valid = cd_read_sector(pce->cd->image, lba, o->sector) == 0;
            o->cached_lba = lba;
        }
        uint8_t ram = pce_bus_read(pce, huc6280_phys(&pce->cpu, (uint16_t)(pc + i)));
        o->checked++;
        if (!o->cached_valid || o->sector[pos % 2048] != ram) {
            o->mismatches++;
            return ORIGIN_NONE;
        }
    }
    return start;
}
