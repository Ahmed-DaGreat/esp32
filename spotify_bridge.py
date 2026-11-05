import ctypes
import time
import socket
import subprocess
import sys
import urllib.parse
import urllib.request
import json
import re

# Auto-install pyserial if missing so we can beam data straight down COM7 USB!
try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Installing pyserial for direct USB COM7 connection...")
    subprocess.run([sys.executable, "-m", "pip", "install", "pyserial"], check=False)
    import serial
    import serial.tools.list_ports

UDP_PORT = 4210
SERIAL_PORT = "COM7"  # Your ESP32 port from Arduino IDE!

def open_esp_serial():
    ports_to_try = [SERIAL_PORT]
    for p in serial.tools.list_ports.comports():
        if p.device not in ports_to_try:
            ports_to_try.append(p.device)
    for port_name in ports_to_try:
        try:
            ser = serial.Serial(port_name, 115200, timeout=0.05, write_timeout=0.1)
            print(f"[USB CONNECTED!] Linked directly to ESP32 on {port_name}!")
            return ser
        except Exception:
            pass
    return None

def get_broadcast_ips():
    ips = ["255.255.255.255"]
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
        s.close()
        parts = local_ip.split(".")
        if len(parts) == 4:
            ips.insert(0, f"{parts[0]}.{parts[1]}.{parts[2]}.255")
    except Exception:
        pass
    return ips

user32 = ctypes.windll.user32
EnumWindows = user32.EnumWindows
EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
GetWindowText = user32.GetWindowTextW
GetWindowTextLength = user32.GetWindowTextLengthW
GetClassName = user32.GetClassNameW
IsWindowVisible = user32.IsWindowVisible

IGNORED_APPS = [
    "Chrome", "Edge", "Brave", "Firefox", "Opera",
    "Visual Studio", "Code", "Arduino", "PowerShell",
    "Command Prompt", "Discord", "WhatsApp", "Explorer"
]

def get_spotify_window_title():
    titles = []
    def foreach_window(hwnd, lParam):
        if IsWindowVisible(hwnd):
            class_buff = ctypes.create_unicode_buffer(256)
            GetClassName(hwnd, class_buff, 256)
            if "Chrome_WidgetWin" in class_buff.value:
                length = GetWindowTextLength(hwnd)
                if length > 0:
                    buff = ctypes.create_unicode_buffer(length + 1)
                    GetWindowText(hwnd, buff, length + 1)
                    val = buff.value.strip()
                    if " - " in val and not any(b.lower() in val.lower() for b in IGNORED_APPS):
                        titles.append(val)
        return True
    EnumWindows(EnumWindowsProc(foreach_window), 0)
    return titles[0] if titles else None

def clean_ascii(text):
    return text.encode("ascii", "ignore").decode("ascii").replace("|", "/").strip()

def fetch_synced_lyrics(artist, title):
    for a, t in [(artist, title), (title, artist)]:
        try:
            q = urllib.parse.urlencode({"artist_name": a, "track_name": t})
            url = f"https://lrclib.net/api/get?{q}"
            req = urllib.request.Request(url, headers={"User-Agent": "DeskBot/2.2"})
            with urllib.request.urlopen(req, timeout=3.5) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                lrc_text = data.get("syncedLyrics") or ""
                dur_ms = int((data.get("duration") or 180) * 1000)
                parsed = []
                for line in lrc_text.splitlines():
                    m = re.match(r"\[(\d+):(\d+\.\d+)\](.*)", line)
                    if m:
                        ms = int(m.group(1)) * 60000 + int(float(m.group(2)) * 1000)
                        txt = clean_ascii(m.group(3))
                        if txt:
                            parsed.append((ms, txt))
                if parsed:
                    return parsed, dur_ms
        except Exception:
            pass
    return [], 180000

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
bcast_targets = get_broadcast_ips()
esp_serial = open_esp_serial()

print("=========================================================")
print("DeskBot Bridge v2.2 LIVE (USB COM7 + Wi-Fi Broadcast)")
print("=========================================================")

last_song_key = ""
lyrics_list = []
song_start_time = time.time()
song_dur_ms = 180000
missed_polls = 0
cached_artist = ""
cached_title = ""

while True:
    win_title = get_spotify_window_title()

    if win_title and " - " in win_title:
        missed_polls = 0
        parts = win_title.split(" - ", 1)
        cached_artist = clean_ascii(parts[0])
        cached_title = clean_ascii(parts[1])
    else:
        missed_polls += 1

    if missed_polls <= 6 and cached_title:
        song_key = f"{cached_artist} - {cached_title}"

        if song_key != last_song_key:
            last_song_key = song_key
            song_start_time = time.time()
            print(f"\n[PLAYING] {cached_title} by {cached_artist}")
            lyrics_list, song_dur_ms = fetch_synced_lyrics(cached_artist, cached_title)
            print(f"[LYRICS READY] Loaded {len(lyrics_list)} synced lines!")

        elapsed_ms = int((time.time() - song_start_time) * 1000)
        current_lyric = "~ Vibing to the music ~"
        for ms, txt in lyrics_list:
            if elapsed_ms >= ms:
                current_lyric = txt
            else:
                break

        packet = f"SPOT|1|{cached_title}|{cached_artist}|{elapsed_ms}|{song_dur_ms}|{current_lyric}\n"
    else:
        if last_song_key != "":
            print("\n[PAUSED] Spotify playback paused.")
            last_song_key = ""
        packet = "SPOT|0||||0|180000|\n"

    # 1. Send directly over USB Cable (COM7)!
    if esp_serial is None:
        esp_serial = open_esp_serial()
    if esp_serial is not None:
        try:
            esp_serial.write(packet.encode("ascii", "ignore"))
        except Exception:
            try:
                esp_serial.close()
            except Exception:
                pass
            esp_serial = None

    # 2. Also broadcast over Wi-Fi UDP!
    for ip in bcast_targets:
        try:
            sock.sendto(packet.strip().encode("ascii", "ignore"), (ip, UDP_PORT))
        except Exception:
            pass

    time.sleep(0.5)