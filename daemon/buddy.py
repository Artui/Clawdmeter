#!/usr/bin/env python3
"""Claude Buddy — deterministic companion generation for the Clawdmeter device.

The daemon is the brain: it derives a stable species/rarity/stats buddy from the
account identity (or an override seed), tracks mood + XP, and hands the device a
compact block to render. Ported from the MIT upstream
https://github.com/1270011/claude-buddy (server/engine.ts, mood.ts) — the
wyhash → mulberry32 → bones pipeline is reproduced so generation is
deterministic and identical across runs.

Identity is resolved in priority order (see resolve_identity):
  1. explicit field overrides in buddy.toml  → used verbatim
  2. `seed` in buddy.toml or $BUDDY_SEED      → hashed to a reproducible buddy
  3. account id (cached, derived from creds)  → the default

Config:  ~/.config/claude-usage-monitor/buddy.toml   (human-edited input)
State:   ~/.config/claude-usage-monitor/buddy.json   (generated values + XP/mood)
"""
from __future__ import annotations

import base64
import json
import os
import random
import time
from pathlib import Path

try:
    import tomllib  # Python 3.11+
except ModuleNotFoundError:  # pragma: no cover
    tomllib = None

# ─── Upstream tables (engine.ts) ────────────────────────────────────────────
SALT = "friend-2026-401"
SPECIES = [
    "duck", "goose", "blob", "cat", "dragon", "octopus", "owl", "penguin",
    "turtle", "snail", "ghost", "axolotl", "capybara", "cactus", "robot",
    "rabbit", "mushroom", "chonk", "wyvern", "pikachu",
]
RARITIES = ["common", "uncommon", "rare", "epic", "legendary"]
RARITY_WEIGHTS = {"common": 60, "uncommon": 25, "rare": 10, "epic": 4, "legendary": 1}
RARITY_FLOOR = {"common": 5, "uncommon": 15, "rare": 25, "epic": 35, "legendary": 50}
STAT_NAMES = ["DEBUGGING", "PATIENCE", "CHAOS", "WISDOM", "SNARK"]
EYES = ["·", "✦", "×", "◉", "@", "°"]
HATS = ["none", "crown", "tophat", "propeller", "halo", "wizard", "beanie", "tinyduck"]
# Device mood order — must match firmware data.h / buddy_art.h.
MOODS = ["happy", "focused", "excited", "tired", "melancholy", "chaotic"]

CONFIG_DIR = Path.home() / ".config" / "claude-usage-monitor"
CONFIG_PATH = CONFIG_DIR / "buddy.toml"
STATE_PATH = CONFIG_DIR / "buddy.json"
# Hook event spool (written by buddy_hook.py, drained here each poll).
SPOOL_PATH = CONFIG_DIR / "buddy-events.jsonl"

# ─── wyhash (Zig stdlib v4.2) port ──────────────────────────────────────────
MASK64 = (1 << 64) - 1
_WY = [0xa0761d6478bd642f, 0xe7037ed1a0b428db, 0x8ebc6af09c88c6e3, 0x589965cc75374cc3]


def _wymum(a: int, b: int) -> tuple[int, int]:
    x = (a & MASK64) * (b & MASK64)
    return x & MASK64, (x >> 64) & MASK64


def _wymix(a: int, b: int) -> int:
    lo, hi = _wymum(a, b)
    return (lo ^ hi) & MASK64


def _wyr8(buf: bytes, off: int) -> int:
    return int.from_bytes(buf[off:off + 8], "little")


def _wyr4(buf: bytes, off: int) -> int:
    return int.from_bytes(buf[off:off + 4], "little")


