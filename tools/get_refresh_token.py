#!/usr/bin/env python3
"""
One-time Spotify authorisation helper for now-playing-clip.

Run this on your laptop. It opens a browser, catches Spotify's redirect,
and prints a ready-to-paste secrets.h containing your refresh token.

    pip install requests
    python get_refresh_token.py
"""

import base64
import http.server
import socketserver
import threading
import urllib.parse
import webbrowser
import secrets as pysecrets
import sys

import requests

REDIRECT_URI = "http://127.0.0.1:8888/callback"
PORT = 8888
SCOPE = "user-read-currently-playing user-read-playback-state"

AUTH_URL = "https://accounts.spotify.com/authorize"
TOKEN_URL = "https://accounts.spotify.com/api/token"

# Filled in by the callback handler
received = {}


class CallbackHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/callback":
            self.send_response(404)
            self.end_headers()
            return

        params = urllib.parse.parse_qs(parsed.query)
        received["code"] = params.get("code", [None])[0]
        received["state"] = params.get("state", [None])[0]
        received["error"] = params.get("error", [None])[0]

        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.end_headers()
        if received["error"]:
            body = f"<h2>Authorisation failed: {received['error']}</h2>"
        else:
            body = "<h2>Done. You can close this tab and go back to the terminal.</h2>"
        self.wfile.write(body.encode("utf-8"))

    def log_message(self, *args):
        pass  # keep the terminal clean


def main():
    print("Spotify credentials (from your app's Settings page)\n")
    client_id = input("  Client ID:     ").strip()
    client_secret = input("  Client secret: ").strip()

    if not client_id or not client_secret:
        sys.exit("Both values are required.")

    state = pysecrets.token_urlsafe(16)
    query = urllib.parse.urlencode(
        {
            "client_id": client_id,
            "response_type": "code",
            "redirect_uri": REDIRECT_URI,
            "scope": SCOPE,
            "state": state,
        }
    )
    url = f"{AUTH_URL}?{query}"

    # Serve exactly one request, then stop.
    socketserver.TCPServer.allow_reuse_address = True
    server = socketserver.TCPServer(("127.0.0.1", PORT), CallbackHandler)
    thread = threading.Thread(target=server.handle_request, daemon=True)
    thread.start()

    print(f"\nOpening your browser. If it doesn't open, paste this:\n\n{url}\n")
    webbrowser.open(url)

    thread.join(timeout=300)
    server.server_close()

    if received.get("error"):
        sys.exit(f"Spotify returned an error: {received['error']}")
    if not received.get("code"):
        sys.exit("Timed out waiting for the redirect.")
    if received.get("state") != state:
        sys.exit("State mismatch — aborting.")

    basic = base64.b64encode(
        f"{client_id}:{client_secret}".encode("utf-8")
    ).decode("ascii")

    resp = requests.post(
        TOKEN_URL,
        headers={"Authorization": f"Basic {basic}"},
        data={
            "grant_type": "authorization_code",
            "code": received["code"],
            "redirect_uri": REDIRECT_URI,
        },
        timeout=20,
    )

    if resp.status_code != 200:
        sys.exit(f"Token exchange failed ({resp.status_code}): {resp.text}")

    refresh_token = resp.json().get("refresh_token")
    if not refresh_token:
        sys.exit("No refresh token in the response. Try authorising again.")

    print("\n" + "=" * 68)
    print("Paste this into firmware/now-playing-clip/secrets.h")
    print("=" * 68 + "\n")
    print("#pragma once\n")
    print('#define WIFI_SSID           "your-network"')
    print('#define WIFI_PASSWORD       "your-password"\n')
    print(f'#define SPOTIFY_BASIC_AUTH  "{basic}"')
    print(f'#define SPOTIFY_REFRESH_TOKEN "{refresh_token}"')
    print("\n" + "=" * 68)
    print("\nSPOTIFY_BASIC_AUTH is your client id and secret, base64 encoded,")
    print("ready for the Authorization header. Treat both lines as passwords.")


if __name__ == "__main__":
    main()
