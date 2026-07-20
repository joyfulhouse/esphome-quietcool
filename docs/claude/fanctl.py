"""Drive the QuietCool fan over the ESPHome native API for controlled testing.

Usage: fanctl.py <off|low|medium|high|refresh> [...]
Each argument is executed in order with a pause between, so a whole test
sequence is one invocation and the timing is deterministic.
"""

import asyncio
import re
import sys
from pathlib import Path

from aioesphomeapi import APIClient

HOST = "10.100.8.46"
SECRETS = Path("/Users/bryanli/Projects/joyfulhouse/homeassistant-dev/quietcool/secrets.yaml")

SPEEDS = {"off": None, "low": 1, "medium": 2, "high": 3}


def api_key() -> str:
    text = SECRETS.read_text()
    m = re.search(r"^quietcool_lora32_api_key:\s*[\"']?([A-Za-z0-9+/=]+)", text, re.M)
    if not m:
        raise SystemExit("could not read quietcool_lora32_api_key from secrets.yaml")
    return m.group(1)


async def main() -> None:
    actions = sys.argv[1:]
    if not actions:
        raise SystemExit(__doc__)

    cli = APIClient(HOST, 6053, None, noise_psk=api_key())
    await cli.connect(login=True)
    entities, _ = await cli.list_entities_services()

    fan = next((e for e in entities if type(e).__name__ == "FanInfo"), None)
    if fan is None:
        raise SystemExit("fan entity not found")

    buttons = {getattr(e, "name", ""): e for e in entities if type(e).__name__ == "ButtonInfo"}

    for action in actions:
        a = action.lower()
        if a == "refresh":
            btn = next((b for n, b in buttons.items() if "Refresh Fan State" in n), None)
            if btn is None:
                raise SystemExit(f"refresh button not found; have {list(buttons)}")
            cli.button_command(btn.key)
            print(f"[fanctl] pressed Refresh Fan State", flush=True)
        elif a in SPEEDS:
            speed = SPEEDS[a]
            if speed is None:
                cli.fan_command(key=fan.key, state=False)
            else:
                cli.fan_command(key=fan.key, state=True, speed_level=speed)
            print(f"[fanctl] commanded {a.upper()}", flush=True)
        elif a.startswith("wait"):
            secs = float(a[4:] or 10)
            await asyncio.sleep(secs)
            continue
        else:
            raise SystemExit(f"unknown action {action!r}")
        await asyncio.sleep(0.2)

    await asyncio.sleep(1.0)
    await cli.disconnect()


asyncio.run(main())
