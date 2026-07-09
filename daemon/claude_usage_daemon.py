#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Clawdmeter" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import getpass
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 8.0
CONNECT_TIMEOUT = 20.0
WAKE_GAP_THRESHOLD = 60.0   # inner wait is <=TICK; a bigger wall-clock jump = Mac slept
ACCOUNTS_POLL_INTERVAL = 300            # multi-account screen: 5 min (7d barely moves)

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "config"

API_URL = "https://api.anthropic.com/api/oauth/usage"
API_HEADERS_TEMPLATE = {
    "anthropic-beta": "oauth-2025-04-20",
    "User-Agent": "claude-code/2.1.5",
}


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _read_credentials_raw() -> str | None:
    """Raw credentials blob — the macOS Keychain item or the Linux creds file."""
    if sys.platform == "darwin":
        try:
            out = subprocess.run(
                [
                    "security",
                    "find-generic-password",
                    "-s",
                    KEYCHAIN_SERVICE,
                    "-a",
                    getpass.getuser(),
                    "-w",
                ],
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )
        except subprocess.CalledProcessError as e:
            log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
            return None
        except (FileNotFoundError, subprocess.TimeoutExpired) as e:
            log(f"Keychain access error: {e}")
            return None
        return out.stdout
    try:
        return CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None


def read_token() -> str | None:
    """The stored OAuth access token (no refresh). Prefer get_access_token()."""
    raw = _read_credentials_raw()
    return _extract_access_token(raw) if raw else None


def _read_oauth() -> dict | None:
    """The full claudeAiOauth blob (accessToken/refreshToken/expiresAt)."""
    raw = _read_credentials_raw()
    if not raw:
        return None
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        return None
    if isinstance(data, dict):
        oauth = data.get("claudeAiOauth")
        if isinstance(oauth, dict):
            return oauth
        if isinstance(data.get("accessToken"), str):
            return data
    return None


# --- OAuth token refresh (in-memory only) -----------------------------------
#
# Normally the daemon just reads the live Keychain access token, which Claude
# Code keeps fresh while it runs. But if the Mac sleeps past the token's ~8h TTL
# with no Claude Code process to refresh it, that token lapses and every usage
# poll 401s until Claude Code next runs. To stay useful across overnight sleep,
# the daemon refreshes the token itself from the Keychain's refresh token — but
# keeps the result IN MEMORY ONLY. It never writes back to the Keychain (Claude
# Code owns that item; rotating refresh tokens could otherwise contend). The
# token endpoint is itself rate-limited, so refreshes are cooldown-limited.
OAUTH_TOKEN_URL = "https://platform.claude.com/v1/oauth/token"
OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
TOKEN_EXPIRY_BUFFER_MS = 5 * 60 * 1000   # treat as expired 5 min early
REFRESH_COOLDOWN = 120.0                 # min seconds between token-endpoint hits
_token_cache: dict = {"access": None, "expires_at_ms": 0, "last_attempt": 0.0}


async def _refresh_access_token(refresh_token: str) -> tuple[str, int] | None:
    """POST the token endpoint; return (access_token, expires_at_ms) or None."""
    body = {
        "grant_type": "refresh_token",
        "refresh_token": refresh_token,
        "client_id": OAUTH_CLIENT_ID,
    }
    try:
        async with httpx.AsyncClient(timeout=15.0) as http:
            resp = await http.post(
                OAUTH_TOKEN_URL, json=body,
                headers={"User-Agent": API_HEADERS_TEMPLATE["User-Agent"]},
            )
    except httpx.HTTPError as e:
        log(f"Token refresh failed: {e}")
        return None
    if resp.status_code >= 400:
        log(f"Token refresh HTTP {resp.status_code}: {resp.text[:160]}")
        return None
    try:
        d = resp.json()
        access = d["access_token"]
        expires_at_ms = int(time.time() * 1000) + int(d["expires_in"]) * 1000
    except (ValueError, KeyError, TypeError) as e:
        log(f"Token refresh: unexpected response ({e})")
        return None
    return access, expires_at_ms


