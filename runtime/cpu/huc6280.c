/*
 * HuC6280 CPU interpreter. See huc6280.h.
 *
 * Behavior references: docs/hardware/huc/cpu_inst.txt,
 * reference/Geargrafx/src/huc6280* and the Mesen tests (reference/huc6280-tests).
 *
 * HuC6280 details that differ from the 65C02:
 *  - Zero page is at $2000-$20FF and the stack at $2100-$21FF (logical addresses).
 *  - T flag (bit 5): every instruction clears it, except SET. If T was set,
 *    ORA/AND/EOR/ADC operate on memory at $2000+X instead of the accumulator (+3 cycles).
 *  - ADC/SBC in decimal mode take 1 extra cycle.
 */
#include "huc6280.h"
#include "huc6280_exec.h"

static const int k_speed_divisor[2] = { 12, 3 };

/* ---- Timing and interrupts ---- */

void huc6280_check_irqs(HuC6280 *c)
{
    c->irq_pending = (c->p & HU_FLAG_I) ? 0 : (uint8_t)(c->irq_request & ~c->irq_disable);
}

static void clock_master(HuC6280 *c, uint32_t master)
{
    if (master == 0 || !c->clock)
        return;
    c->clock(c->ctx, master);
    c->clocked_master += master;
    huc6280_check_irqs(c);
}

void huc6280_add_cycles(HuC6280 *c, int n)
{
    c->instr_cycles += n;
    clock_master(c, (uint32_t)(n * k_speed_divisor[c->speed]));
}

void huc6280_clock_counted(HuC6280 *c, int n)
{
    clock_master(c, (uint32_t)(n * k_speed_divisor[c->speed]));
}

void huc6280_clock_pending(HuC6280 *c)
{
    uint32_t target = (uint32_t)(c->instr_cycles * k_speed_divisor[c->speed]) + c->extra_master;
    if (target > c->clocked_master)
        clock_master(c, target - c->clocked_master);
}

void huc6280_stall_fast_cycle(HuC6280 *c)
{
    c->extra_master += 3;
    clock_master(c, 3);
}

/* ---- Bus ---- */

void huc6280_reset(HuC6280 *c)
{
    c->mpr[7] = 0x00;
    c->p = (uint8_t)((c->p | HU_FLAG_I) & ~(HU_FLAG_D | HU_FLAG_T | HU_FLAG_B));
    c->speed = 0;
    c->irq_request = 0;
    c->irq_disable = 0;
    c->irq_pending = 0;
    c->in_block_transfer = 0;
    c->pc = (uint16_t)(rd(c, 0xFFFE) | (rd(c, 0xFFFF) << 8));
}

int huc6280_step(HuC6280 *c)
{
    if (c->hook)
        c->hook(c->ctx, 0);
    uint8_t op = rd(c, c->pc);
    int t = hu_begin(c, op);
    int extra = 0;
    int block = op == 0x73 || op == 0xC3 || op == 0xD3 || op == 0xE3 || op == 0xF3;
    if (block)
        block_push(c); /* before reading the operands, like the console (see block_push) */
    uint8_t b[7] = { op, 0, 0, 0, 0, 0, 0 };
    for (int i = 1; i < k_size[op]; i++)
        b[i] = rd(c, (uint16_t)(c->pc + i));
    c->pc = (uint16_t)(c->pc + k_size[op]);
    if (block) {
        static const int kinds[5] = { BLK_TII, BLK_TDD, BLK_TIN, BLK_TIA, BLK_TAI };
        int k = op == 0x73 ? 0 : op == 0xC3 ? 1 : op == 0xD3 ? 2 : op == 0xE3 ? 3 : 4;
        block_transfer(c, kinds[k], (uint16_t)(b[1] | (b[2] << 8)), (uint16_t)(b[3] | (b[4] << 8)),
                       (uint16_t)(b[5] | (b[6] << 8)));
    } else {
        hu_exec(c, op, b[1], b[2], b[3], b[4], b[5], b[6], t, &extra);
    }
    hu_end(c, extra);
    if (c->hook)
        c->hook(c->ctx, 1);
    return c->instr_cycles;
}

int huc6280_instruction_size(uint8_t opcode)
{
    return k_size[opcode];
}

/* ---- BIOS pseudo-instructions in C (runtime/bios/hle.h) ---- */

void huc6280_hle_begin(HuC6280 *c)
{
    c->p &= (uint8_t)~HU_FLAG_T;
    c->extra_master = 0;
    c->clocked_master = 0;
    c->instr_cycles = 0;
    c->hle_masked = (c->p & HU_FLAG_I) != 0;
    c->hle_irq_snap = 0;
    huc6280_check_irqs(c);
}

int huc6280_hle_end(HuC6280 *c, int cycles)
{
    c->instr_cycles += cycles;
    /* Like RTI/CLI/PLP on the CPU: if interrupts were disabled at the start and this pseudo-instruction
     * enabled them, the pending interrupt is only taken after the next instruction. */
    if (c->hle_irq_snap) /* a single instruction: as on the CPU, the request at its start counts (see hle_instr) */
        c->irq_pending = c->hle_masked ? 0 : (uint8_t)(c->hle_irq_req & ~c->irq_disable);
    else if (c->hle_masked && !(c->p & HU_FLAG_I))
        c->irq_pending = 0;
    else
        huc6280_check_irqs(c);
    hu_end(c, 0);
    return c->instr_cycles;
}
