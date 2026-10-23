#!/usr/bin/env python3
"""Sign in to Xiaomi Cloud and save this account's BLE-Mesh key privately.

The account password is entered with terminal echo disabled. Login credentials
and session fields stay in memory; the returned 32-byte gatt_ltmk is written
outside the repository with owner-only permissions.
"""

from __future__ import annotations

import argparse
import base64
import getpass
import hashlib
import hmac
import http.cookiejar
import json
import os
from pathlib import Path
import secrets
import stat
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit, urlunsplit
from urllib.request import HTTPCookieProcessor, HTTPRedirectHandler, Request, build_opener

API_URL = "https://api.io.mi.com/app/v2/blemesh/query_dev"
API_BASE = "https://api.io.mi.com/app"
DEVICE_LIST_PATH = "/home/device_list"
TARGET_MODEL = "lemesh.light.wy0d02"
DEFAULT_OUT = Path.home() / ".config" / "smartled-esp32" / "gatt_ltmk.hex"


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class _CaptureRedirectHeaders(HTTPRedirectHandler):
    """Retain Xiaomi's session extension header across automatic redirects."""
    def __init__(self):
        super().__init__()
        self.extension_pragma_headers: list[str] = []

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        value = headers.get("Extension-Pragma")
        if value:
            self.extension_pragma_headers.append(value)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def _login_json(payload: bytes) -> dict:
    """Decode Xiaomi's JSON login response without echoing its contents."""
    try:
        text = payload.decode("utf-8")
        if text.startswith("&&&START&&&"):
            text = text[len("&&&START&&&"):]
        value = json.loads(text)
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise RuntimeError("Xiaomi login returned an unreadable response") from None
    if not isinstance(value, dict):
        raise RuntimeError("Xiaomi login returned an unexpected response")
    return value


def _post_login_form(opener, url: str, form: dict[str, str], headers: dict[str, str],
                     timeout: float) -> dict:
    request = Request(url, data=urlencode(form).encode("utf-8"), headers={
        **headers, "Content-Type": "application/x-www-form-urlencoded",
    }, method="POST")
    with opener.open(request, timeout=timeout) as response:
        return _login_json(response.read(65536))


def _complete_identity_check(opener, notification_url: str,
                             headers: dict[str, str], timeout: float) -> str:
    """Ask Xiaomi to send a one-time code, then finish its identity check."""
    parts = urlsplit(notification_url)
    if parts.scheme != "https" or parts.hostname != "account.xiaomi.com":
        raise RuntimeError("Xiaomi identity-check URL was not on the expected account host")
    if "/fe/service/identity/authStart" not in parts.path:
        raise RuntimeError("Xiaomi returned an unsupported identity-check flow")
    list_url = urlunsplit((parts.scheme, parts.netloc,
                           parts.path.replace("/fe/service/identity/authStart", "/identity/list", 1),
                           parts.query, ""))
    with opener.open(Request(list_url, headers=headers), timeout=timeout) as response:
        identity = _login_json(response.read(65536))
    flag = identity.get("flag")
    method = {4: ("Phone", identity.get("maskedPhone")),
              8: ("Email", identity.get("maskedEmail"))}.get(flag)
    if not method:
        raise RuntimeError("Xiaomi identity check offered an unsupported verification method")
    method_name, destination = method

    verify_url = f"https://account.xiaomi.com/identity/auth/verify{method_name}?_flag={flag}&_json=true"
    with opener.open(Request(verify_url, headers=headers), timeout=timeout) as response:
        challenge = _login_json(response.read(65536))
    destination = destination or challenge.get("maskedPhone") or challenge.get("maskedEmail")
    if not destination:
        raise RuntimeError("Xiaomi did not provide a masked verification destination")
    print(f"小米将向 {destination} 发送一次性验证码。")
    input("确认后按 Enter 发送验证码（Ctrl-C 取消）: ")

    send_url = f"https://account.xiaomi.com/identity/auth/send{method_name}Ticket?_flag={flag}&_json=true"
    sent = _post_login_form(opener, send_url, {
        "_json": "true", "icode": "", "retry": "0",
    }, headers, timeout)
    if sent.get("captchaURL"):
        raise RuntimeError("Xiaomi requires a CAPTCHA before sending the verification code")
    if sent.get("code") not in (None, 0):
        raise RuntimeError("Xiaomi could not send the verification code")
    ticket = getpass.getpass("输入小米发来的验证码（隐藏输入）: ").strip()
    if not ticket:
        raise RuntimeError("No verification code was entered")

    verify_ticket_query = urlencode({
        "_flag": str(flag), "ticket": ticket, "trust": "false", "_json": "true",
    })
    verify_ticket_url = (f"https://account.xiaomi.com/identity/auth/verify{method_name}?"
                         f"{verify_ticket_query}")
    with opener.open(Request(verify_ticket_url, data=b"", headers=headers,
                             method="POST"), timeout=timeout) as response:
        verified = _login_json(response.read(65536))
    location = verified.get("location")
    if not isinstance(location, str) or not location.startswith("https://"):
        raise RuntimeError("Xiaomi rejected the verification code or returned no session redirect")
    return location