async def get_access_token() -> str | None:
    """A usable access token for the usage poll.

    Prefers the live Keychain token (fresh whenever Claude Code is running). If
    that has lapsed — typically after the Mac slept past the ~8h TTL — fall back
    to an in-memory token, refreshing via the Keychain refresh token at most once
    per REFRESH_COOLDOWN. Never writes back to the Keychain.
    """
    now = time.time()
    now_ms = int(now * 1000)
    oauth = _read_oauth() or {}
    kc_access = oauth.get("accessToken")
    kc_exp = oauth.get("expiresAt") or 0

    # 1. A still-valid Keychain token always wins (Claude Code owns refreshing it).
    if kc_access and kc_exp > now_ms + TOKEN_EXPIRY_BUFFER_MS:
        return kc_access
    # 2. A still-valid in-memory refreshed token.
    if (_token_cache["access"]
            and _token_cache["expires_at_ms"] > now_ms + TOKEN_EXPIRY_BUFFER_MS):
        return _token_cache["access"]
    # 3. Refresh (cooldown-limited so a bad window can't hammer the endpoint).
    refresh_token = oauth.get("refreshToken")
    if refresh_token and now - _token_cache["last_attempt"] >= REFRESH_COOLDOWN:
        _token_cache["last_attempt"] = now
        refreshed = await _refresh_access_token(refresh_token)
        if refreshed:
            _token_cache["access"], _token_cache["expires_at_ms"] = refreshed
            log("Refreshed OAuth token in-memory (Keychain copy had lapsed)")
            return _token_cache["access"]
    # 4. Best effort: whatever the Keychain has (may 401, but no worse than before).
    return kc_access


# --- Account identity (which account are we actually polling?) ---------------
#
# The OAuth token is opaque, so it can't tell us whose account it is. Claude
# Code records the logged-in account in ~/.claude.json (written alongside the
# Keychain/credentials token on login and on account switch). We surface that
# email so the logs say WHICH account each reading came from, and warn loudly
# if it changes mid-run — otherwise a silent account switch just looks like the
# usage numbers inexplicably jumped.
CLAUDE_CODE_CONFIG = Path.home() / ".claude.json"
_account_cache: dict = {"mtime": None, "email": None}
_last_logged_email: str | None = None


def read_account_email() -> str | None:
    """Email of the account the current token belongs to, or None if unknown.

    Cached on the config file's mtime so we don't re-parse a possibly multi-MB
    JSON file every poll — it only changes on login / account switch.
    """
    try:
        mtime = CLAUDE_CODE_CONFIG.stat().st_mtime
    except OSError:
        return None
    if _account_cache["mtime"] == mtime:
        return _account_cache["email"]
    email = None
    try:
        data = json.loads(CLAUDE_CODE_CONFIG.read_text())
        acct = data.get("oauthAccount")
        if isinstance(acct, dict) and isinstance(acct.get("emailAddress"), str):
            email = acct["emailAddress"]
    except (OSError, json.JSONDecodeError):
        email = None
    _account_cache["mtime"] = mtime
    _account_cache["email"] = email
    return email


def log_account_if_changed() -> None:
    """Log the active account once, then again only when it changes."""
    global _last_logged_email
    email = read_account_email()
    if email == _last_logged_email:
        return
    if _last_logged_email is None:
        log(f"Account: {email or 'unknown (is Claude Code logged in?)'}")
    else:
        log(f"Account CHANGED: {_last_logged_email} -> {email or 'unknown'}")
    _last_logged_email = email


# --- Multi-account pacing (claude-limits) -----------------------------------
#
# claude-limits (github.com/coalesce-labs/claude-limits) is a standalone
# fleet-wide usage-limit authority: one container per Claude account identity
# (each with its own independent login), polling Anthropic directly and
# publishing OTel metrics to the catalyst-otel stack. We read those metrics
# back from Prometheus over Tailscale — the SAME source for every account,
# including whichever one is active on this Mac, so there's no local-active-
# account special case and no hardcoded alias map (claude-limits emits the
# display alias itself as the claude_limits_account_alias label). Drives the
# device's "Accounts" pacing screen — refreshed slowly (7d barely moves).
PROMETHEUS_URL = os.environ.get("CLAUDE_LIMITS_PROMETHEUS_URL", "http://home:9098")

