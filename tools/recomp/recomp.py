"""Recompiler command line.

Usage:
  python tools/recomp/recomp.py info <rom>
      size, banks, CRC32/SHA-1 and interrupt vectors
  python tools/recomp/recomp.py disasm <rom> <address> [count] [--mpr 00,00,...]
      disassembles from a logical address (hex). By default MPR7 = 0 (as at reset) and
      the other windows also show bank 0.
  python tools/recomp/recomp.py coverage <rom> <file or folder> [...] [--cd <image.cue>]
      summarizes and checks coverage files written by the runtime (--coverage); CD games:
      <rom> is the BIOS and --cd checks the code loaded from the disc (coverage v2)
  python tools/recomp/recomp.py analyze <rom> <coverage file or folder> [...] --out <file.asm>
      recursive analysis with bank tracking; writes the annotated disassembly
  python tools/recomp/recomp.py disc <bios> --cd <image.cue> --map games/<game>/disc.toml [--check <coverage>...]
      CD game: finds all the code from the disc map, without play sessions; with --check, shows
      what the sessions executed that the analysis did not find (and how the game got there, with -v)
  python tools/recomp/recomp.py generate <bios> --map games/<game>/disc.toml --cd <image.cue> --out <folder>
      generates the C (or, with coverage files instead of --map, only the code the sessions saw)
"""
import argparse
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pcerecomp.analysis import Analysis  # noqa: E402
from pcerecomp.codegen import CodeGen  # noqa: E402
from pcerecomp.config import Config  # noqa: E402
from pcerecomp.cdimage import CdImage  # noqa: E402
from pcerecomp.coverage import EDGE_KINDS, Coverage, check, check_origins, overlay_loads  # noqa: E402
from pcerecomp.discmap import DiscMap  # noqa: E402
from pcerecomp.disasm import decode  # noqa: E402
from pcerecomp.functions import find_functions, function_stats  # noqa: E402
from pcerecomp.listing import write_listing  # noqa: E402
from pcerecomp.program import CdProgram, ram_linear  # noqa: E402
from pcerecomp.rom import Rom  # noqa: E402


def cmd_info(args):
    rom = Rom.load(args.rom)
    print(f"file:     {args.rom}")
    print(f"size:     {rom.size} bytes ({rom.bank_count} 8 KB banks)")
    print(f"CRC32:    {rom.crc32:08X}")
    print(f"SHA-1:    {rom.sha1}")
    print("vectors:")
    for name, addr in rom.vectors().items():
        print(f"  {name:9s} ${addr:04X}")
    return 0


def cmd_disasm(args):
    rom = Rom.load(args.rom)
    mpr = [int(x, 16) for x in args.mpr.split(",")] if args.mpr else [0] * 8
    pc = int(args.address, 16)
    for _ in range(args.count):
        ins = decode(lambda a: rom.read_logical(mpr, a), pc)
        hexbytes = " ".join(f"{b:02X}" for b in ins.bytes)
        print(f"{ins.pc:04X}  {hexbytes:20s}  {ins.text}")
        pc = (pc + ins.opcode.size) & 0xFFFF
    return 0