def login_xiaomi(username: str, password: str, timeout: float) -> tuple[str, str, str]:
    """Sign in locally and return userId, serviceToken, and ssecurity.

    Credentials and session values remain in memory. Xiaomi may require a
    captcha/QR flow; those are reported without printing response bodies or
    session data. Phone/email identity verification is handled interactively.
    """
    jar = http.cookiejar.CookieJar()
    redirect_capture = _CaptureRedirectHeaders()
    opener = build_opener(HTTPCookieProcessor(jar), redirect_capture)
    headers = {"User-Agent": "Smartled-esp32-local-tool/0.1"}
    init_url = "https://account.xiaomi.com/pass/serviceLogin?_json=true&sid=xiaomiio"
    try:
        with opener.open(Request(init_url, headers=headers), timeout=timeout) as response:
            init = _login_json(response.read(65536))
        required = ("sid", "qs", "_sign", "callback")
        if not all(isinstance(init.get(key), str) for key in required):
            raise RuntimeError("Xiaomi login setup response was incomplete")

        form = {
            "_json": "true",
            "user": username,
            "hash": hashlib.md5(password.encode("utf-8")).hexdigest().upper(),
            "sid": init["sid"], "qs": init["qs"],
            "_sign": init["_sign"], "callback": init["callback"],
        }
        post = Request(
            "https://account.xiaomi.com/pass/serviceLoginAuth2",
            data=urlencode(form).encode("utf-8"), headers={
                **headers, "Content-Type": "application/x-www-form-urlencoded",
            }, method="POST",
        )
        with opener.open(post, timeout=timeout) as response:
            auth = _login_json(response.read(65536))
        if auth.get("captchaURL"):
            raise RuntimeError("Xiaomi requires a CAPTCHA; login stopped before cloud access")
        location = auth.get("location")
        if auth.get("notificationUrl"):
            location = _complete_identity_check(
                opener, auth["notificationUrl"], headers, timeout
            )
        elif auth.get("code") in (70016, 70022):
            raise RuntimeError("Xiaomi requires an unsupported identity-check flow")
        if not isinstance(location, str) or not location.startswith("https://"):
            raise RuntimeError("Xiaomi rejected login or returned no session redirect")

        # The redirect endpoint sets userId/serviceToken cookies. Depending on
        # Xiaomi's login variant, ssecurity is in the auth JSON or a response header.
        with opener.open(Request(location, headers=headers), timeout=timeout) as response:
            redirect_header = response.headers.get("Extension-Pragma", "")
            ssecurity = auth.get("ssecurity")
            pragma_headers = [redirect_header, *redirect_capture.extension_pragma_headers]
            for pragma in pragma_headers:
                if ssecurity or not pragma:
                    continue
                try:
                    ssecurity = json.loads(pragma).get("ssecurity")
                except (json.JSONDecodeError, AttributeError):
                    continue
            response.read(65536)
        values = {cookie.name: cookie.value for cookie in jar}
        user_id = str(auth.get("userId") or values.get("userId") or "")
        service_token = values.get("serviceToken", "")
        if not all((user_id, service_token, ssecurity)):
            missing = [name for name, value in (
                ("userId", user_id), ("serviceToken", service_token),
                ("ssecurity", ssecurity),
            ) if not value]
            raise RuntimeError(
                "Xiaomi login session was incomplete (missing: " + ", ".join(missing) + ")"
            )
        # Validate the key's documented representation before making cloud calls.
        try:
            base64.b64decode(ssecurity, validate=True)
        except (ValueError, TypeError):
            raise RuntimeError("Xiaomi returned an invalid session signing value") from None
        return user_id, service_token, ssecurity
    except HTTPError as exc:
        raise RuntimeError(f"Xiaomi login returned HTTP {exc.code}") from None
    except URLError as exc:
        raise RuntimeError(f"Xiaomi login request failed ({type(exc.reason).__name__})") from None