# Prometheus deliberately carries no email label (PII stays off metrics per the
# ratified telemetry contract), so matching "which account is active on THIS
# Mac" against a fleet-wide account_uuid needs a small local correlation map.
# This is NOT a display alias map (claude-limits already provides that) — just
# enough to answer "is this me right now."
ACCOUNT_UUID_BY_EMAIL = {
    "ryan@rozich.com": "49f9f37d-dea6-4248-b656-b989add677ee",
    "ryan.rozich@gmail.com": "3a3a9a22-d249-4420-94f8-57c7f6378d25",
    "ryan@getadva.ai": "11543267-6b8f-4899-abcc-7442230dd324",
}


async def _prom_query(promql: str) -> list[dict]:
    """Instant PromQL query -> Prometheus's `data.result` list."""
    async with httpx.AsyncClient(timeout=8.0) as http:
        resp = await http.get(f"{PROMETHEUS_URL}/api/v1/query", params={"query": promql})
    resp.raise_for_status()
    return resp.json().get("data", {}).get("result", [])


async def poll_accounts() -> list[dict]:
    """Build the accounts-screen payload: {e,u,wr,a,st?} per account, sourced
    entirely from claude-limits via Prometheus — one query result covers every
    configured identity at once, active or not.
    """
    active_email = read_account_email()   # the live, logged-in account on this Mac
    active_uuid = ACCOUNT_UUID_BY_EMAIL.get(active_email or "")
    now = time.time()

    try:
        auth_rows = await _prom_query("claude_limits_account_auth_active")
        util_rows = await _prom_query(
            'claude_limits_account_utilization_ratio{claude_limits_window="seven_day"}')
        reset_rows = await _prom_query(
            'claude_limits_account_reset_time_seconds{claude_limits_window="seven_day"}')
    except (httpx.HTTPError, ValueError) as e:
        log(f"claude-limits/Prometheus query failed: {e}")
        return []

    util_by_uuid = {r["metric"].get("user_account_uuid"): float(r["value"][1]) for r in util_rows}
    reset_by_uuid = {r["metric"].get("user_account_uuid"): float(r["value"][1]) for r in reset_rows}

    # auth_active is emitted every export cycle for every configured identity —
    # the canonical "which accounts exist" list; utilization/reset are
    # overlaid onto it (may be briefly absent for a just-logged-in account).
    out: list[dict] = []
    for row in auth_rows:
        m = row["metric"]
        uuid = m.get("user_account_uuid")
        alias = m.get("claude_limits_account_alias") or uuid or "?"
        auth_ok = row["value"][1] == "1"
        is_active = bool(active_uuid and uuid == active_uuid)

        if not auth_ok:
            out.append({"e": alias, "a": 1 if is_active else 0, "st": 1})   # invalid token
            continue

        util = util_by_uuid.get(uuid)
        if util is None:
            # The utilization series itself is missing — a genuinely broken/
            # not-yet-emitted state, not a data quirk.
            out.append({"e": alias, "a": 1 if is_active else 0, "st": 3})   # unavailable
            continue

        entry = {"e": alias, "u": int(round(util * 100)), "a": 1 if is_active else 0}
        reset_epoch = reset_by_uuid.get(uuid)
        if reset_epoch is not None:
            entry["wr"] = max(0, int((reset_epoch - now) / 60))
        # else: a completely fresh/unused window (0% utilization) genuinely has
        # no resets_at yet — confirmed against the live API, not a bug. Omit
        # "wr" rather than fabricate a countdown; the firmware defaults an
        # absent reset to no pace marker, which is the honest rendering.
        out.append(entry)
    return out


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


