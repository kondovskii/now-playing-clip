# now-playing-clip

A wearable hair clip that shows whatever is currently playing on Spotify.

Hardware: LILYGO T-Display-S3 (ESP32-S3, 1.9" 170x320 ST7789) + 3.7V LiPo.

## How it works

1. You authorise the app **once**, in a browser on your laptop, using
   `tools/get_refresh_token.py`. That gives you a **refresh token**.
2. The refresh token goes into `firmware/now-playing-clip/secrets.h`.
3. On the clip, the firmware trades that refresh token for a short-lived
   **access token** every ~50 minutes, and uses the access token to poll
   Spotify for the current track every few seconds.

Refresh tokens don't expire unless you revoke them. Step 1 happens once, ever.

---

## Step 1 — Register a Spotify app

1. Go to <https://developer.spotify.com/dashboard> and log in.
2. **Create app**. Name and description can be anything.
3. Redirect URI: `http://127.0.0.1:8888/callback`
   - Use the literal IP `127.0.0.1`, **not** `localhost`. Spotify rejects
     `localhost` for new apps.
4. Under APIs used, tick **Web API**.
5. Save, then open **Settings** and copy the **Client ID** and **Client secret**.

You do not need to submit anything for review. Development mode allows up to
25 users, and you only need one — yourself.

## Step 2 — Get your refresh token

On your laptop (not the ESP32):

```bash
cd tools
pip install requests
python get_refresh_token.py
```

It will:

- ask for your Client ID and Client secret
- open your browser to Spotify's consent page
- catch the redirect on `127.0.0.1:8888`
- exchange the code for tokens
- print a ready-to-paste `secrets.h`

Copy that output into `firmware/now-playing-clip/secrets.h`.

## Step 3 — Arduino IDE setup

Install:

- **esp32** boards package by Espressif (Boards Manager)
- **ArduinoJson** by Benoit Blanchon (Library Manager)
- **TFT_eSPI** by Bodmer (Library Manager)
- **TJpg_Decoder** by Bodmer (Library Manager) — for album art

TFT_eSPI needs the T-Display-S3 pin configuration, which is not in the stock
library. Get it from LILYGO's `T-Display-S3` repo on GitHub and follow their
instructions for copying their setup files into your `TFT_eSPI` library folder.

Board settings that matter:

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | **Enabled** |
| Flash Size | 16MB (128Mb) |
| PSRAM | OPI PSRAM |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |

`USB CDC On Boot: Enabled` is the one people miss. Without it you get no
serial output and no way to see what's wrong.

## Step 4 — Flash and watch serial

Open the Serial Monitor at 115200 before you worry about the screen. In order
you should see: WiFi connected, token refreshed, then a track title every few
seconds. If all three appear, the hard part is done.

---

## Layout

Album art sits in a 150x150 box on the left; the title and artist scroll in a
band on the right. The two regions never overlap, so the art is drawn once
when the track changes and only the text band is redrawn each frame.

Spotify returns several art sizes (usually 640, 300 and 64 px). The sketch
picks whichever is closest to 300 and decodes it at 1:2, landing at 150 px.

To resize the art, change `ART_SZ` and `TEXT_X` together — `TEXT_W` is derived
from `TEXT_X`, so the sprite follows automatically.

## Gotchas

- **GPIO15 must be driven HIGH** or the screen stays black when running on
  battery. It works fine over USB without it, which is how this wastes an
  evening. Already handled in the sketch.
- **Nothing playing** returns HTTP 204 with an empty body, not an error.
- **Private sessions** return nothing. If the screen says idle while music is
  playing, check that.
- **Album art download blocks for a second or two** on each track change,
  while the scroll sits still. Expected. Moving the HTTP work to the second
  core with FreeRTOS is the proper fix, once the simple version works.
- Art is only refetched when the image URL changes, not on every poll.
- `setInsecure()` skips TLS certificate verification. Fine for a hobby build on
  your own network; swap in a root CA if you ever care.

## Repo layout

```
now-playing-clip/
├── tools/
│   └── get_refresh_token.py    run once on your laptop
└── firmware/
    └── now-playing-clip/
        ├── now-playing-clip.ino
        └── secrets.h.example   copy to secrets.h and fill in
```

`secrets.h` is gitignored. Don't commit it — it contains your client secret
and a token that grants access to your Spotify account.