def parse_response(payload: bytes) -> bytes:
    """Return exactly 32 key bytes, without exposing malformed response data."""
    try:
        response = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise ValueError("key API returned invalid JSON") from None
    if not isinstance(response, dict):
        raise ValueError("key API returned an unexpected response")
    if response.get("code") != 0:
        code = response.get("code")
        safe_code = str(code) if isinstance(code, (int, str)) else "unknown"
        raise RuntimeError(f"Xiaomi key API rejected the request (code {safe_code})")
    try:
        value = response["result"]["gatt_ltmk"]
        key = bytes.fromhex(value)
    except (KeyError, TypeError, ValueError, AttributeError):
        raise ValueError("key API response did not contain a valid gatt_ltmk field") from None
    if len(key) != 32:
        raise ValueError("gatt_ltmk has an unexpected length")
    return key


def signed_form(api_path: str, data: str, ssecurity: str,
                nonce_bytes: bytes, minute: int) -> dict[str, str]:
    """Create the standard MiCloud signed form fields for one API request."""
    if len(nonce_bytes) != 8:
        raise ValueError("nonce source must be 8 bytes")
    nonce = base64.b64encode(nonce_bytes + minute.to_bytes(4, "big")).decode("ascii")
    signed_nonce = base64.b64encode(hashlib.sha256(
        base64.b64decode(ssecurity, validate=True) +
        base64.b64decode(nonce, validate=True)
    ).digest()).decode("ascii")
    message = "&".join((api_path, signed_nonce, nonce, "data=" + data)).encode("utf-8")
    signature = base64.b64encode(hmac.new(
        base64.b64decode(signed_nonce), message, hashlib.sha256
    ).digest()).decode("ascii")
    return {"signature": signature, "_nonce": nonce, "data": data}


def matching_devices(payload: bytes) -> list[dict[str, str]]:
    try:
        response = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise ValueError("invalid JSON") from None
    if not isinstance(response, dict):
        raise ValueError("unexpected response")
    if response.get("code") != 0:
        code = response.get("code")
        safe_code = str(code) if isinstance(code, (int, str)) else "unknown"
        raise RuntimeError(f"Xiaomi device-list API rejected the request (code {safe_code})")
    try:
        devices = response["result"]["list"]
        return [
            {"did": str(device["did"]), "name": str(device.get("name", TARGET_MODEL))}
            for device in devices
            if device.get("model") == TARGET_MODEL and device.get("did")
        ]
    except (AttributeError, KeyError, TypeError, ValueError):
        raise ValueError("missing or malformed device list") from None


def write_private(path: Path, key: bytes) -> None:
    path = path.expanduser().resolve()
    repo = Path(__file__).resolve().parents[1]
    try:
        path.relative_to(repo)
    except ValueError:
        pass
    else:
        raise ValueError("refusing to write a BLE key inside the project repository")
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(path.parent, 0o700)
    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    fd = os.open(path, flags, 0o600)
    try:
        with os.fdopen(fd, "w", encoding="ascii") as stream:
            stream.write(key.hex() + "\n")
    except Exception:
        try:
            path.unlink()
        except OSError:
            pass
        raise
    os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)