async def scan_for_device() -> str | None:
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address
    return None


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Clawdmeter', or None.

    Two-step lookup, strongest signal first:

    1. Peripherals connected under our CUSTOM service UUID. Membership in
       that service is unambiguous (no other device exposes it), so we accept
       by service alone — the peripheral's name can be None on macOS.
    2. Fall back to the generic HID service 0x1812, but ONLY trust a
       peripheral whose name matches DEVICE_NAME. 0x1812 also matches
       unrelated keyboards/mice, so picking blindly here could grab the
       wrong device.

    ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
    stale CoreBluetooth handle can't trap us into never trying a fresh scan.
    """
    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:  # BleakBluetoothNotAvailableError etc.
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic HID service — require an exact name match.
    hid = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_("1812")]
    )
    for p in hid or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    macOS: prefer the system-connected peripheral (HID-grabbed devices are
    invisible to scans); fall back to a normal scan that yields a BLEDevice
    so the subsequent connect doesn't have to re-scan. ``skip_addr`` is
    forwarded so a just-failed peripheral is skipped, making the scan
    fallback reachable.

    Other platforms: keep the original cached-address / scan-by-name flow.
    A freshly scanned address is cached here (the only place it's saved).
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is not None:
            return dev
        log(f"Not held by OS; scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
        dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
        if dev:
            log(f"Found: {dev.address}")
        return dev

    address = load_cached_address()
    if not address:
        address = await scan_for_device()
        if address:
            save_address(address)  # cache only freshly-scanned addresses
    return address


def read_clock_setting() -> str:
    """Read the `clock` option from the config file. One of: off|auto|12|24.

    Defaults to "off" (no clock; the device keeps showing "Usage") so existing
    setups are unaffected until the user opts in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "clock":
                    val = val.strip().lower()
                    if val in ("off", "auto", "12", "24"):
                        return val
    except OSError:
        pass
    return "off"


def detect_hour_format() -> int:
    """Best-effort 12h/24h detection for the host. Returns 12 or 24 (default 24)."""
    # macOS: the explicit System Settings toggle lives in NSGlobalDomain.
    for key, result in (("AppleICUForce24HourTime", 24), ("AppleICUForce12HourTime", 12)):
        try:
            out = subprocess.run(["defaults", "read", "-g", key],
                                 capture_output=True, text=True, timeout=3)
            if out.stdout.strip() == "1":
                return result
        except (OSError, subprocess.SubprocessError):
            pass
    # Fallback to the C locale's time format (may be C/24h under launchd).
    try:
        import locale
        locale.setlocale(locale.LC_TIME, "")
        fmt = locale.nl_langinfo(locale.T_FMT)
        if "%p" in fmt or "%r" in fmt or "%I" in fmt:
            return 12
    except (ImportError, locale.Error, AttributeError):
        pass
    return 24


def add_clock_fields(payload: dict) -> None:
    """Add wall-clock fields to the payload when the config opts in.

    "t"  = local wall-clock epoch (UTC epoch shifted by the tz offset) so the
           device can show the time without an RTC.
    "tf" = 12 or 24, the hour format the device should render.
    """
    clock = read_clock_setting()
    if clock == "off":
        return
    tf = 24 if clock == "24" else 12 if clock == "12" else detect_hour_format()
    payload["t"] = int(time.time()) + time.localtime().tm_gmtoff
    payload["tf"] = tf


