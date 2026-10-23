import base64
import json
import unittest
from types import SimpleNamespace
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit

from tools import fetch_gatt_ltmk as fetcher


class _Response:
    def __init__(self, payload=b"{}", headers=None):
        self.payload = payload
        self.headers = headers or {}

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def read(self, _limit=-1):
        return self.payload


class XiaomiLoginTests(unittest.TestCase):
    def test_login_json_strips_xiaomi_prefix(self):
        result = fetcher._login_json(b'&&&START&&&{"code":0}')
        self.assertEqual(result, {"code": 0})

    def test_redirect_handler_retains_extension_pragma(self):
        handler = fetcher._CaptureRedirectHeaders()
        result = handler.redirect_request(
            fetcher.Request("https://account.xiaomi.com/start"), None, 302,
            "Found", {"Extension-Pragma": '{"ssecurity":"opaque"}'},
            "https://sts.example/session",
        )
        self.assertEqual(result.full_url, "https://sts.example/session")
        self.assertEqual(handler.extension_pragma_headers, ['{"ssecurity":"opaque"}'])

    def test_login_returns_session_fields_without_printing_them(self):
        ssecurity = base64.b64encode(b"session-secret").decode("ascii")
        setup = {"sid": "xiaomiio", "qs": "q", "_sign": "sign", "callback": "cb"}
        auth = {"ssecurity": ssecurity, "userId": "12345", "location": "https://sts.example/login"}
        jar = [SimpleNamespace(name="userId", value="12345"),
               SimpleNamespace(name="serviceToken", value="private-token")]

        class FakeOpener:
            calls = 0

            def open(self, request, timeout):
                self.calls += 1
                if self.calls == 1:
                    return _Response(b"&&&START&&&" + json.dumps(setup).encode())
                if self.calls == 2:
                    return _Response(b"&&&START&&&" + json.dumps(auth).encode())
                return _Response()

        opener = FakeOpener()
        with patch.object(fetcher.http.cookiejar, "CookieJar", return_value=jar), \
             patch.object(fetcher, "build_opener", return_value=opener):
            result = fetcher.login_xiaomi("user@example.test", "password", 5)
        self.assertEqual(result, ("12345", "private-token", ssecurity))
        self.assertEqual(opener.calls, 3)

    def test_login_stops_if_captcha_is_required(self):
        setup = {"sid": "xiaomiio", "qs": "q", "_sign": "sign", "callback": "cb"}

        class FakeOpener:
            calls = 0

            def open(self, _request, timeout):
                self.calls += 1
                if self.calls == 1:
                    return _Response(b"&&&START&&&" + json.dumps(setup).encode())
                return _Response(b'&&&START&&&{"captchaURL":"/captcha"}')

        opener = FakeOpener()
        with patch.object(fetcher.http.cookiejar, "CookieJar", return_value=[]), \
             patch.object(fetcher, "build_opener", return_value=opener):
            with self.assertRaisesRegex(RuntimeError, "CAPTCHA"):
                fetcher.login_xiaomi("user", "password", 5)
        self.assertEqual(opener.calls, 2)

    def test_phone_identity_check_requests_code_and_finishes_login(self):
        class FakeOpener:
            def __init__(self):
                self.urls = []

            def open(self, request, timeout):
                self.urls.append(request.get_full_url())
                if request.get_full_url().endswith("/identity/list?auth=1"):
                    return _Response(b'{"flag":4}')
                if "/identity/auth/verifyPhone?_flag=4&_json=true" in request.get_full_url():
                    return _Response(b'{"maskedPhone":"+86****1234"}')
                if "/identity/auth/sendPhoneTicket?" in request.get_full_url():
                    return _Response(b'{"code":0}')
                if "/identity/auth/verifyPhone?_flag=4&ticket=" in request.get_full_url():
                    return _Response(b'{"location":"https://sts.example/session"}')
                raise AssertionError(f"unexpected Xiaomi login request: {request.full_url}")

        opener = FakeOpener()
        with patch("builtins.input", return_value=""), \
             patch.object(fetcher.getpass, "getpass", return_value="246810"):
            location = fetcher._complete_identity_check(
                opener, "https://account.xiaomi.com/fe/service/identity/authStart?auth=1",
                {"User-Agent": "test"}, 5,
            )
        self.assertEqual(location, "https://sts.example/session")
        verify_url = next(url for url in opener.urls if "ticket=" in url)
        self.assertEqual(parse_qs(urlsplit(verify_url).query)["ticket"], ["246810"])

    def test_device_list_parser_does_not_expect_gatt_key(self):
        payload = json.dumps({
            "code": 0,
            "result": {"list": [{"did": "private-did", "model": fetcher.TARGET_MODEL,
                                  "name": "Test light"}]},
        }).encode()
        self.assertEqual(fetcher.matching_devices(payload), [
            {"did": "private-did", "name": "Test light"},
        ])

    def test_key_parser_reports_safe_cloud_error_code(self):
        with self.assertRaisesRegex(RuntimeError, r"code -3"):
            fetcher.parse_response(b'{"code":-3,"message":"private details"}')


if __name__ == "__main__":
    unittest.main()
