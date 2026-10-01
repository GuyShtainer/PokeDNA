#!/usr/bin/env python3
"""Generate the PokeDNA HW-test tracker page (docs/hw-tracker.html) from docs/HW-QUEUE.md.

The page is published as a claude.ai Artifact with the db/assets capabilities; row
DEFINITIONS are baked here, all mutable state (Guy's sign-offs, Claude's emu statuses,
findings, notes) lives in the artifact's shared db and is overlaid at view time.
Run from the PokeDNA repo root:  python3 tools/hw_tracker_gen.py
"""
import html
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "docs" / "HW-QUEUE.md"
OUT = ROOT / "docs" / "hw-tracker.html"

# ---------------------------------------------------------------- parse

def split_row(line: str):
    # split on pipes that are not escaped as \|
    cells = re.split(r"(?<!\\)\|", line.strip())
    cells = [c.strip().replace("\\|", "|") for c in cells]
    # leading/trailing empties from the border pipes
    if cells and cells[0] == "":
        cells = cells[1:]
    if cells and cells[-1] == "":
        cells = cells[:-1]
    return cells


def parse(src_text: str):
    rows = []
    seen = {}
    build_line = ""
    for line in src_text.splitlines():
        if not build_line and line.startswith("Build on OneDrive:"):
            m = re.match(r"Build on OneDrive: `([0-9a-f]+)` \(([0-9-]+);", line)
            if m:
                build_line = f"{m.group(1)} ({m.group(2)})"
        if not line.startswith("| "):
            continue
        cells = split_row(line)
        if len(cells) < 8 or cells[0] in ("ID", "---") or set(cells[0]) <= {"-"}:
            continue
        rid = cells[0]
        n = seen.get(rid, 0) + 1
        seen[rid] = n
        uid = rid if n == 1 else f"{rid}~{n}"
        rows.append({
            "id": uid, "base": rid, "kind": cells[1], "what": cells[2],
            "commit": cells[3].strip("`"), "check": cells[4], "expect": cells[5],
            "steps": cells[6], "status": cells[7],
        })
    return rows, build_line


# ---------------------------------------------------------------- classify
# sections: g3 = Gen 3, gb = Gen 1/2, xg = cross-gen (Bank & transfers), sys = all-gens/system
PREFIX = [
    ("xg", ("XFER", "BANK", "SERIAL-RESYNC", "META-BAK", "LOSS-", "CLIP-", "ARRIVE-",
            "FIRSTLIFT", "GBIMP", "ITEM-", "ORIGIN-", "GLYPH-2", "GLYPH-6", "GLYPH-7",
            "TINY2", "GHOST-", "GLOVE-")),
    ("gb", ("GBUI", "GBDEX", "GBMON", "GBMOVE", "GBNAMES", "GBROM", "GBSCR", "GBART",
            "DAYCARE-GB", "HOF-", "DEX-GB", "MAP-G1", "MAP-G2", "MAP1-", "RELEASE-1",
            "PARTY-MAIL", "DEXART", "CARD-GB", "GLYPH-5", "HW-B86", "B90-", "B94-")),
    ("g3", ("RS-ART", "CONTEST", "BAG-", "MOVEGEN", "MOVEPICK", "MOVEROW", "SHEDINJA",
            "BOXFULL", "GRIDOPS", "ROMHACK", "GLYPH-3", "GLYPH-4", "CREATE-")),
    ("sys", ("UNDO-", "BOOT-", "STACK-", "SPEED-", "WRGATE", "P-MEASURE", "IME-",
             "REPAINT", "PHANTOM", "FOOTER-", "RDONLY", "SPRITE-", "FBGBA",
             "DEX-DETAIL", "PICKUI", "EDITRET", "EDITSEL")),
]
RE_XG = re.compile(r"\bBank\b|transfer|bridge|\.pds|ledger|sidecar|reconcile|\bUP move\b", re.I)
RE_GB = re.compile(r"Game Boy|Gen-? ?1\b|Gen-? ?2\b|Gen-1/2|\bGold\b|\bCrystal\b|\bYellow\b|\bRed\.s|\bRed\.gb|\.gbc?\b")
RE_G3 = re.compile(r"Emerald|\bRuby\b|Sapphire|FireRed|LeafGreen|FRLG|Gen-? ?3\b|R/S\b")