def wyhash(text: str, seed: int = 0) -> int:
    buf = text.encode("utf-8")
    n = len(buf)
    s0 = (seed ^ _wymix((seed ^ _WY[0]) & MASK64, _WY[1])) & MASK64
    s1 = s2 = s0
    if n <= 16:
        if n >= 4:
            q = (n >> 3) << 2
            a = ((_wyr4(buf, 0) << 32) | _wyr4(buf, q)) & MASK64
            b = ((_wyr4(buf, n - 4) << 32) | _wyr4(buf, n - 4 - q)) & MASK64
        elif n > 0:
            a = (buf[0] << 16) | (buf[n >> 1] << 8) | buf[n - 1]
            b = 0
        else:
            a = b = 0
    else:
        i = 0
        if n >= 48:
            while i + 48 < n:
                states = [s0, s1, s2]
                for j in range(3):
                    ra = _wyr8(buf, i + 16 * j)
                    rb = _wyr8(buf, i + 16 * j + 8)
                    states[j] = _wymix((ra ^ _WY[j + 1]) & MASK64,
                                       (rb ^ states[j]) & MASK64)
                s0, s1, s2 = states
                i += 48
            s0 = (s0 ^ s1 ^ s2) & MASK64
        rem = buf[i:]
        ri = 0
        while ri + 16 < len(rem):
            s0 = _wymix((_wyr8(rem, ri) ^ _WY[1]) & MASK64,
                        (_wyr8(rem, ri + 8) ^ s0) & MASK64)
            ri += 16
        a = _wyr8(buf, n - 16)
        b = _wyr8(buf, n - 8)
    a = (a ^ _WY[1]) & MASK64
    b = (b ^ s0) & MASK64
    a, b = _wymum(a, b)
    return _wymix((a ^ _WY[0] ^ n) & MASK64, (b ^ _WY[1]) & MASK64)


def hash_string(s: str) -> int:
    return wyhash(s) & 0xFFFFFFFF


# ─── mulberry32 PRNG ────────────────────────────────────────────────────────
def _imul(x: int, y: int) -> int:
    return ((x & 0xFFFFFFFF) * (y & 0xFFFFFFFF)) & 0xFFFFFFFF


def mulberry32(seed: int):
    a = seed & 0xFFFFFFFF

    def rng() -> float:
        nonlocal a
        a = (a + 0x6D2B79F5) & 0xFFFFFFFF
        t = _imul(a ^ (a >> 15), 1 | a)
        t = (((t + _imul(t ^ (t >> 7), 61 | t)) & 0xFFFFFFFF) ^ t) & 0xFFFFFFFF
        return ((t ^ (t >> 14)) & 0xFFFFFFFF) / 4294967296

    return rng


# ─── Generation ─────────────────────────────────────────────────────────────
def _pick(rng, arr):
    return arr[int(rng() * len(arr))]


def _roll_rarity(rng) -> str:
    total = sum(RARITY_WEIGHTS.values())
    roll = rng() * total
    for r in RARITIES:
        roll -= RARITY_WEIGHTS[r]
        if roll < 0:
            return r
    return "common"


def generate_bones(user_id: str, salt: str = SALT) -> dict:
    """Deterministic species/rarity/stats from an identity string."""
    rng = mulberry32(hash_string(user_id + salt))
    rarity = _roll_rarity(rng)
    species = _pick(rng, SPECIES)
    eye = _pick(rng, EYES)
    hat = "none" if rarity == "common" else _pick(rng, HATS)
    shiny = rng() < 0.01
    peak = _pick(rng, STAT_NAMES)
    dump = _pick(rng, STAT_NAMES)
    while dump == peak:
        dump = _pick(rng, STAT_NAMES)
    floor = RARITY_FLOOR[rarity]
    stats = {}
    for name in STAT_NAMES:
        if name == peak:
            stats[name] = min(100, floor + 50 + int(rng() * 30))
        elif name == dump:
            stats[name] = max(1, floor - 10 + int(rng() * 15))
        else:
            stats[name] = floor + int(rng() * 40)
    return {"rarity": rarity, "species": species, "eye": eye, "hat": hat,
            "shiny": shiny, "stats": stats, "peak": peak, "dump": dump}


# ─── Mood (mood.ts: Phase 1 = time-of-day + usage pressure) ──────────────────
def time_of_day_mood(hour: int | None = None) -> str:
    h = time.localtime().tm_hour if hour is None else hour
    if h >= 22 or h < 6:
        return "tired"
    if h < 9:
        return "melancholy"
    if h < 12:
        return "focused"
    if h < 17:
        return "happy"
    if h < 20:
        return "excited"
    return "tired"


def compute_mood(usage: dict, activity: dict | None = None) -> str:
    """Mood: real coding events first, then usage pressure, then time of day.

    When the hook spool has drained recent activity (Phase 2) it dominates —
    errors make the buddy chaotic, a run of edits makes it focused, a busy
    terminal/prompt cadence makes it excited. With no events we fall back to
    the usage-pressure + time-of-day baseline.
    """
    if activity:
        if activity.get("errors", 0) > 0:
            return "chaotic"
        if activity.get("edits", 0) >= 8:
            return "focused"
        if activity.get("bash", 0) >= 6 or activity.get("prompts", 0) >= 3:
            return "excited"

    base = time_of_day_mood()
    session = float(usage.get("s", 0) or 0)
    if usage.get("st", "").lower() in ("limited", "exceeded"):
        return "chaotic"
    if session >= 90:
        return "chaotic"
    if session >= 70:
        return "excited"
    return base


