"""Function delimitation from an Analysis.

Function entry = target of JSR/BSR, interrupt vector or interrupt entry seen in the
coverage ("sub"/"vec_" labels). The body is everything reachable from the entry by following the
next instruction, branches, jumps and jump tables, without entering calls (JSR/BSR).
A jump to another function's entry is a "tail call": it does not enter the body.

An instruction may belong to more than one function (shared tails); the C generator
emits each function with its own body.
"""
from collections import namedtuple

from .coverage import Point
from .opcodes import FLOW_BRANCH, FLOW_CALL, FLOW_JUMP, FLOW_JUMP_IND, FLOW_NORMAL

Function = namedtuple("Function", "entry body tail_calls")


def _fallthrough(info):
    p, size = info.point, info.ins.opcode.size
    end = p.offset + size
    if end < 0x2000:
        return Point(p.page, p.bank, end)
    return Point((p.page + 1) & 7, info.next_bank, end - 0x2000)


def _partition(analysis, entries):
    """Disc map: each instruction belongs to a single function. Many entries fall in the middle of routines
    (guesses, continuations): the generated function can be started at any point of its body (switch at
    the start, codegen.function_c), so only an entry that no other one reached becomes a new function. Flow
    that reaches the body of another function goes back to the dispatcher, which calls it at that point."""
    code = analysis.code
    load_key = analysis.rom.load_key
    owner = {}
    functions = {}

    def claim(entry):
        body, tails = set(), set()
        addrs = set()  # logical addresses of the body: the same address in another bank goes to another function
        work = [entry]
        while work:
            p = work.pop()
            if p in body or p not in code:
                continue
            if p in owner or (p.page, p.offset) in addrs:
                tails.add(p)
                continue
            body.add(p)
            addrs.add((p.page, p.offset))
            owner[p] = entry
            info = code[p]
            flow = info.ins.opcode.flow
            nxt = []
            if flow in (FLOW_NORMAL, FLOW_BRANCH, FLOW_CALL):
                nxt.append(_fallthrough(info))
            if flow in (FLOW_BRANCH, FLOW_JUMP, FLOW_JUMP_IND):
                nxt += sorted(analysis.targets.get(p, ()))
            for t in nxt:
                if load_key(t.bank) != load_key(entry.bank):
                    continue  # another disc load: another function
                work.append(t)
        functions[entry] = Function(entry, frozenset(body), frozenset(tails))

    # Certain entries first; guesses (Analysis.weak) last: otherwise a guess that is garbage
    # decoded over data right before a routine would own it, and the function's signature (which
    # covers the garbage) would fail when the game changed that data: the routine would fall back to the interpreter
    weak = getattr(analysis, "weak", set())
    for e in sorted(entries, key=lambda q: (q in weak, q)):
        if e not in owner:
            claim(e)
    for p in sorted(code):  # what no entry reaches (e.g. the entry of another load)
        if p not in owner:
            claim(p)
    return functions


def find_functions(analysis):
    code = analysis.code
    entries = sorted(p for p, kinds in analysis.labels.items()
                     if p in code and ("sub" in kinds or any(k.startswith("vec_") for k in kinds)))
    if getattr(analysis, "keep_overlaps", False):
        return _partition(analysis, entries)
    crossings = set()
    functions = _bodies(analysis, entries, crossings)
    # Code no function reaches (e.g. only through an indirect jump, or, in a CD game, the entry of
    # a load, which the BIOS reaches through an RTS/RTI to a pushed address): labeled points and
    # the start of each executed stretch become entries too, until none is left. In a CD game,
    # so does the point of another disc load where the flow enters (a function does not cross loads:
    # its signature only covers its own, and the generator would leave the other part without an owner).
    while True:
        owned = {p for f in functions.values() for p in f.body}
        orphans = {p for p in code if p not in owned}
        followers = {_fallthrough(code[p]) for p in orphans}
        extra = sorted(p for p in orphans
                       if analysis.labels.get(p) or (code[p].executed and p not in followers) or p in crossings)
        if not extra:
            break
        functions = _bodies(analysis, sorted(set(functions) | set(extra)), crossings)
    return functions


def _bodies(analysis, entries, crossings):
    code = analysis.code
    load_key = analysis.rom.load_key
    entry_set = set(entries)
    functions = {}
    for entry in entries:
        body, tails = set(), set()
        work = [entry]
        while work:
            p = work.pop()
            if p in body or p not in code:
                continue
            body.add(p)
            info = code[p]
            flow = info.ins.opcode.flow
            nxt = []
            if flow in (FLOW_NORMAL, FLOW_BRANCH, FLOW_CALL):
                fall = _fallthrough(info)
                if fall in code:
                    nxt.append(fall)
            if flow in (FLOW_BRANCH, FLOW_JUMP, FLOW_JUMP_IND):
                nxt += sorted(analysis.targets.get(p, ()))
            for t in nxt:
                if load_key(t.bank) != load_key(entry.bank):
                    crossings.add(t)  # another disc load: another function
                elif t in entry_set and t != entry and flow in (FLOW_JUMP, FLOW_JUMP_IND):
                    tails.add(t)  # jump to another function
                else:
                    work.append(t)
        functions[entry] = Function(entry, frozenset(body), frozenset(tails))
    return functions


def function_stats(analysis, functions):
    owned = {}
    for f in functions.values():
        for p in f.body:
            owned[p] = owned.get(p, 0) + 1
    orphans = sorted(set(analysis.code) - set(owned))
    sizes = sorted(len(f.body) for f in functions.values())
    return {
        "functions": len(functions),
        "instructions_in_functions": len(owned),
        "shared_instructions": sum(1 for n in owned.values() if n > 1),
        "orphan_instructions": orphans,
        "largest_function": sizes[-1] if sizes else 0,
    }