async def poll_api(token: str) -> dict | None:
    """Read-only usage poll. Was POST /v1/messages (max_tokens:1) reading
    rate-limit HEADERS off a real inference call — a genuine ~1/min API hit
    with a session-activity footprint on the active account, live 24/7.
    GET /api/oauth/usage (the same read-only endpoint claude-meter and
    claude-limits use) returns the same 5h/7d numbers directly in the JSON
    body, with zero consumption and no inference session created."""
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.get(API_URL, headers=headers)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return None

    try:
        data = resp.json()
    except ValueError as e:
        log(f"API response unreadable: {e}")
        return None

    now = time.time()

    def reset_minutes(resets_at: str | None) -> int:
        if not resets_at:
            return 0
        try:
            dt = datetime.fromisoformat(resets_at.replace("Z", "+00:00"))
        except ValueError:
            return 0
        mins = (dt.timestamp() - now) / 60.0
        return int(round(mins)) if mins > 0 else 0

    def pct(block: dict) -> int:
        util = block.get("utilization")
        return int(round(util)) if util is not None else 0

    five_hour = data.get("five_hour") or {}
    seven_day = data.get("seven_day") or {}
    session_pct = pct(five_hour)
    payload = {
        "s": session_pct,
        "sr": reset_minutes(five_hour.get("resets_at")),
        "w": pct(seven_day),
        "wr": reset_minutes(seven_day.get("resets_at")),
        # The old "allowed"/"limited" string came from a rate-limit response
        # header this endpoint doesn't have; the firmware parses this field
        # but doesn't currently render it (data.h status[16], unused in
        # ui.cpp), so a simple derived value keeps it meaningful without
        # fabricating detail the API no longer gives us.
        "st": "limited" if session_pct >= 100 else "allowed",
        "ok": True,
        "acct": read_account_email() or "",   # which account this reading is for
    }
    add_clock_fields(payload)   # adds "t" + "tf" iff the config opts in
    return payload


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        # start_notify awaits CoreBluetooth's CCCD-write confirmation, which
        # never arrives if the peripheral doesn't ACK the subscribe (a
        # half-open link after the OS auto-connects the HID). Unbounded, that
        # await wedges the whole daemon between "Connected" and the first poll
        # — the device then shows nothing until a manual restart. Bound it: the
        # subscription is only an optional device-initiated refresh nudge (we
        # poll every POLL_INTERVAL regardless), so on timeout we proceed.
        try:
            await asyncio.wait_for(
                self.client.start_notify(REQ_CHAR_UUID, self._on_refresh),
                timeout=10,
            )
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")
        except asyncio.TimeoutError:
            log("Refresh subscription timed out; polling without it")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


def _is_encryption_error(exc: BaseException) -> bool:
    """True if a connect error is a macOS bonding/encryption mismatch.

    macOS reports a stale bond as CBErrorDomain Code=15 ("Failed to encrypt
    the connection..."). Match on the message text so we don't depend on how
    bleak wraps the underlying CoreBluetooth error.
    """
    s = str(exc).lower()
    return "code=15" in s or "encrypt" in s


# blueutil talks to Bluetooth via IOBluetooth, which on recent macOS needs its
# OWN Bluetooth TCC grant (separate from the daemon's CoreBluetooth grant).
# Without it, blueutil *hangs* instead of erroring — so every call is bounded
# by a timeout and a hang is reported as a permission problem, not a crash.
BLUEUTIL_TIMEOUT = 8


def _blueutil(*args: str) -> str | None:
    """Run `blueutil <args>`, returning stdout, or None on failure/timeout.

    A timeout almost always means blueutil lacks Bluetooth permission (it
    blocks rather than failing), so we surface that cause explicitly.
    """
    try:
        return subprocess.run(
            ["blueutil", *args],
            capture_output=True, text=True,
            timeout=BLUEUTIL_TIMEOUT, check=True,
        ).stdout
    except subprocess.TimeoutExpired:
        log(f"blueutil {' '.join(args)} timed out — it likely lacks Bluetooth "
            "permission. Grant it under System Settings > Privacy & Security > "
            "Bluetooth (run `blueutil --paired` once from Terminal to prompt).")
        return None
    except (subprocess.SubprocessError, OSError) as e:
        log(f"blueutil {' '.join(args)} failed: {e}")
        return None


