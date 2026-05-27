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


def compute_mood(usage: dict) -> str:
    """Phase 1 mood: time of day, nudged by how hard the limits are being hit.

    High utilisation = the user is grinding → chaotic/excited; otherwise fall
    back to the time-of-day baseline. Phase 2 will fold in real coding events
    from the hook spool.
    """
    base = time_of_day_mood()
    session = float(usage.get("s", 0) or 0)
    if usage.get("st", "").lower() in ("limited", "exceeded"):
        return "chaotic"
    if session >= 90:
        return "chaotic"
    if session >= 70:
        return "excited"
    return base


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


def update_and_block(usage: dict, token: str | None) -> dict:
    """Compute the compact buddy block to ship over BLE, persisting state.

    Compact keys match firmware parse_json:
      sp species idx, ra rarity idx, lv level, mo mood idx, ht hat idx,
      nm name, st [5 stats in STAT_NAMES order].
    """
    config = load_config()
    state = load_state()

    bones = resolve_identity(config, state, token)

    # XP / level: Phase 1 grows slowly with usage pressure (Phase 2: hook events).
    xp = int(state.get("xp", 0)) + max(1, round(float(usage.get("s", 0) or 0) / 5))
    state["xp"] = xp
    level = 1 + xp // 100

    mood = compute_mood(usage)
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
    # Persist the human-readable snapshot alongside the cached account id.
    state["buddy"] = {**bones, "level": level, "mood": mood, "name": name}
    save_state(state)
    return block


if __name__ == "__main__":
    # Quick manual check: print the buddy for a seed or the cached account.
    import sys
    seed = sys.argv[1] if len(sys.argv) > 1 else None
    if seed:
        os.environ["BUDDY_SEED"] = seed
    blk = update_and_block({"s": 42, "st": "allowed"}, None)
    print(json.dumps(blk, ensure_ascii=False))
    print("species:", SPECIES[blk["sp"]], "| rarity:", RARITIES[blk["ra"]],
          "| mood:", MOODS[blk["mo"]], "| hat:", HATS[blk["ht"]])
