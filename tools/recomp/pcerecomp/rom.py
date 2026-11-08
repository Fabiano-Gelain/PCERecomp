"""HuCard ROM reading.

The ROM is split into 8 KB banks. The CPU sees 8 windows of 8 KB (MPR0..MPR7);
each window points to a physical bank (0x00-0xFF). Physical banks 0x00-0x7F are
the ROM, mirrored according to its size (same rule as runtime/pce/pce.c, map_rom).

At reset, MPR7 = 0: addresses $E000-$FFFF show bank 0, where the vectors are.
"""
import hashlib
import zlib

BANK_SIZE = 0x2000

# Interrupt vectors (logical address of the pointer, name)
VECTORS = [
    (0xFFF6, "irq2_brk"),  # IRQ2 (CD-ROM) and the BRK instruction
    (0xFFF8, "irq1_vdc"),  # IRQ1: video (VBlank, scanline, DMA)
    (0xFFFA, "timer"),     # timer
    (0xFFFC, "nmi"),       # NMI (not used on the console)
    (0xFFFE, "reset"),
]


class Rom:
    def __init__(self, data, path=None):
        if len(data) % BANK_SIZE == 512:  # copier header
            data = data[512:]
        if not data or len(data) > 0x80 * BANK_SIZE:
            raise ValueError(f"invalid ROM size: {len(data)} bytes")
        self.path = path
        self.size = len(data)
        self.bank_count = (len(data) + BANK_SIZE - 1) // BANK_SIZE
        padded = data + b"\xff" * (self.bank_count * BANK_SIZE - len(data))
        self.banks = [padded[i * BANK_SIZE:(i + 1) * BANK_SIZE] for i in range(self.bank_count)]
        self.crc32 = zlib.crc32(data) & 0xFFFFFFFF
        self.sha1 = hashlib.sha1(data).hexdigest()

    @classmethod
    def load(cls, path):
        with open(path, "rb") as f:
            return cls(f.read(), path)

    def rom_bank(self, physical_bank):
        """ROM bank (0..bank_count-1) that shows up in physical bank 0x00-0x7F."""
        if not 0 <= physical_bank < 0x80:
            raise ValueError(f"physical bank {physical_bank:#x} is not ROM")
        n, i = self.bank_count, physical_bank
        if n == 0x30:  # 384 KB
            return (i & 0x1F) if i < 0x40 else 0x20 + (i & 0x0F)
        if n == 0x40:  # 512 KB
            return (i & 0x3F) if i < 0x40 else 0x20 + (i & 0x1F)
        if n == 0x60:  # 768 KB
            return (i & 0x3F) if i < 0x40 else 0x40 + (i & 0x1F)
        return i % n

    def read(self, physical_bank, offset):
        """Byte at the physical bank (ROM only) and offset 0..0x1FFF."""
        return self.banks[self.rom_bank(physical_bank)][offset & 0x1FFF]

    def read_logical(self, mpr, address):
        """Byte at logical address 0..0xFFFF, with the given MPRs (ROM banks only)."""
        return self.read(mpr[address >> 13], address & 0x1FFF)

    # Interface shared with CdProgram (program.py): in a HuCard ROM, code = banks $00-$7F.

    def is_code_bank(self, bank):
        return bank < 0x80

    def real_bank(self, bank):
        return bank

    def load_key(self, bank):
        return "rom"

    def next_bank(self, bank):
        return (bank + 1) & 0xFF

    def vectors(self):
        """Interrupt vectors, read from bank 0 as at reset (MPR7 = 0)."""
        out = {}
        for addr, name in VECTORS:
            off = addr & 0x1FFF
            out[name] = self.read(0, off) | (self.read(0, off + 1) << 8)
        return out
