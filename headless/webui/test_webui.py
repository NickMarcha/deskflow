# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
"""Tests for POST /api/keystroke and the unix socket. Run: python3 -m unittest discover -s headless/webui"""

import http.client
import importlib.machinery
import importlib.util
import json
import socket
import stat
import tempfile
import threading
import unittest
from pathlib import Path

loader = importlib.machinery.SourceFileLoader("webui", str(Path(__file__).with_name("deskflow-headless-webui")))
spec = importlib.util.spec_from_loader("webui", loader)
webui = importlib.util.module_from_spec(spec)
loader.exec_module(webui)


class FakeCore(threading.Thread):
    """Stands in for deskflow-core's IPC socket and answers like CoreIpcServer."""

    def __init__(self, path: Path):
        super().__init__(daemon=True)
        self.lines: list[str] = []
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(str(path))
        self.sock.listen()

    def run(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            with conn:
                line = conn.makefile().readline().rstrip("\n")
                self.lines.append(line)
                conn.sendall(b"bye\n")  # broadcasts to the GUI may arrive first
                conn.sendall(b"error=unable to parse key\n" if "Bogus" in line else b"ok\n")


class UnixConnection(http.client.HTTPConnection):
    def __init__(self, path: Path):
        super().__init__("localhost", timeout=5)
        self.path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(str(self.path))


@unittest.skipUnless(hasattr(socket, "AF_UNIX"), "needs unix sockets")
class KeystrokeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        tmp = Path(self.tmp.name)
        self.core = FakeCore(tmp / "deskflow-core")
        self.core.start()
        app = webui.App(tmp / "server.ini", tmp, "deskflow-headless.service", tmp, tmp / "deskflow-core")
        self.socket_path = tmp / "status" / "webui.sock"
        self.server = webui.UnixHTTPServer(str(self.socket_path), webui.make_handler(app))
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.core.sock.close()
        self.tmp.cleanup()

    def post(self, body, header=True):
        conn = UnixConnection(self.socket_path)
        headers = {"Content-Type": "application/json"}
        if header:
            headers["X-Deskflow-Webui"] = "1"
        conn.request("POST", "/api/keystroke", json.dumps(body), headers)
        res = conn.getresponse()
        data = json.loads(res.read())
        conn.close()
        return res.status, data

    def test_named_computers(self):
        status, data = self.post({"keys": "Control+Shift+Alt+M", "computers": ["sage", "sage-dev", "sagedeck"]})
        self.assertEqual((status, data), (200, {"ok": True}))
        self.assertEqual(self.core.lines, ["keystroke=Control+Shift+Alt+M,sage:sage-dev:sagedeck"])

    def test_active_computer(self):
        status, _ = self.post({"keys": "Control+Alt+F8"})
        self.assertEqual(status, 200)
        self.assertEqual(self.core.lines, ["keystroke=Control+Alt+F8"])

    def test_server_error_is_passed_on(self):
        status, data = self.post({"keys": "Control+Bogus"})
        self.assertEqual((status, data), (400, {"ok": False, "error": "unable to parse key"}))

    def test_needs_header(self):
        status, data = self.post({"keys": "M"}, header=False)
        self.assertEqual(status, 403)
        self.assertFalse(data["ok"])
        self.assertEqual(self.core.lines, [])

    def test_rejects_bad_input(self):
        for body in ({}, {"keys": ""}, {"keys": "M,sage"}, {"keys": "M\nstop"}, {"keys": "M) = x("},
                     {"keys": "M", "computers": "sage"}, {"keys": "M", "computers": ["../x"]},
                     {"keys": "M", "computers": ["a:b"]}, {"keys": "M", "computers": ["sage\n"]}):
            status, data = self.post(body)
            self.assertEqual(status, 400, body)
            self.assertFalse(data["ok"])
        self.assertEqual(self.core.lines, [])

    def test_server_down(self):
        self.core.sock.close()
        Path(self.tmp.name, "deskflow-core").unlink()
        status, data = self.post({"keys": "M"})
        self.assertEqual(status, 503)
        self.assertFalse(data["ok"])

    def test_socket_mode(self):
        self.assertEqual(stat.S_IMODE(self.socket_path.stat().st_mode), 0o660)


if __name__ == "__main__":
    unittest.main()