def unpair_macos() -> bool:
    """Forget a stale macOS bond for DEVICE_NAME so the device can re-pair.

    A Code=15 "failed to encrypt" connect error means macOS holds bonding
    keys that no longer match the ESP32's (e.g. after a firmware reflash or
    the on-device bond-clear gesture). The firmware pairs "just works" (no
    MITM), so once the stale bond is gone the next connect re-bonds silently
    with no GUI prompt.

    CoreBluetooth exposes no unpair API, so we shell out to `blueutil`. The
    daemon only knows the peripheral's CoreBluetooth UUID, not the BD_ADDR
    that blueutil needs, so we map by name via `blueutil --paired`. Returns
    True if a bond was removed. Mirrors the Linux daemon's `bluetoothctl
    remove` self-heal.
    """
    if not shutil.which("blueutil"):
        log("Stale bond detected but `blueutil` is not installed; cannot "
            "auto-recover. Run `brew install blueutil`, or forget "
            f"'{DEVICE_NAME}' in System Settings > Bluetooth and reconnect.")
        return False

    out = _blueutil("--paired")
    if out is None:
        return False

    # Each line looks like:
    #   address: 28-84-85-55-5c-3d, ... name: "Clawdmeter", ...
    addr = None
    for line in out.splitlines():
        if f'name: "{DEVICE_NAME}"' in line:
            m = re.search(r"address:\s*([0-9a-fA-F:-]+)", line)
            if m:
                addr = m.group(1)
                break
    if not addr:
        log(f"No paired '{DEVICE_NAME}' found to unpair (already forgotten?)")
        return False

    if _blueutil("--unpair", addr) is None:
        return False
    log(f"Unpaired stale bond for '{DEVICE_NAME}' [{addr}]; re-pairing on "
        "next connect")
    return True


async def connect_and_run(target, stop_event: asyncio.Event) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        # Bound the connect the same way #84 bounded the refresh subscribe.
        # On macOS the OS auto-connects the firmware's HID link, so
        # CoreBluetooth can hand us a half-open peripheral whose GATT connect
        # handshake never completes. BleakClient's own timeout governs
        # discovery, not connectPeripheral, so an unbounded await here wedges
        # the single-threaded daemon forever at "Connecting..." (observed ~13h,
        # device stuck on stale data). wait_for raises TimeoutError, which the
        # handler below already treats as a connection failure -> drop the
        # cached address and rescan.
        await asyncio.wait_for(client.connect(), timeout=CONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        if sys.platform == "darwin" and _is_encryption_error(e):
            log("Encryption failed — likely a stale macOS bond; self-healing")
            unpair_macos()
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    last_poll = 0.0
    last_accounts_poll = 0.0
    used_successfully = False
    woke_from_sleep = False
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                log_account_if_changed()
                token = await get_access_token()
                if not token:
                    log("No token; skipping poll")
                else:
                    payload = await poll_api(token)
                    if payload is not None:
                        if await session.write_payload(payload):
                            last_poll = time.time()
                            used_successfully = True

            # Multi-account pacing screen — refreshed slowly and sent as its
            # own {"accts":[...]} message (the firmware routes on that key).
            if now - last_accounts_poll >= ACCOUNTS_POLL_INTERVAL:
                last_accounts_poll = now
                accts = await poll_accounts()
                if accts:
                    await session.write_payload({"accts": accts})

            wait_start = time.time()
            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
            # Sleep/wake guard: that wait returns within TICK in normal operation.
            # A much larger wall-clock jump means the Mac slept and just woke, so
            # the BLE link is now half-open — the device re-advertises ("Listening")
            # while CoreBluetooth still reports it connected and our writes vanish.
            # Bail out so the caller drops the stale handle and rescans fresh.
            gap = time.time() - wait_start
            if gap > WAKE_GAP_THRESHOLD:
                log(f"Woke from sleep ({int(gap)}s gap); forcing a fresh BLE reconnect")
                woke_from_sleep = True
                break
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    if woke_from_sleep:
        # Return False so the macOS caller skips this stale handle on the next
        # discover_target and falls through to a fresh scan of the re-advertising
        # device (a True return would retrieveConnected the same ghost).
        return False
    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")
    log_account_if_changed()

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    while not stop_event.is_set():
        # Apply any pending skip exactly once, then clear it so the next
        # cycle re-tries retrieveConnected (the device may have recovered).
        target = await discover_target(skip_addr=skip_addr)
        skip_addr = None
        if not target:
            log(f"Device not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
            continue

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(target, stop_event)
        if not ok:
            if sys.platform == "darwin":
                # No string cache to drop; instead skip this stale handle on
                # the next retrieveConnected so the scan fallback is reachable.
                skip_addr = addr
            else:
                log("Invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
