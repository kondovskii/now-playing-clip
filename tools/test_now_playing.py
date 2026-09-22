#!/usr/bin/env python3
"""
Verify the Spotify side of now-playing-clip without any hardware.

Reads the same secrets.h the firmware uses, refreshes the access token,
and polls currently-playing — printing exactly the fields the ESP32 will
parse, including the album art sizes.

    python test_now_playing.py

Ctrl-C to stop.
"""

import re
import sys
import time
from pathlib import Path

import requests

SECRETS = Path(__file__).resolve().parents[1] / \
    "firmware" / "now-playing-clip" / "secrets.h"

TOKEN_URL = "https://accounts.spotify.com/api/token"
NOW_URL = "https://api.spotify.com/v1/me/player/currently-playing"


def read_secrets():
    if not SECRETS.exists():
        sys.exit(f"Can't find {SECRETS}\nRun get_refresh_token.py first.")

    text = SECRETS.read_text(encoding="utf-8", errors="replace")
    out = {}
    for key in ("SPOTIFY_BASIC_AUTH", "SPOTIFY_REFRESH_TOKEN"):
        m = re.search(rf'#define\s+{key}\s+"([^"]*)"', text)
        if not m or not m.group(1) or "paste" in m.group(1):
            sys.exit(f"{key} is missing or still a placeholder in secrets.h")
        out[key] = m.group(1)
    return out


def get_access_token(basic, refresh):
    r = requests.post(
        TOKEN_URL,
        headers={"Authorization": f"Basic {basic}"},
        data={"grant_type": "refresh_token", "refresh_token": refresh},
        timeout=20,
    )
    if r.status_code != 200:
        sys.exit(
            f"Token refresh failed ({r.status_code}): {r.text}\n\n"
            "401 usually means the client id/secret pair is wrong.\n"
            "400 usually means the refresh token is wrong or revoked."
        )
    return r.json()["access_token"]


def poll(access):
    r = requests.get(
        NOW_URL, headers={"Authorization": f"Bearer {access}"}, timeout=20
    )

    if r.status_code == 204:
        print("  nothing playing (204)")
        return True
    if r.status_code == 401:
        return False          # caller refreshes
    if r.status_code == 403:
        sys.exit(
            "403 Forbidden. The app probably lacks the "
            "user-read-currently-playing scope. Re-run get_refresh_token.py."
        )
    if r.status_code == 429:
        wait = int(r.headers.get("Retry-After", "5"))
        print(f"  rate limited, waiting {wait}s")
        time.sleep(wait)
        return True
    if r.status_code != 200:
        print(f"  HTTP {r.status_code}: {r.text[:200]}")
        return True

    d = r.json()
    item = d.get("item") or {}
    title = item.get("name", "")
    artists = ", ".join(a.get("name", "") for a in item.get("artists", []))
    playing = d.get("is_playing", False)

    if not title:
        print("  no track in response")
        return True

    state = "playing" if playing else "paused"
    print(f"  [{state}] {title} - {artists}")

    images = (item.get("album") or {}).get("images", [])
    if images:
        sizes = ", ".join(f"{i.get('width')}px" for i in images)
        print(f"     art sizes: {sizes}")
        # Same choice the firmware makes
        best = min(images, key=lambda i: abs((i.get("width") or 0) - 300))
        print(f"     firmware picks: {best.get('width')}px")
    else:
        print("     no album art in response")

    return True


def main():
    s = read_secrets()
    print(f"secrets.h: ok ({SECRETS})")

    access = get_access_token(s["SPOTIFY_BASIC_AUTH"],
                              s["SPOTIFY_REFRESH_TOKEN"])
    print("access token: ok\n")
    print("Polling every 5s. Play something in Spotify. Ctrl-C to stop.\n")

    try:
        while True:
            if not poll(access):
                print("  token expired, refreshing")
                access = get_access_token(s["SPOTIFY_BASIC_AUTH"],
                                          s["SPOTIFY_REFRESH_TOKEN"])
            time.sleep(5)
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