def post_signed(api_path: str, data: str, user_id: str, service_token: str,
                ssecurity: str, timeout: float) -> bytes:
    form = signed_form(api_path, data, ssecurity, secrets.token_bytes(8), int(time.time() / 60))
    body = urlencode(form).encode("ascii")
    request = Request(
        API_BASE + api_path,
        data=body,
        headers={
            "Content-Type": "application/x-www-form-urlencoded",
            "Cookie": f"userId={user_id}; serviceToken={service_token}",
            "User-Agent": "Smartled-esp32-local-tool/0.1",
            "x-xiaomi-protocal-flag-cli": "PROTOCAL-HTTP2",
        },
        method="POST",
    )
    try:
        opener = build_opener(_NoRedirect())
        with opener.open(request, timeout=timeout) as response:
            return response.read(65536)
    except HTTPError as exc:
        raise RuntimeError(f"Xiaomi API returned HTTP {exc.code}") from None
    except URLError as exc:
        raise RuntimeError(f"Xiaomi API request failed ({type(exc.reason).__name__})") from None


def find_device_did(user_id: str, service_token: str, ssecurity: str,
                    timeout: float, requested_did: str | None) -> str:
    if requested_did:
        return requested_did
    data = json.dumps({"getVirtualModel": False, "getHuamiDevices": 0},
                      separators=(",", ":"))
    try:
        payload = post_signed(DEVICE_LIST_PATH, data, user_id, service_token,
                              ssecurity, timeout)
        candidates = matching_devices(payload)
    except ValueError:
        raise RuntimeError("Xiaomi device-list response was invalid") from None
    if not candidates:
        raise RuntimeError(f"no {TARGET_MODEL} device found in this Mi Home account")
    if len(candidates) == 1:
        return candidates[0]["did"]
    print(f"Found {len(candidates)} devices matching {TARGET_MODEL}:")
    for index, device in enumerate(candidates, 1):
        print(f"  {index}. {device['name']}")
    try:
        choice = int(input("Choose the target device number: "))
    except (ValueError, EOFError):
        raise RuntimeError("device selection was not a valid number") from None
    if not 1 <= choice <= len(candidates):
        raise RuntimeError("device selection is out of range")
    return candidates[choice - 1]["did"]


def fetch(did: str, user_id: str, service_token: str,
          ssecurity: str, timeout: float) -> bytes:
    api_path = "/v2/blemesh/query_dev"
    data = json.dumps({"did": did}, separators=(",", ":"), ensure_ascii=True)
    payload = post_signed(api_path, data, user_id, service_token, ssecurity, timeout)
    return parse_response(payload)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--did", help="optional DID; otherwise discover this model in your account")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help=f"private output file (default: {DEFAULT_OUT})")
    parser.add_argument("--timeout", type=float, default=20.0)
    args = parser.parse_args()
    if args.timeout < 1 or args.timeout > 120:
        parser.error("timeout must be 1..120 seconds")

    username = input("Mi Home account (phone/email/Xiaomi ID): ").strip()
    password = getpass.getpass("Mi Home password (input hidden; not saved): ")
    if not username or not password:
        print("Mi Home account and password are required.", file=sys.stderr)
        return 2
    try:
        user_id, service_token, ssecurity = login_xiaomi(username, password, args.timeout)
        username = password = ""
        print("Xiaomi login succeeded.")
        if not args.did:
            print("Querying your Mi Home device list for the target model...")
        did = find_device_did(user_id, service_token, ssecurity, args.timeout, args.did)
        print("Target found; requesting its BLE key...")
        key = fetch(did, user_id, service_token, ssecurity, args.timeout)
        write_private(args.out, key)
    except (RuntimeError, ValueError, OSError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    finally:
        user_id = ""
        service_token = ""
        ssecurity = ""
        did = ""
        username = ""
        password = ""
    print(f"gatt_ltmk saved with owner-only permissions: {args.out.expanduser()}")
    print("The key value was not printed. Keep the file outside this repository.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
