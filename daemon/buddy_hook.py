#!/usr/bin/env python3
"""Claude Code hook → Claude Buddy event spool.

Registered on a handful of Claude Code hook events (see README / buddy.example
.toml). On each event Claude Code pipes a JSON object on stdin; we extract the
few fields the buddy cares about and append one compact line to a spool file
that the usage daemon drains each poll (daemon/buddy.py::drain_events).

Design rules:
  * Fire-and-forget. This runs inside the user's coding session, so it must
    never block Claude and must never exit non-zero in a way that matters —
    any failure is swallowed and we exit 0.
  * Zero dependencies (stdlib only) and no import of buddy.py, so it works no
    matter which checkout/worktree the daemon happens to be running from.
  * Tiny lines. tool_input can be huge (a whole file in a Write); we keep only
    the event name, tool name, and a coarse source/reason tag.
"""
import json
import sys
import time
from pathlib import Path

SPOOL = Path.home() / ".config" / "claude-usage-monitor" / "buddy-events.jsonl"
MAX_BYTES = 256 * 1024   # keep the spool bounded if the daemon isn't draining
KEEP_LINES = 500         # trimmed-to tail when the cap is hit


def main() -> None:
    try:
        data = json.load(sys.stdin)
    except Exception:
        data = {}

    evt = {
        "e": data.get("hook_event_name", ""),
        "t": data.get("tool_name", ""),
        "ts": int(time.time()),
    }
    # A coarse tag: SessionStart.source / SessionEnd.reason / Notification.type.
    tag = (data.get("source") or data.get("reason")
           or data.get("notification_type"))
    if isinstance(tag, str) and tag:
        evt["s"] = tag

    try:
        SPOOL.parent.mkdir(parents=True, exist_ok=True)
        # Best-effort size cap (racy under concurrent sessions, but the cost of
        # a lost line is purely cosmetic).
        try:
            if SPOOL.exists() and SPOOL.stat().st_size > MAX_BYTES:
                tail = SPOOL.read_text(errors="ignore").splitlines()[-KEEP_LINES:]
                SPOOL.write_text("\n".join(tail) + "\n")
        except OSError:
            pass
        line = json.dumps(evt, separators=(",", ":")) + "\n"
        with SPOOL.open("a") as f:
            f.write(line)
    except Exception:
        pass

    sys.exit(0)


if __name__ == "__main__":
    main()