# ─── Event spool (hook activity → buddy reactions) ───────────────────────────
def drain_events() -> list[dict]:
    """Atomically consume the hook spool, returning the events since last drain.

    We rename-then-read so events appended by a concurrent hook (which always
    opens the spool path fresh) land in a new file rather than being lost.
    Best-effort: any failure yields no events and never raises.
    """
    tmp = SPOOL_PATH.with_suffix(".jsonl.draining")
    try:
        SPOOL_PATH.rename(tmp)
    except OSError:
        return []  # nothing to drain (or a parallel drain already took it)
    try:
        text = tmp.read_text(errors="ignore")
    except OSError:
        text = ""
    finally:
        try:
            tmp.unlink()
        except OSError:
            pass

    events: list[dict] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            events.append(json.loads(line))
        except ValueError:
            continue
    return events


def summarize_events(events: list[dict]) -> dict:
    """Fold raw hook events into a coarse activity summary for mood/remarks/XP."""
    ev_counts: dict[str, int] = {}
    tool_counts: dict[str, int] = {}
    for e in events:
        ev_counts[e.get("e", "")] = ev_counts.get(e.get("e", ""), 0) + 1
        t = e.get("t", "")
        if t:
            tool_counts[t] = tool_counts.get(t, 0) + 1
    return {
        "edits": (tool_counts.get("Edit", 0) + tool_counts.get("Write", 0)
                  + tool_counts.get("MultiEdit", 0) + tool_counts.get("NotebookEdit", 0)),
        "bash": tool_counts.get("Bash", 0),
        "reads": tool_counts.get("Read", 0) + tool_counts.get("Grep", 0)
        + tool_counts.get("Glob", 0),
        "errors": ev_counts.get("PostToolUseFailure", 0),
        "prompts": ev_counts.get("UserPromptSubmit", 0),
        "session_start": ev_counts.get("SessionStart", 0),
        "session_end": ev_counts.get("SessionEnd", 0),
        "stops": ev_counts.get("Stop", 0),
        "total": len(events),
    }


# ─── Remarks (the buddy "talks") ─────────────────────────────────────────────
# The device renders a transient speech bubble from the optional `rm` field on
# the buddy block (and pops the buddy screen to the front when `rf` is set). The
# daemon decides *what* and *when*. Phase 1 (here) is curated lines keyed by mood
# + usage; Phase 2 folds in real coding events from the hook spool, and Phase 3
# can swap a curated line for an LLM-generated one at a configurable rate.
#
# Lines are plain ASCII — the device fonts cover letters/digits/basic
# punctuation only, so anything fancier renders blank. Keep them short (the
# bubble wraps but ~80 chars is the comfortable ceiling) and in the buddy's
# voice: a little snarky, a little fond.
REMARKS = {
    "happy":      ["all green over here", "we're cooking", "good vibes, good code",
                   "this is the fun part"],
    "focused":    ["heads down. let's go", "in the zone, don't blink",
                   "one function at a time", "locked in"],
    "excited":    ["oh this is GOOD", "now we're moving", "ship it ship it",
                   "love this for us"],
    "tired":      ["it's late, you good?", "coffee. now.", "running on fumes here",
                   "maybe wrap up soon?"],
    "melancholy": ["slow morning, huh", "we'll get there", "deep breath. begin.",
                   "mondays, am i right"],
    "chaotic":    ["everything is fine (it's not)", "WHO TOUCHED THE CONFIG",
                   "we ball", "no notes, only chaos"],
}
# Usage-band lines take priority over mood when the pressure is notable.
USAGE_REMARKS = {
    "limit":  ["easy tiger, we hit the limit", "rate limit. take five.",
               "that's the cap for now"],
    "high":   ["90% burned, pace yourself", "we're really sending it today",
               "tokens going fast"],
    "medium": ["solid session so far", "good chunk of the window used"],
}
# Reactions to real coding activity from the hook spool (Phase 2). These win
# over ambient mood/usage lines, and the "notable" ones bypass the chattiness
# gate so the buddy actually reacts when something happens.
EVENT_REMARKS = {
    "error":         ["red again? deep breath", "okay, who broke it", "that's a yikes",
                      "bug spotted. classic.", "we'll squash it"],
    "edit_streak":   ["you're on FIRE", "look at you go", "rapid-fire edits, nice",
                      "shipping like crazy"],
    "bash_heavy":    ["smashing that terminal", "command-line cowboy today",
                      "lots of shell happening"],
    "session_start": ["back at it, let's go", "fresh session, fresh bugs",
                      "morning. or whenever. hi."],
    "session_end":   ["gg, nice work", "see you next time", "rest up, dev"],
}


