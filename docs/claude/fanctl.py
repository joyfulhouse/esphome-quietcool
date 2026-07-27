"""Drive the QuietCool fan over the ESPHome native API for controlled testing.

Usage: fanctl.py <off|low|medium|high|refresh> [...]
Each argument is executed in order with a pause between, so a whole test
sequence is one invocation and the timing is deterministic.
"""

import asyncio
import os
import re
import sys
from pathlib import Path

from aioesphomeapi import APIClient

HOST = os.environ.get("QUIETCOOL_HOST", "10.100.8.46")

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SECRETS_CANDIDATES = (
    Path(os.environ["QUIETCOOL_SECRETS"]).expanduser(),
) if "QUIETCOOL_SECRETS" in os.environ else (
    _REPO_ROOT / "secrets.yaml",
    _REPO_ROOT / "legacy" / "secrets.yaml",
)
SECRETS = next(
    (p for p in _SECRETS_CANDIDATES if p.is_file()),
    _SECRETS_CANDIDATES[0],
)

_EXAMPLE = _REPO_ROOT / "secrets.yaml.example"
_KEY_NAME = "quietcool_lora32_api_key"
_ANY_API_KEY = re.compile(r"^\w*api_key:\s*[\"']?([A-Za-z0-9+/=]+)", re.M)

SPEEDS = {"off": None, "low": 1, "medium": 2, "high": 3}


def _placeholder_keys() -> set[str]:
    """The fake API keys shipped in secrets.yaml.example.

    Read from the example rather than hardcoded, so the check cannot drift
    away from what a fresh checkout actually contains.
    """
    try:
        return set(_ANY_API_KEY.findall(_EXAMPLE.read_text()))
    except OSError:
        return set()


def api_key() -> str:
    if not SECRETS.is_file():
        raise SystemExit(
            f"no secrets file found at {SECRETS}; set QUIETCOOL_SECRETS to the "
            f"secrets.yaml holding this device's real {_KEY_NAME}"
        )
    m = re.search(rf"^{_KEY_NAME}:\s*[\"']?([A-Za-z0-9+/=]+)", SECRETS.read_text(), re.M)
    if not m:
        raise SystemExit(f"could not read {_KEY_NAME} from {SECRETS}")
    key = m.group(1)
    if key in _placeholder_keys():
        # `cp secrets.yaml.example secrets.yaml` is what CI and CONTRIBUTING.md
        # tell you to do, so the repository's own secrets.yaml normally holds
        # placeholders. Connecting with one fails deep inside the noise
        # handshake; say so here instead.
        raise SystemExit(
            f"{SECRETS} still holds the {_KEY_NAME} placeholder from "
            f"secrets.yaml.example — that is not a real credential. Set "
            f"QUIETCOOL_SECRETS to the secrets.yaml for the device at {HOST}."
        )
    return key


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