def coverage_files(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            files += sorted(os.path.join(p, f) for f in os.listdir(p) if f.endswith(".txt"))
        else:
            files.append(p)
    return files


def cmd_coverage(args):
    rom = Rom.load(args.rom)
    files = coverage_files(args.paths)
    if not files:
        print("no coverage files found")
        return 1
    cov = Coverage.load(files)
    banks, kinds = cov.summary()
    print(f"{len(files)} file(s), {cov.instructions} instructions executed in total")
    print(f"{len(cov.exec)} distinct instructions, by bank:")
    for bank in sorted(banks):
        kind = "ROM" if bank < 0x80 else "RAM"
        print(f"  bank {bank:02X} ({kind}): {banks[bank]}")
    print(f"{len(cov.edges)} distinct control transfers:")
    for k in EDGE_KINDS:
        if k in kinds:
            print(f"  {k} {EDGE_KINDS[k]}: {kinds[k]}")
    problems = check(cov, rom)
    if args.cd:
        # CD game: the code that ran in RAM was loaded from the disc; check it against the image.
        disc = CdImage(args.cd)
        n_none = sum(1 for v in cov.origins.values() if None in v)
        print(f"code in RAM: {len(cov.origins)} addresses, {sum(len(v) for v in cov.origins.values())} "
              f"address/origin pairs; {n_none} addresses with code modified by the game (no origin)")
        loads = overlay_loads(cov)
        print(f"{len(loads)} disc loads with code:")
        for ld in loads:
            lo, hi = ld["disc"]
            banks = ",".join(f"{b:02X}" for b in ld["banks"])
            print(f"  sectors {lo // 2048}-{hi // 2048} -> banks {banks}: {ld['instructions']} instructions")
        cd_problems, stats = check_origins(cov, disc)
        for k, v in stats.items():
            print(f"{k}: {v}")
        problems += cd_problems
    for p in problems[:30]:
        print("PROBLEM: " + p)
    print("RESULT: OK" if not problems else f"RESULT: {len(problems)} problem(s)")
    return 1 if problems else 0


def load_program(args, files):
    """The ROM (HuCard) or, with --cd, the BIOS + the code loaded from the disc (program.py), and the
    matching coverage (in CD games, with the RAM points in virtual banks)."""
    rom = Rom.load(args.rom)
    cov = Coverage.load(files)
    if not getattr(args, "cd", None):
        return rom, cov
    program = CdProgram(rom, CdImage(args.cd), cov)
    st = program.stats()
    print(f"CD: {st['loads']} disc loads with code, {st['virtual_banks']} virtual banks")
    return program, program.virtual_coverage(cov)


def disc_program(args):
    """BIOS + all the disc code from the map: (program, analysis)."""
    rom = Rom.load(args.rom)
    disc = CdImage(args.cd)
    dmap = DiscMap.load(args.map, disc)
    program = CdProgram(rom, disc, discmap=dmap)
    config = Config.load(args.config) if getattr(args, "config", None) else None
    seeds = dmap.seeds(program)
    an = Analysis(program, Coverage(), vectors=False, config=config, seeds=seeds)
    return program, an


def cmd_disc(args):
    program, an = disc_program(args)
    dmap = program.discmap
    st = an.stats()
    fams = {}
    for ld in dmap.loads:
        fams.setdefault(ld.family.name, []).append(ld)
    print(f"map: {len(dmap.loads)} loads (" + ", ".join(f"{k} {len(v)}" for k, v in fams.items()) +
          f"), {len(program.vinfo)} virtual banks, {len(an.seeds)} entries")
    print(f"instructions found: {st['instructions']}; targets with unknown bank: {st['unresolved']}")
    per_load = defaultdict(int)
    for p in an.code:
        per_load[program.load_name(p.bank)] += 1
    if args.verbose:
        for name, n in sorted(per_load.items()):
            print(f"  {name}: {n}")
    if not args.check:
        return 0
    # Check: everything the play sessions executed (with an origin on the disc) must be in the analysis
    cov = Coverage.load(coverage_files(args.check))
    vcov = program.virtual_coverage(cov)
    ram_exec = {p for p in vcov.exec if p.bank >= 0x100}
    missing = sorted(ram_exec - set(an.code))
    outside = defaultdict(int)  # code executed from a load outside the map
    for p, positions in cov.origins.items():
        for pos in positions:
            if pos is not None and program.vbank.get((pos - ram_linear(p.bank, p.offset), p.bank)) is None:
                outside[(dmap.file_of(pos), p.page, p.bank)] += 1
    print(f"check: {len(cov.files)} sessions, {len(ram_exec)} executed instructions from the disc; "
          f"outside the analysis: {len(missing)}; from loads outside the map: {sum(outside.values())}")
    # Code the game modifies: the sessions saw it run with disc bytes and with changed bytes
    seen_mod = {(p.page, p.bank, p.offset) for p, v in cov.origins.items() if None in v and len(v) > 1}
    mod_missing = sorted(seen_mod - an.modified)
    print(f"code modified by the game: {len(seen_mod)} instructions seen, {len(an.modified)} found by the "
          f"analysis, seen but not found: {len(mod_missing)} "
          + " ".join(f"{pg}:{b:02X}:{o:04X}" for pg, b, o in [m for m in mod_missing if m[1] != 0xF8][:24]))
    for (f, page, bank), n in sorted(outside.items(), key=lambda kv: -kv[1])[:15]:
        print(f"  outside the map: file {'?' if f is None else f'${f:02X}'} window {page} bank {bank:02X}: {n}")
    # How the game reached the missing code: coverage edges that enter it from outside it
    # (each one is a "root" to explain)
    miss = set(missing)
    roots = defaultdict(set)
    for e in vcov.edges:
        # RTS/RTI return into the middle of code (after a JSR, or where the interrupt stopped): not a root
        if e.dst in miss and e.src not in miss and e.kind in ("C", "J", "N", "K", "I"):
            src = f"{e.src.page}:{(e.src.page << 13) | e.src.offset:04X} {program.load_name(e.src.bank)}"
            if e.kind in ("C", "J"):
                src = f"from {program.load_name(e.src.bank)} to {program.load_name(e.dst.bank)}"
            roots[(e.kind, src)].add(e.dst)
    if args.verbose:
        for (kind, src), dsts in sorted(roots.items(), key=lambda kv: -len(kv[1]))[:25]:
            loads = defaultdict(list)
            for d in sorted(dsts):
                loads[program.load_name(d.bank)].append(f"{(d.page << 13) | d.offset:04X}")
            ex = "; ".join(f"{k}: {' '.join(v[:4])}" for k, v in sorted(loads.items(), key=lambda kv: -len(kv[1]))[:3])
            print(f"  root {kind} {src}: {len(dsts)} targets ({ex})")
    by_load = defaultdict(list)
    for p in missing:
        by_load[program.load_name(p.bank)].append(p)
    for name, pts in sorted(by_load.items(), key=lambda kv: -len(kv[1])):
        ex = " ".join(f"{p.page}:{p.offset | (p.page << 13):04X}" for p in pts[:args.examples])
        print(f"  {name}: {len(pts)} missing, e.g.: {ex}")
    ok = len(missing) <= args.limit and sum(outside.values()) <= args.limit_outside_map
    print("RESULT: OK" if ok else f"RESULT: more missing than the limit ({args.limit} in the analysis, "
          f"{args.limit_outside_map} outside the map)")
    return 0 if ok else 1


def cmd_generate(args):
    if args.map:
        # CD game with a disc map: all the code, without coverage
        if not args.cd or args.paths:
            print("--map requires --cd and no coverage files")
            return 1
        _program, an = disc_program(args)
    else:
        files = coverage_files(args.paths)
        config = Config.load(args.config) if args.config else None
        rom, cov = load_program(args, files)
        an = Analysis(rom, cov, config=config)
    if an.unresolved:
        print(f"warning: {len(an.unresolved)} targets with unknown bank (the dispatcher resolves them at run time)")
    gen = CodeGen(an, find_functions(an), title=os.path.basename(args.rom))
    out = gen.write(args.out)
    st = gen.stats()
    print(f"generated in {args.out}: {st['functions']} functions, {st['instructions']} instructions, "
          f"{st['table_points']} entry points in the table ({len(out)} files)")
    return 0


def cmd_analyze(args):
    files = coverage_files(args.paths)
    rom, cov = load_program(args, files)
    config = Config.load(args.config) if args.config else None
    an = Analysis(rom, cov, config=config)
    st = an.stats()
    print(f"coverage: {len(files)} file(s), {len(cov.exec)} confirmed instructions")
    if config:
        print(f"config: {args.config} ({len(config.destinations)} targets, {len(config.functions)} functions, "
              f"{len(config.data_regions)} data regions, {len(config.tables)} tables)")
    print(f"instructions found: {st['instructions']} ({st['executed']} executed, {st['static']} deduced)")
    print(f"code bytes: {st['code_bytes']} of {rom.size} ({100 * st['code_bytes'] / rom.size:.1f}% of the ROM)")
    print(f"conflicts: {st['dropped_conflicts']} deduced dropped, "
          f"{st['executed_conflicts']} between executed")
    print(f"targets with unknown bank: {st['unresolved']}; targets in RAM: {st['ram_targets']}")
    for u in an.unresolved[:10]:
        print(f"  unresolved: {u.src.page}:{u.src.bank:02X}:{u.src.offset:04X} -> ${u.target:04X} ({u.reason})")
    print(f"banks deduced from context learned from coverage: {st['resolved_by_context']}; "
          f"by single candidate: {st['resolved_by_candidate']}")
    print(f"jump tables read from the ROM: {st['tables_read']} ({st['table_targets']} targets); "
          f"diverging from coverage: {len(st['diverging_tables'])}")
    functions = find_functions(an)
    fst = function_stats(an, functions)
    print(f"functions: {fst['functions']} (largest: {fst['largest_function']} instructions); "
          f"instructions outside functions: {len(fst['orphan_instructions'])}")
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        write_listing(an, args.out, title=os.path.basename(args.rom), functions=functions)
        print(f"listing written to {args.out}")
    errors = []
    if st["missing_executed"]:
        errors.append(f"{len(st['missing_executed'])} executed instructions were left out of the analysis")
    # In CD games, two situations are normal and only become warnings: jump tables stored in RAM
    # (e.g. interrupt hooks the BIOS calls), which cannot be read from the disc (the targets come
    # from coverage), and the 6502 "skip a byte" trick (entering the middle of a BIT #n), which makes
    # two executed instructions overlap. The dispatcher checks everything at run time.
    cd = getattr(args, "cd", None)
    problems = errors if not cd else []
    if st["diverging_tables"]:
        problems.append(f"{len(st['diverging_tables'])} jump tables do not contain all the coverage targets")
    if st["executed_conflicts"]:
        problems.append(f"{st['executed_conflicts']} executed instructions overlap")
    if cd:
        for p in problems:
            print("warning: " + p)
    for rule in st["config_unused"]:
        errors.append(f"config rule never used (typo?): {rule}")
    for p in st["config_data_executed"]:
        errors.append(f"the config marks as data a point that was executed: {p}")
    for e in errors:
        print("ERROR: " + e)
    if errors:
        return 1
    print("RESULT: OK (all executed instructions found, tables match the coverage)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info")
    p.add_argument("rom")
    p.set_defaults(func=cmd_info)
    p = sub.add_parser("disasm")
    p.add_argument("rom")
    p.add_argument("address")
    p.add_argument("count", nargs="?", type=int, default=20)
    p.add_argument("--mpr", help="8 banks in hex separated by commas (MPR0..MPR7)")
    p.set_defaults(func=cmd_disasm)
    p = sub.add_parser("coverage")
    p.add_argument("rom")
    p.add_argument("paths", nargs="+")
    p.add_argument("--cd", help="CD image (.cue): checks the code that ran in RAM by its position on the disc")
    p.set_defaults(func=cmd_coverage)
    p = sub.add_parser("analyze")
    p.add_argument("rom")
    p.add_argument("paths", nargs="+")
    p.add_argument("--out")
    p.add_argument("--config", help="manual game config (games/<game>/config.toml)")
    p.add_argument("--cd", help="CD game: .cue image (rom = BIOS); also analyzes the code loaded from the disc")
    p.set_defaults(func=cmd_analyze)
    p = sub.add_parser("disc")
    p.add_argument("rom", help="BIOS")
    p.add_argument("--cd", required=True, help="CD image (.cue)")
    p.add_argument("--map", required=True, help="disc map (games/<game>/disc.toml)")
    p.add_argument("--config")
    p.add_argument("--check", nargs="*", help="coverage of play sessions: what they executed must have been found")
    p.add_argument("--examples", type=int, default=8)
    p.add_argument("--limit", type=int, default=0, help="accepted executed instructions outside the analysis")
    p.add_argument("--limit-outside-map", type=int, default=0, help="accepted instructions from loads outside the map")
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(func=cmd_disc)
    p = sub.add_parser("generate")
    p.add_argument("rom")
    p.add_argument("paths", nargs="*")
    p.add_argument("--map", help="CD game: disc map (games/<game>/disc.toml) instead of coverage")
    p.add_argument("--out", required=True, help="output folder for the generated C")
    p.add_argument("--config")
    p.add_argument("--cd", help="CD game: .cue image (rom = BIOS)")
    p.set_defaults(func=cmd_generate)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