def sanitize_remark(text: str, limit: int = 80) -> str:
    """Strip to printable ASCII the device fonts can render, clamp length."""
    cleaned = "".join(c for c in text if 0x20 <= ord(c) < 0x7F)
    cleaned = " ".join(cleaned.split())  # collapse whitespace/newlines
    return cleaned[:limit].rstrip()


# Back-compat internal alias.
_sanitize_ascii = sanitize_remark


def _select_pool(usage: dict, mood: str, activity: dict | None,
                 rng: random.Random) -> tuple[list[str], bool]:
    """Choose the candidate line pool and whether it's "notable".

    Notable pools react to a real coding event and bypass the chattiness gate
    (but still respect the cooldown). Returns ``(pool, notable)``.
    """
    if activity and activity.get("total", 0):
        if activity.get("errors", 0) > 0:
            return EVENT_REMARKS["error"], True
        if activity.get("session_end", 0) > 0:
            return EVENT_REMARKS["session_end"], True
        if activity.get("session_start", 0) > 0:
            return EVENT_REMARKS["session_start"], True
        if activity.get("edits", 0) >= 8:
            return EVENT_REMARKS["edit_streak"], True
        if activity.get("bash", 0) >= 6:
            return EVENT_REMARKS["bash_heavy"], False

    # Ambient fallback: usage-band specials, else mood flavour.
    session = float(usage.get("s", 0) or 0)
    status = str(usage.get("st", "")).lower()
    if status in ("limited", "exceeded"):
        return USAGE_REMARKS["limit"], False
    if session >= 90:
        return USAGE_REMARKS["high"], False
    if session >= 70 and rng.random() < 0.5:
        return USAGE_REMARKS["medium"], False
    return REMARKS.get(mood, REMARKS["happy"]), False


def compute_remark(usage: dict, mood: str, config: dict, state: dict,
                   activity: dict | None = None) -> tuple[str, bool]:
    """Decide what (if anything) the buddy says this poll.

    Returns ``(text, force_foreground)``; an empty string means stay silent so
    the caller omits the `rm` field entirely. A cooldown bounds how often the
    buddy speaks; ambient (mood/usage) lines additionally pass a chattiness
    probability, while reactions to real coding events bypass it so they feel
    responsive. Never repeats the immediately-previous line.

    `activity` is the drained hook-spool summary (Phase 2). `llm_rate` (Phase 3)
    is read from config but inert until that path is wired.
    """
    rc = config.get("remarks", {}) if isinstance(config.get("remarks"), dict) else {}
    if not rc.get("enabled", True):
        return "", False

    cooldown = float(rc.get("cooldown_secs", 120))
    chattiness = float(rc.get("chattiness", 0.6))
    foreground = bool(rc.get("foreground", False))

    now = time.time()
    if now - float(state.get("remark_last_ts", 0)) < cooldown:
        return "", False

    rng = random.Random()
    pool, notable = _select_pool(usage, mood, activity, rng)
    if not notable and random.random() > chattiness:
        return "", False

    text = ""
    for _ in range(4):  # avoid repeating the previous line
        candidate = _sanitize_ascii(rng.choice(pool))
        if candidate and candidate != state.get("remark_last_text"):
            text = candidate
            break
    if not text:
        return "", False

    state["remark_last_ts"] = now
    state["remark_last_text"] = text
    return text, foreground


# ─── LLM remarks (Phase 3, optional, gated by llm_rate) ──────────────────────
# The daemon already authenticates to /v1/messages with the Claude Code OAuth
# token, so we can occasionally generate a fresh quip instead of a curated line.
# OAuth credentials require the identity sentence as its own first system
# block, so the daemon sends LLM_IDENTITY then LLM_PERSONA. LLM_PERSONA is
# static, so it's sent cache_control:ephemeral for a prompt-cache hit; the
# situation (mood/activity) goes in the per-call user message.
LLM_IDENTITY = "You are Claude Code, Anthropic's official CLI for Claude."
LLM_PERSONA = (
    "For this task you voice \"Claude Buddy\", a tiny pixel companion that lives "
    "on a desk gadget watching a developer's Claude Code session. Given the "
    "buddy's mood and what just happened, reply with ONE very short remark the "
    "buddy says out loud: at most ~70 characters, lowercase and casual, playful "
    "and a little snarky but genuinely fond. Plain ASCII only — no emoji, no "
    "quotation marks, no markdown, no preamble. Output only the remark."
)