def classify(r):
    for sec, prefixes in PREFIX:
        for p in prefixes:
            if r["base"].startswith(p):
                return sec
    t = " ".join((r["what"], r["check"], r["expect"]))
    xg, gb, g3 = len(RE_XG.findall(t)), len(RE_GB.findall(t)), len(RE_G3.findall(t))
    if xg >= 2 and gb and g3:
        return "xg"
    if gb > g3:
        return "gb"
    if g3 > gb:
        return "g3"
    if xg:
        return "xg"
    return "sys"


RE_EMU = re.compile(r"[^.]*?(?:emulat|HW-only|HW-owed|hardware-only|hardware alone|"
                    r"delta has no FAT|only proof|never runs on|cart-only)[^.]*\.", re.I)


def emu_hint(r):
    hits = RE_EMU.findall(r["check"] + " " + r["expect"])
    return " ".join(h.strip() for h in hits[:2])[:300]


# ---------------------------------------------------------------- inline md -> html

def md(s):
    s = html.escape(s, quote=False)
    s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
    s = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", s)
    return s


def main():
    rows, build_line = parse(SRC.read_text(encoding="utf-8"))
    for r in rows:
        r["sec"] = classify(r)
        r["emu"] = emu_hint(r)
        stat_up = r["status"].upper()
        retired = "SUPERSEDED" in r["what"].upper() or "RETIRED" in r["what"].upper()
        r["baked"] = ("pass" if "HW-PASS" in stat_up else
                      "retired" if retired or stat_up not in ("HW-PEND", "HW-PASS", "") else
                      "pend")
        for k in ("what", "check", "expect", "steps"):
            r[k] = md(r[k])
    counts = {}
    for r in rows:
        counts[r["sec"]] = counts.get(r["sec"], 0) + 1
    print(f"rows={len(rows)} by section: {counts}  build='{build_line}'", file=sys.stderr)
    for sec in ("g3", "gb", "xg", "sys"):
        ids = [r["id"] for r in rows if r["sec"] == sec]
        print(f"  {sec}: {' '.join(ids)}", file=sys.stderr)

    rows_json = json.dumps(rows, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")
    page = TEMPLATE.replace("__ROWS__", rows_json).replace("__BUILD__", html.escape(build_line))
    OUT.write_text(page, encoding="utf-8")
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes)", file=sys.stderr)


