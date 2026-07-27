#!/usr/bin/env python3
"""Read PokeDNA .rec exports (Emerald Battle Record = raw save sector 31, 4096 B).

Usage:  python3 tools/read_rec.py logs/battles/*.rec
        python3 tools/read_rec.py --json file.rec        (machine-readable)
        python3 tools/read_rec.py --inject file.rec save.sav
            write the record into the save as its "last recorded battle"
            (sector 31 = bytes 0x1F000..0x1FFFF; the game gates the Frontier
            Pass Battle Record purely on this sector's own validity, so a
            valid injected record replays like it was just recorded — back
            up your .sav first; the previous record in it is overwritten)

Pure Python, no deps — works on macOS/Windows/Linux. Layout (little-endian; full
spec in docs/analysis-2026-07-17/record-spec.md):
  +0x000 u32 sentinel 0x0000B39D          +0x004 RecordedBattleSave (3968 B):
    +0    playerParty[6]   6 x 100-B encrypted party mons
    +600  opponentParty[6]
    +1200 playersName[4][8]  +1232 genders[4]  +1236 trainerIds[4] u32
    +1256 rngSeed u32  +1260 battleFlags u32  +1268/70/72 oppA/oppB/partner u16
    +1274 multiplayerId u16  +1276 lvlMode u8  +1277 facility u8
    +1308 battleRecord[4][664] input lanes (0xFF-terminated)
    +3964 checksum u32 = byte-sum of the struct's first 3964 bytes
"""
import struct, sys, json, os

FACILITY = ["Battle Tower", "Battle Dome", "Battle Palace", "Battle Arena",
            "Battle Factory", "Battle Pike", "Battle Pyramid"]

# Gen-3 text charset (the subset used by names)
def g3chr(b):
    if 0xBB <= b <= 0xD4: return chr(ord('A') + b - 0xBB)
    if 0xD5 <= b <= 0xEE: return chr(ord('a') + b - 0xD5)
    if 0xA1 <= b <= 0xAA: return chr(ord('0') + b - 0xA1)
    return {0x00: ' ', 0xAB: '!', 0xAC: '?', 0xAD: '.', 0xAE: '-', 0xB4: "'",
            0xB5: 'M', 0xB6: 'F', 0xB8: ',', 0xBA: '/', 0xF0: ':'}.get(b, '?')

def g3str(raw):
    out = []
    for b in raw:
        if b == 0xFF: break
        out.append(g3chr(b))
    return ''.join(out).strip()

# the 24 substruct orders (Growth/Attacks/EVs/Misc), index = personality % 24
ORDERS = ["GAEM","GAME","GEAM","GEMA","GMAE","GMEA","AGEM","AGME","AEGM","AEMG",
          "AMGE","AMEG","EGAM","EGMA","EAGM","EAMG","EMGA","EMAG","MGAE","MGEA",
          "MAGE","MAEG","MEGA","MEAG"]

def decode_mon(m):
    """m = 100-byte party mon -> dict or None if empty/undecodable."""
    pers, otid = struct.unpack_from("<II", m, 0)
    if pers == 0 and otid == 0 and m[32:80] == b"\x00" * 48:
        return None
    key = pers ^ otid
    words = list(struct.unpack_from("<12I", m, 32))
    dec = struct.pack("<12I", *(w ^ key for w in words))
    ck = sum(struct.unpack("<24H", dec)) & 0xFFFF
    g = ORDERS[pers % 24].index('G') * 12
    species = struct.unpack_from("<H", dec, g)[0]
    if species == 0:
        return None
    return {"species_internal": species, "nickname": g3str(m[8:18]),
            "level": m[84], "checksum_ok": ck == struct.unpack_from("<H", m, 28)[0],
            "shiny": ((otid >> 16) ^ (otid & 0xFFFF) ^ (pers >> 16) ^ (pers & 0xFFFF)) < 8}

def read_rec(path):
    raw = open(path, "rb").read()
    if len(raw) != 4096 or struct.unpack_from("<I", raw, 0)[0] != 0xB39D:
        return {"file": os.path.basename(path), "valid": False}
    r = raw[4:]
    csum = sum(r[:3964]) & 0xFFFFFFFF
    seed, flags = struct.unpack_from("<II", r, 1256)
    oppA, oppB, partner, mpid = struct.unpack_from("<4H", r, 1268)
    names = [g3str(r[1200 + p*8: 1200 + p*8 + 8]) for p in range(4)]
    lanes = []
    for p in range(4):
        lane = r[1308 + p*664: 1308 + (p+1)*664]
        n = 0
        while n < 664 and lane[n] != 0xFF: n += 1
        lanes.append(n)
    teams = {}
    for side, base in (("player", 0), ("opponent", 600)):
        teams[side] = [d for i in range(6)
                       if (d := decode_mon(r[base + i*100: base + (i+1)*100]))]
    return {"file": os.path.basename(path), "valid": True,
            "checksum_ok": csum == struct.unpack_from("<I", r, 3964)[0],
            "facility": FACILITY[r[1277]] if r[1277] < 7 else "?",
            "level_mode": "Open Level" if r[1276] else "Level 50",
            "rng_seed": "%08x" % seed, "battle_flags": "%08x" % flags,
            "opponent_a": oppA, "opponent_b": oppB, "partner": partner,
            "recorded_by": names[mpid] if mpid < 4 else "?",
            "recorder_gender": "F" if mpid < 4 and r[1232 + mpid] else "M",
            "players": [n for n in names if n], "input_lanes": lanes,
            "teams": teams}

def inject(rec_path, sav_path):
    info = read_rec(rec_path)
    if not info["valid"] or not info["checksum_ok"]:
        sys.exit(f"refusing: {rec_path} is not a valid battle record")
    sav = bytearray(open(sav_path, "rb").read())
    if len(sav) < 0x20000:
        sys.exit(f"refusing: {sav_path} is {len(sav)} bytes — a 128 KiB .sav is "
                 "required (64 KiB dumps have no sector 31)")
    sav[0x1F000:0x20000] = open(rec_path, "rb").read()
    open(sav_path, "wb").write(sav)
    print(f"injected {info['file']} ({info['facility']}, seed {info['rng_seed']}) "
          f"into {sav_path} as the last recorded battle")

def main():
    args = [a for a in sys.argv[1:] if a != "--json"]
    as_json = "--json" in sys.argv
    if "--inject" in sys.argv:
        i = sys.argv.index("--inject")
        inject(sys.argv[i + 1], sys.argv[i + 2]); return
    if not args:
        print(__doc__); sys.exit(1)
    for path in args:
        info = read_rec(path)
        if as_json:
            print(json.dumps(info, indent=2)); continue
        if not info["valid"]:
            print(f"{info['file']}: NOT a battle record"); continue
        print(f"{info['file']}: {info['facility']}, {info['level_mode']}, "
              f"seed {info['rng_seed']}, opp #{info['opponent_a']}"
              + (f"+#{info['opponent_b']}" if info['opponent_b'] else "")
              + f", by {info['recorded_by']} ({info['recorder_gender']})"
              + ("" if info["checksum_ok"] else "  [CHECKSUM BAD]"))
        for side in ("player", "opponent"):
            mons = ", ".join(f"{m['nickname'] or '#'+str(m['species_internal'])} "
                             f"Lv{m['level']}" + (" *shiny*" if m["shiny"] else "")
                             for m in info["teams"][side])
            print(f"    {side:8s}: {mons}")
        print(f"    inputs: {[n for n in info['input_lanes'] if n]} bytes/battler")

main()