def _maybe_llm_req(usage: dict, mood: str, activity: dict | None, name: str,
                   species: str, config: dict, fallback: str) -> dict | None:
    """If the llm_rate gate fires, return a request the daemon can fulfil.

    Returns ``{"model", "user", "fallback"}`` or None to keep the curated line.
    The curated `fallback` is what ships if the generation call fails.
    """
    rc = config.get("remarks", {}) if isinstance(config.get("remarks"), dict) else {}
    rate = float(rc.get("llm_rate", 0.0) or 0.0)
    if rate <= 0 or random.random() > rate:
        return None

    bits = [f"buddy: {name} the {species}", f"mood: {mood}"]
    if activity:
        acts = []
        if activity.get("edits"):
            acts.append(f"{activity['edits']} file edits")
        if activity.get("bash"):
            acts.append(f"{activity['bash']} shell commands")
        if activity.get("errors"):
            acts.append(f"{activity['errors']} tool errors")
        if activity.get("prompts"):
            acts.append(f"{activity['prompts']} new prompts")
        if activity.get("session_start"):
            acts.append("just started a session")
        if activity.get("session_end"):
            acts.append("the session just ended")
        bits.append("recent activity: " + (", ".join(acts) if acts else "quiet"))
    else:
        bits.append("no coding activity right now")
    bits.append(f"5h usage window at {int(float(usage.get('s', 0) or 0))}%")

    return {
        "model": rc.get("model") or "claude-haiku-4-5",
        "user": "; ".join(bits) + ". one short remark:",
        "fallback": fallback,
    }


# ─── Config + state ──────────────────────────────────────────────────────────
def load_config() -> dict:
    if tomllib is None or not CONFIG_PATH.exists():
        return {}
    try:
        with CONFIG_PATH.open("rb") as f:
            return tomllib.load(f)
    except (OSError, ValueError):
        return {}


def load_state() -> dict:
    try:
        return json.loads(STATE_PATH.read_text())
    except (OSError, ValueError):
        return {}


def save_state(state: dict) -> None:
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    tmp = STATE_PATH.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(state, indent=2))
    tmp.replace(STATE_PATH)


def _decode_jwt_subject(token: str) -> str | None:
    """Best-effort: pull a stable subject claim out of a JWT access token."""
    parts = token.split(".")
    if len(parts) < 2:
        return None
    try:
        pad = "=" * (-len(parts[1]) % 4)
        payload = json.loads(base64.urlsafe_b64decode(parts[1] + pad))
    except (ValueError, json.JSONDecodeError):
        return None
    for key in ("sub", "account_id", "user_id", "email"):
        if isinstance(payload.get(key), str):
            return payload[key]
    return None


def resolve_account_id(token: str | None, state: dict) -> str:
    """A stable identity for the account, cached so token refreshes don't reroll.

    Access tokens rotate, but the subject claim inside them does not. Once we've
    resolved a stable id we cache it in state and reuse it forever.
    """
    if state.get("account_id"):
        return state["account_id"]
    acct = _decode_jwt_subject(token) if token else None
    if not acct:
        # Last resort: machine-stable fallback so the buddy is at least
        # consistent on this host. Override with a `seed` for portability.
        acct = f"{os.uname().nodename}:{os.getenv('USER', 'user')}"
    state["account_id"] = acct
    return acct


# ─── Public: resolve identity → buddy block ──────────────────────────────────
def resolve_identity(config: dict, state: dict, token: str | None) -> dict:
    """Return bones (species/rarity/stats/hat) after applying config precedence."""
    overrides = config.get("overrides", {})
    seed = config.get("seed") or os.getenv("BUDDY_SEED")
    identity = seed if seed else resolve_account_id(token, state)
    bones = generate_bones(str(identity))

    # Field-level overrides win over generation.
    if overrides.get("species") in SPECIES:
        bones["species"] = overrides["species"]
    if overrides.get("rarity") in RARITIES:
        bones["rarity"] = overrides["rarity"]
    if overrides.get("hat") in HATS:
        bones["hat"] = overrides["hat"]
    if isinstance(overrides.get("stats"), dict):
        smap = {"debug": "DEBUGGING", "patience": "PATIENCE", "chaos": "CHAOS",
                "wisdom": "WISDOM", "snark": "SNARK"}
        for k, v in overrides["stats"].items():
            name = smap.get(k.lower())
            if name and isinstance(v, (int, float)):
                bones["stats"][name] = max(0, min(100, int(v)))
    return bones