TEMPLATE = r"""<title>PokeDNA HW Tests</title>
<style>
/* Layout: one column of gen sections, sticky header with progress; rows are sign-off cards. */
:root {
  --bg:#f5f3ee; --panel:#ffffff; --ink:#23242b; --muted:#6d6f7a; --line:#dcd9d0;
  --accent:#5b4bd4; --accent-ink:#ffffff; --chip:#ecebf3;
  --pass:#1d7a3e; --pass-bg:#e2f3e7; --fail:#b3261e; --fail-bg:#f9e3e1;
  --skip:#8a6d1a; --skip-bg:#f5eccc; --walled:#7a4fb0; --walled-bg:#efe6f8;
  --code:#f0eee7;
}
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {
  --bg:#17181d; --panel:#1f2128; --ink:#e8e7ef; --muted:#9a9ca8; --line:#34363f;
  --accent:#8d80ec; --accent-ink:#17181d; --chip:#2a2c36;
  --pass:#6fd394; --pass-bg:#1c2f24; --fail:#f1958e; --fail-bg:#3a2321;
  --skip:#dec36a; --skip-bg:#36301c; --walled:#c5a5ee; --walled-bg:#2d2338;
  --code:#262830; color-scheme: dark;
} }
:root[data-theme="dark"] {
  --bg:#17181d; --panel:#1f2128; --ink:#e8e7ef; --muted:#9a9ca8; --line:#34363f;
  --accent:#8d80ec; --accent-ink:#17181d; --chip:#2a2c36;
  --pass:#6fd394; --pass-bg:#1c2f24; --fail:#f1958e; --fail-bg:#3a2321;
  --skip:#dec36a; --skip-bg:#36301c; --walled:#c5a5ee; --walled-bg:#2d2338;
  --code:#262830; color-scheme: dark;
}
body { background:var(--bg); color:var(--ink); font:15px/1.5 "IBM Plex Sans",system-ui,sans-serif;
  margin:0; padding:0 16px 48px; }
.wrap { max-width:980px; margin:0 auto; }
h1 { font-family:"Silkscreen",monospace; font-size:1.35rem; letter-spacing:.5px; margin:0; }
h2 { font-family:"Silkscreen",monospace; font-size:.95rem; margin:0; letter-spacing:.5px; }
code { background:var(--code); padding:0 .3em; border-radius:4px; font:.88em "IBM Plex Mono",monospace; }
.hdr { position:sticky; top:env(safe-area-inset-top,0px); background:var(--bg); z-index:5;
  padding:10px 0 8px; border-bottom:2px solid var(--line); }
.hdr-top { display:flex; flex-wrap:wrap; gap:8px 16px; align-items:baseline; }
.build { color:var(--muted); font-size:.82rem; }
.progress { display:flex; flex-wrap:wrap; gap:6px; margin-top:6px; font-size:.8rem; }
.pchip { padding:1px 8px; border-radius:99px; background:var(--chip); }
.pchip.pass { background:var(--pass-bg); color:var(--pass); }
.pchip.fail { background:var(--fail-bg); color:var(--fail); }
.pchip.skip { background:var(--skip-bg); color:var(--skip); }
.bar { display:flex; flex-wrap:wrap; gap:8px; margin-top:8px; align-items:center; }
.bar input[type=search] { flex:1 1 180px; min-width:0; padding:5px 10px; border:1px solid var(--line);
  border-radius:8px; background:var(--panel); color:var(--ink); font:inherit; }
.seg { display:flex; flex-wrap:wrap; gap:0; border:1px solid var(--line); border-radius:8px; overflow:hidden; }
.seg button { border:0; background:var(--panel); color:var(--muted); padding:5px 10px; font:.82rem "IBM Plex Sans",sans-serif; cursor:pointer; }
.seg button.on { background:var(--accent); color:var(--accent-ink); }
.card { background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:12px 14px; margin:14px 0; }
.note-claude { font-size:.88rem; }
.sec { margin-top:26px; }
.sec-h { display:flex; flex-wrap:wrap; gap:8px 12px; align-items:center; cursor:pointer;
  border-bottom:2px solid var(--accent); padding-bottom:6px; }
.sec-h .cnt { color:var(--muted); font-size:.8rem; }
.row { background:var(--panel); border:1px solid var(--line); border-left:4px solid var(--line);
  border-radius:8px; margin:10px 0; padding:10px 12px; }
.row.st-pass { border-left-color:var(--pass); }
.row.st-fail { border-left-color:var(--fail); }
.row.st-skip { border-left-color:var(--skip); }
.row.retired { opacity:.55; }
.row-h { display:flex; flex-wrap:wrap; gap:6px 10px; align-items:center; }
.rid { font:.8rem "IBM Plex Mono",monospace; background:var(--chip); border-radius:5px; padding:1px 7px; }
.kind { font-size:.72rem; text-transform:uppercase; letter-spacing:.6px; color:var(--muted); }
.chip { font-size:.74rem; border-radius:99px; padding:1px 8px; background:var(--chip); white-space:nowrap; }
.chip.pass { background:var(--pass-bg); color:var(--pass); }
.chip.fail { background:var(--fail-bg); color:var(--fail); }
.chip.skip { background:var(--skip-bg); color:var(--skip); }
.chip.walled { background:var(--walled-bg); color:var(--walled); }
.what { margin:6px 0 0; font-size:.92rem; }
.more { margin-top:8px; font-size:.88rem; }
.more dt { font-weight:600; margin-top:8px; color:var(--muted); font-size:.78rem; text-transform:uppercase; letter-spacing:.5px; }
.more dd { margin:2px 0 0; overflow-wrap:anywhere; }
.emu { margin-top:6px; font-size:.82rem; color:var(--walled); }
.claude-note { margin-top:6px; font-size:.85rem; }
.claude-note b { color:var(--accent); }
.ctl { display:flex; flex-wrap:wrap; gap:8px; margin-top:10px; align-items:center; }
.ctl .sgn { display:flex; border:1px solid var(--line); border-radius:8px; overflow:hidden; }
.ctl .sgn button { border:0; background:var(--panel); color:var(--muted); padding:6px 14px; font:700 .82rem "IBM Plex Sans",sans-serif; cursor:pointer; }
.ctl .sgn button.on-pass { background:var(--pass); color:#fff; }
.ctl .sgn button.on-fail { background:var(--fail); color:#fff; }
.ctl .sgn button.on-skip { background:var(--skip); color:#fff; }
.ctl .when { font-size:.76rem; color:var(--muted); }
.ctl .exp { margin-left:auto; border:0; background:none; color:var(--accent); cursor:pointer; font:.82rem "IBM Plex Sans",sans-serif; }
.notes { margin-top:8px; }
.notes textarea { width:100%; box-sizing:border-box; min-height:38px; border:1px solid var(--line);
  border-radius:8px; background:var(--bg); color:var(--ink); font:.88rem "IBM Plex Sans",sans-serif; padding:6px 8px; }
.notes .st { font-size:.74rem; color:var(--muted); }
.photos { display:flex; flex-wrap:wrap; gap:6px; margin-top:6px; }
.photos img { height:64px; border-radius:6px; border:1px solid var(--line); cursor:pointer; }
.photos img.big { height:auto; width:100%; max-width:480px; }
.photos .ph { position:relative; }
.photos .ph button { position:absolute; top:-6px; right:-6px; border:0; border-radius:99px;
  background:var(--fail); color:#fff; width:18px; height:18px; line-height:16px; font-size:11px; cursor:pointer; padding:0; }
.addph { font-size:.8rem; }
.offline { background:var(--skip-bg); color:var(--skip); border-radius:8px; padding:8px 12px; margin:12px 0; font-size:.86rem; }
.fnd { border-top:1px dashed var(--line); padding:8px 0; font-size:.88rem; }
.fnd img { max-width:100%; border-radius:6px; border:1px solid var(--line); margin-top:4px; }
.sev { font-size:.72rem; border-radius:99px; padding:1px 8px; margin-right:6px; }
.sev.blocker { background:var(--fail-bg); color:var(--fail); }
.sev.visual { background:var(--skip-bg); color:var(--skip); }
.sev.question { background:var(--chip); color:var(--muted); }
button:focus-visible, input:focus-visible, textarea:focus-visible { outline:2px solid var(--accent); outline-offset:1px; }
@media (prefers-reduced-motion: no-preference) { .row { transition:border-color .2s; } }
</style>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Silkscreen&family=IBM+Plex+Sans:wght@400;600;700&family=IBM+Plex+Mono&display=swap">
<div class="wrap">
  <header class="hdr">
    <div class="hdr-top">
      <h1>PokeDNA HW Tests</h1>
      <span class="build">build <b id="build">__BUILD__</b> · Omega DE · <span id="sync"></span></span>
    </div>
    <div class="progress" id="progress"></div>
    <div class="bar">
      <input type="search" id="q" placeholder="Search ID or text&hellip;" aria-label="Search rows">
      <div class="seg" id="filter" role="group" aria-label="Status filter"></div>
    </div>
  </header>

  <div id="offline" class="offline" hidden>Read-only view: open this page signed in on claude.ai to save sign-offs, notes and photos.</div>

  <section class="card" id="claude-card">
    <h2>Claude's side</h2>
    <p class="note-claude" id="claude-note">Loading&hellip;</p>
    <div id="findings"></div>
  </section>

  <section class="card">
    <h2>Note to Claude</h2>
    <p class="note-claude" style="color:var(--muted)">Anything you want me to pick up at the next 7:00 / 22:00 sync &mdash; questions, rulings, session summaries.</p>
    <div class="notes"><textarea id="guy-note" placeholder="Write here&hellip;"></textarea>
    <div class="st" id="guy-note-st"></div></div>
  </section>

  <div id="sections"></div>
</div>
<script>
"use strict";
var ROWS = __ROWS__;
var SECS = { g3:"Gen 3 (Emerald · R/S · FR/LG)", gb:"Gen 1/2 (Game Boy)",
             xg:"Cross-gen (Bank & transfers)", sys:"All gens & system (undo · journal · boot)" };
var state = { signoffs:{}, claude:{}, meta:null, findings:{}, guyNote:null };
var db = null, assetsNS = null, canWrite = false;
var versions = {};           // doc path -> last seen (unused with capability db; kept for clarity)
var saveTimers = {};

function lsGet(k){ try { return localStorage.getItem(k); } catch(e){ return null; } }
function lsSet(k,v){ try { localStorage.setItem(k,v); } catch(e){} }

function domId(id){ return id.replace(/[^A-Za-z0-9_-]/g, "-"); }
function esc(s){ var d=document.createElement("span"); d.textContent=s==null?"":String(s); return d.innerHTML; }

// ------------------------------------------------- render skeleton
function rowGuy(id){ return state.signoffs[id] || null; }
function effStatus(r){
  var g = rowGuy(r.id);
  if (g && g.status) return g.status;
  if (r.baked === "pass") return "pass";
  return r.baked === "retired" ? "retired" : "";
}

function renderAll(){
  var host = document.getElementById("sections");
  host.innerHTML = "";
  ["g3","gb","xg","sys"].forEach(function(sec){
    var rows = ROWS.filter(function(r){ return r.sec === sec; });
    var el = document.createElement("section");
    el.className = "sec"; el.id = "sec-" + sec;
    el.innerHTML = '<div class="sec-h" role="button" tabindex="0" aria-expanded="true">' +
      "<h2>" + esc(SECS[sec]) + '</h2><span class="cnt" id="cnt-' + sec + '"></span></div>' +
      '<div class="sec-b" id="secb-' + sec + '"></div>';
    host.appendChild(el);
    var body = el.querySelector(".sec-b");
    rows.forEach(function(r){ body.appendChild(rowEl(r)); });
    var h = el.querySelector(".sec-h");
    function toggle(){ var b=el.querySelector(".sec-b"); b.hidden=!b.hidden;
      h.setAttribute("aria-expanded", String(!b.hidden)); lsSet("col-"+sec, b.hidden?"1":"0"); }
    h.addEventListener("click", toggle);
    h.addEventListener("keydown", function(e){ if(e.key==="Enter"||e.key===" "){ e.preventDefault(); toggle(); } });
    if (lsGet("col-"+sec) === "1") toggle();
  });
  applyAllRows(); refreshCounts(); applyFilter();
}

function rowEl(r){
  var d = domId(r.id);
  var el = document.createElement("article");
  el.className = "row" + (r.baked === "retired" ? " retired" : "");
  el.id = "row-" + d;
  el.innerHTML =
    '<div class="row-h"><span class="rid">' + esc(r.id) + '</span>' +
    '<span class="kind">' + esc(r.kind) + '</span>' +
    '<span class="chip st" id="st-' + d + '"></span>' +
    '<span class="chip walled" id="cl-' + d + '" hidden></span></div>' +
    '<p class="what">' + r.what + '</p>' +
    (r.emu ? '<p class="emu">emu note: ' + r.emu + '</p>' : '') +
    '<p class="claude-note" id="cn-' + d + '" hidden></p>' +
    '<dl class="more" id="more-' + d + '" hidden>' +
    '<dt>Check on the cart</dt><dd>' + r.check + '</dd>' +
    '<dt>Expect</dt><dd>' + r.expect + '</dd>' +
    (r.steps && r.steps !== "—" ? '<dt>Steps</dt><dd>' + r.steps + '</dd>' : '') +
    '<dt>Commit</dt><dd><code>' + esc(r.commit) + '</code> · queue status: ' + esc(r.status) + '</dd></dl>' +
    '<div class="ctl">' +
      '<div class="sgn" role="group" aria-label="Sign off ' + esc(r.id) + '">' +
        '<button type="button" data-s="pass" id="bp-' + d + '">PASS</button>' +
        '<button type="button" data-s="fail" id="bf-' + d + '">FAIL</button>' +
        '<button type="button" data-s="skip" id="bs-' + d + '">SKIP</button></div>' +
      '<span class="when" id="when-' + d + '"></span>' +
      '<button type="button" class="exp" id="exp-' + d + '" aria-expanded="false">details</button></div>' +
    '<div class="notes"><textarea id="note-' + d + '" placeholder="Notes (what you saw)&hellip;"></textarea>' +
      '<div class="st" id="notest-' + d + '"></div>' +
      '<div class="photos" id="ph-' + d + '"></div>' +
      '<label class="addph"><input type="file" id="file-' + d + '" accept="image/*" multiple hidden>' +
      '<button type="button" id="addph-' + d + '" hidden>+ add photo</button></label></div>';
  // wire
  el.querySelector("#exp-" + d).addEventListener("click", function(){
    var m = el.querySelector("#more-" + d); m.hidden = !m.hidden;
    this.setAttribute("aria-expanded", String(!m.hidden));
    this.textContent = m.hidden ? "details" : "hide";
  });
  ["pass","fail","skip"].forEach(function(s){
    el.querySelector('[data-s="' + s + '"]').addEventListener("click", function(){ sign(r, s); });
  });
  var ta = el.querySelector("#note-" + d);
  ta.addEventListener("input", function(){ queueNote(r, ta.value); });
  var fi = el.querySelector("#file-" + d);
  el.querySelector("#addph-" + d).addEventListener("click", function(){ fi.click(); });
  fi.addEventListener("change", function(){ addPhotos(r, fi.files); fi.value = ""; });
  return el;
}

// ------------------------------------------------- db overlay
function applyRow(id){
  var r = ROWS.find(function(x){ return x.id === id; });
  if (!r) return;
  var d = domId(id), g = rowGuy(id), c = state.claude[id];
  var el = document.getElementById("row-" + d);
  if (!el) return;
  var st = effStatus(r);
  el.classList.remove("st-pass","st-fail","st-skip");
  if (st === "pass" || st === "fail" || st === "skip") el.classList.add("st-" + st);
  var chip = document.getElementById("st-" + d);
  var lbl = { pass:"PASS", fail:"FAIL", skip:"SKIP", retired:"retired", "":"needs cart" };
  chip.textContent = (g && g.status) ? lbl[g.status] : (r.baked === "pass" ? "PASS (earlier session)" : lbl[st]);
  chip.className = "chip st " + (st === "pass" ? "pass" : st === "fail" ? "fail" : st === "skip" ? "skip" : "");
  document.getElementById("when-" + d).textContent = g && g.at ? "signed " + g.at : "";
  ["pass","fail","skip"].forEach(function(s){
    var b = el.querySelector('[data-s="' + s + '"]');
    b.className = (g && g.status === s) ? "on-" + s : "";
  });
  var ta = document.getElementById("note-" + d);
  if (g && typeof g.note === "string" && document.activeElement !== ta) ta.value = g.note;
  // claude layer
  var cchip = document.getElementById("cl-" + d), cnote = document.getElementById("cn-" + d);
  if (c && c.state) {
    cchip.hidden = false;
    cchip.textContent = { ok:"Claude: emu ✓", partial:"Claude: emu partial", walled:"Claude: emu can't prove" }[c.state] || ("Claude: " + c.state);
    cchip.className = "chip " + (c.state === "ok" ? "pass" : "walled");
  } else cchip.hidden = true;
  if (c && c.note) { cnote.hidden = false; cnote.innerHTML = "<b>Claude:</b> " + esc(c.note) + (c.at ? ' <span style="color:var(--muted)">(' + esc(c.at) + ")</span>" : ""); }
  else cnote.hidden = true;
  // photos
  var ph = document.getElementById("ph-" + d);
  ph.innerHTML = "";
  ((g && g.images) || []).forEach(function(aid){
    var w = document.createElement("span"); w.className = "ph";
    var img = document.createElement("img"); img.src = "/_blob/" + aid; img.alt = "photo for " + r.id;
    img.addEventListener("click", function(){ img.classList.toggle("big"); });
    w.appendChild(img);
    if (canWrite) { var x = document.createElement("button"); x.textContent = "×"; x.title = "remove";
      x.addEventListener("click", function(){ removePhoto(r, aid); }); w.appendChild(x); }
    ph.appendChild(w);
  });
}
function applyAllRows(){ ROWS.forEach(function(r){ applyRow(r.id); }); }

function refreshCounts(){
  var tot = { pass:0, fail:0, skip:0, pend:0, retired:0 };
  var per = {};
  ROWS.forEach(function(r){
    var st = effStatus(r) || "pend";
    if (st === "retired") st = "retired";
    per[r.sec] = per[r.sec] || { pass:0, fail:0, skip:0, pend:0, retired:0 };
    per[r.sec][st === "" ? "pend" : st] += 1;
    tot[st === "" ? "pend" : st] += 1;
  });
  var p = document.getElementById("progress");
  p.innerHTML = '<span class="pchip pass">' + tot.pass + " passed</span>" +
    '<span class="pchip fail">' + tot.fail + " failed</span>" +
    '<span class="pchip skip">' + tot.skip + " skipped</span>" +
    '<span class="pchip">' + tot.pend + " need the cart</span>" +
    '<span class="pchip">' + tot.retired + " retired</span>";
  Object.keys(per).forEach(function(sec){
    var c = per[sec], el = document.getElementById("cnt-" + sec);
    if (el) el.textContent = c.pass + " passed · " + c.fail + " failed · " + c.pend + " to go";
  });
}

// ------------------------------------------------- writes
function nowStr(){ return new Date().toISOString().slice(0, 16).replace("T", " "); }
function signDoc(id){ var g = state.signoffs[id] || {}; return { status: g.status || "", note: g.note || "", images: g.images || [], at: g.at || "" }; }
function writeSign(r){
  if (!db || !canWrite) return;
  var doc = signDoc(r.id);
  db.doc("signoffs/" + r.id).set(doc).catch(function(e){
    if (e && (e.code === "invalid_argument" || e.code === "not_granted")) { canWrite = false; markOffline(); }
  });
}
function sign(r, s){
  if (!canWrite) return;
  var g = state.signoffs[r.id] = signDoc(r.id);
  g.status = (g.status === s) ? "" : s;
  g.at = g.status ? nowStr() : "";
  applyRow(r.id); refreshCounts(); writeSign(r);
}
function queueNote(r, text){
  if (!canWrite) return;
  var g = state.signoffs[r.id] = signDoc(r.id);
  g.note = text;
  var st = document.getElementById("notest-" + domId(r.id));
  st.textContent = "saving…";
  clearTimeout(saveTimers[r.id]);
  saveTimers[r.id] = setTimeout(function(){ writeSign(r); st.textContent = "saved ✓";
    setTimeout(function(){ if (st.textContent === "saved ✓") st.textContent = ""; }, 2500); }, 900);
}
function addPhotos(r, files){
  if (!assetsNS || !canWrite || !files || !files.length) return;
  var st = document.getElementById("notest-" + domId(r.id));
  st.textContent = "uploading…";
  var list = Array.prototype.slice.call(files);
  (function next(){
    var f = list.shift();
    if (!f) { st.textContent = ""; writeSign(r); applyRow(r.id); return; }
    assetsNS.upload(f).then(function(res){
      var g = state.signoffs[r.id] = signDoc(r.id);
      g.images = (g.images || []).concat([res.id]);
      next();
    }).catch(function(){ st.textContent = "upload failed"; next(); });
  })();
}
function removePhoto(r, aid){
  var g = state.signoffs[r.id] = signDoc(r.id);
  g.images = (g.images || []).filter(function(x){ return x !== aid; });
  applyRow(r.id); writeSign(r);
}

// ------------------------------------------------- claude card + findings
function renderClaude(){
  var m = state.meta;
  var el = document.getElementById("claude-note");
  el.textContent = m && m.claude_note ? m.claude_note : "No sync yet.";
  document.getElementById("sync").textContent = m && m.last_sync ? "last sync " + m.last_sync : "";
  if (m && m.build) document.getElementById("build").textContent = m.build;
}
function renderFindings(){
  var host = document.getElementById("findings");
  var keys = Object.keys(state.findings).sort(function(a,b){ return (state.findings[a].n||0)-(state.findings[b].n||0); });
  host.innerHTML = keys.length ? "<h2 style='margin-top:12px'>RC QA findings (mGBA+VSD)</h2>" : "";
  keys.forEach(function(k){
    var f = state.findings[k];
    var d = document.createElement("div"); d.className = "fnd";
    d.innerHTML = '<span class="sev ' + esc(f.sev || "question") + '">' + esc(f.sev || "?") + "</span>" +
      "<b>FND-" + esc(f.n) + (f.item ? " · #" + esc(f.item) : "") + ":</b> " + esc(f.title || "") +
      (f.detail ? '<div style="color:var(--muted)">' + esc(f.detail) + "</div>" : "");
    if (f.img) { var img = document.createElement("img"); img.loading = "lazy"; img.src = "/_blob/" + f.img; img.alt = "finding " + f.n; d.appendChild(img); }
    if (f.status && f.status !== "open") d.innerHTML += ' <span class="chip pass">' + esc(f.status) + "</span>";
    host.appendChild(d);
  });
}

// ------------------------------------------------- guy note
var guyTimer = null;
function wireGuyNote(){
  var ta = document.getElementById("guy-note"), st = document.getElementById("guy-note-st");
  ta.addEventListener("input", function(){
    if (!canWrite) return;
    st.textContent = "saving…";
    clearTimeout(guyTimer);
    guyTimer = setTimeout(function(){
      db.doc("notes/guy").set({ text: ta.value, at: nowStr() }).then(function(){ st.textContent = "saved ✓"; })
        .catch(function(){ st.textContent = "could not save"; });
    }, 900);
  });
}

// ------------------------------------------------- filter/search
var FILTERS = [["all","All"],["pend","Needs cart"],["pass","Passed"],["fail","Failed"],["skip","Skipped"],["retired","Retired"]];
var curFilter = lsGet("filter") || "all";
function buildFilter(){
  var seg = document.getElementById("filter");
  FILTERS.forEach(function(f){
    var b = document.createElement("button"); b.type = "button"; b.textContent = f[1]; b.dataset.f = f[0];
    b.addEventListener("click", function(){ curFilter = f[0]; lsSet("filter", curFilter); applyFilter(); });
    seg.appendChild(b);
  });
  document.getElementById("q").addEventListener("input", applyFilter);
}
function applyFilter(){
  var q = document.getElementById("q").value.trim().toLowerCase();
  document.querySelectorAll("#filter button").forEach(function(b){ b.classList.toggle("on", b.dataset.f === curFilter); });
  ROWS.forEach(function(r){
    var el = document.getElementById("row-" + domId(r.id));
    if (!el) return;
    var st = effStatus(r) || "pend";
    var okF = curFilter === "all" || st === curFilter;
    var okQ = !q || (r.id + " " + r.what + " " + r.check).toLowerCase().indexOf(q) >= 0;
    el.hidden = !(okF && okQ);
  });
}

function markOffline(){ document.getElementById("offline").hidden = false;
  document.querySelectorAll(".sgn button, .notes textarea, .addph button").forEach(function(b){ b.disabled = true; }); }

// ------------------------------------------------- boot
buildFilter(); renderAll(); wireGuyNote();
(async function(){
  if (!(window.claude && window.claude.use)) { markOffline(); return; }
  db = await claude.use("db");
  if (!db) { markOffline(); return; }
  canWrite = true; // refused writes downgrade to read-only
  var u = await claude.use("user");
  if (u && u.can) { var w = await u.can("data.write"); if (w === false) { canWrite = false; markOffline(); } }
  document.querySelectorAll(".addph button").forEach(function(b){ b.hidden = false; });
  db.collection("signoffs").onSnapshot(function(s){
    s.docChanges().forEach(function(c){
      if (c.type === "removed") delete state.signoffs[c.doc.id];
      else state.signoffs[c.doc.id] = c.doc.data();
      applyRow(c.doc.id);
    });
    refreshCounts(); applyFilter();
  }, function(){});
  db.collection("claude").onSnapshot(function(s){
    s.docChanges().forEach(function(c){
      if (c.type === "removed") delete state.claude[c.doc.id];
      else state.claude[c.doc.id] = c.doc.data();
      applyRow(c.doc.id);
    });
  }, function(){});
  db.collection("findings").onSnapshot(function(s){
    s.docChanges().forEach(function(c){
      if (c.type === "removed") delete state.findings[c.doc.id];
      else state.findings[c.doc.id] = c.doc.data();
    });
    renderFindings();
  }, function(){});
  db.doc("meta/board").onSnapshot(function(snap){ state.meta = snap.exists ? snap.data() : null; renderClaude(); }, function(){});
  db.doc("notes/guy").onSnapshot(function(snap){
    var ta = document.getElementById("guy-note");
    if (snap.exists && document.activeElement !== ta) ta.value = snap.data().text || "";
  }, function(){});
  assetsNS = await claude.use("assets");
  if (!assetsNS) document.querySelectorAll(".addph button").forEach(function(b){ b.hidden = true; });
})();
</script>
"""

if __name__ == "__main__":
    main()