def update_and_block(usage: dict, token: str | None) -> tuple[dict, dict | None]:
    """Compute the compact buddy block to ship over BLE, persisting state.

    Returns ``(block, llm_req)``. Compact block keys match firmware parse_json:
      sp species idx, ra rarity idx, lv level, mo mood idx, ht hat idx,
      nm name, st [5 stats], rm remark (optional), rf force-foreground (optional).
    `llm_req` is None unless the llm_rate gate fired, in which case the daemon
    may replace `block["rm"]` with a generated line (curated stays as fallback).
    """
    config = load_config()
    state = load_state()

    bones = resolve_identity(config, state, token)

    # Drain real coding activity from the hook spool (empty when no hooks/events).
    activity = summarize_events(drain_events())
    if not activity.get("total"):
        activity = None

    # XP / level. A small floor keeps it ticking on usage alone; real coding
    # events (edits/bash/prompts) earn the bulk of the XP.
    event_xp = 0
    if activity:
        event_xp = (activity["edits"] * 2 + activity["bash"]
                    + activity["reads"] // 2 + activity["prompts"] * 3)
    xp = int(state.get("xp", 0)) + max(1, round(float(usage.get("s", 0) or 0) / 5)) + event_xp
    state["xp"] = xp
    level = 1 + xp // 100

    mood = compute_mood(usage, activity)
    name = config.get("overrides", {}).get("name") or bones["species"].capitalize()

    block = {
        "sp": SPECIES.index(bones["species"]),
        "ra": RARITIES.index(bones["rarity"]),
        "lv": min(level, 255),
        "mo": MOODS.index(mood),
        "ht": HATS.index(bones["hat"]),
        "nm": name[:15],
        "st": [bones["stats"][n] for n in STAT_NAMES],
    }

    # Optional speech bubble. Only attach `rm` when the buddy actually has
    # something new to say — a present `rm` is what makes the device pop the
    # bubble, so omitting it keeps the buddy quiet between remarks.
    # Rarity-color tint targets (firmware applies the fixed palette). Bitmask:
    # 1=stars, 2=name, 4=creature. Default: stars + creature.
    ap = config.get("appearance", {}) if isinstance(config.get("appearance"), dict) else {}
    tint = ((1 if ap.get("rarity_color_stars", True) else 0)
            | (2 if ap.get("rarity_color_name", False) else 0)
            | (4 if ap.get("rarity_color_creature", True) else 0))
    block["tc"] = tint

    # Bubble lifetime in seconds (device clamps via its own default if 0). Sent
    # every poll so the device — including the local LEFT+RIGHT phrase combo —
    # uses the configured duration.
    rc_cfg = config.get("remarks", {}) if isinstance(config.get("remarks"), dict) else {}
    block["bd"] = max(1, min(60, int(rc_cfg.get("bubble_secs", 7))))

    remark, force = compute_remark(usage, mood, config, state, activity)
    llm_req = None
    if remark:
        block["rm"] = remark
        if force:
            block["rf"] = True
        llm_req = _maybe_llm_req(usage, mood, activity, name,
                                 bones["species"], config, remark)

    # Persist the human-readable snapshot alongside the cached account id.
    state["buddy"] = {**bones, "level": level, "mood": mood, "name": name}
    save_state(state)
    return block, llm_req


if __name__ == "__main__":
    # Quick manual check: print the buddy for a seed or the cached account.
    import sys
    seed = sys.argv[1] if len(sys.argv) > 1 else None
    if seed:
        os.environ["BUDDY_SEED"] = seed
    blk, _llm = update_and_block({"s": 42, "st": "allowed"}, None)
    print(json.dumps(blk, ensure_ascii=False))
    print("species:", SPECIES[blk["sp"]], "| rarity:", RARITIES[blk["ra"]],
          "| mood:", MOODS[blk["mo"]], "| hat:", HATS[blk["ht"]],
          "| remark:", blk.get("rm", "(silent)"))
