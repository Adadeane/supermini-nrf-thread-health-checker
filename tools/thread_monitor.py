#!/usr/bin/env python3
"""
SuperMini nRF52840 Thread Network Health Checker & Matter Pairing Assistant
Host Companion Tool

Monitors USB serial telemetry from the SuperMini, renders live signal strength meters,
displays Matter onboarding credentials / QR code for Google Home, and provides an
interactive OpenThread CLI prompt.

Copyright (c) 2026 Antigravity
SPDX-License-Identifier: Apache-2.0
"""

import sys
import time
import argparse
import threading
import re
import csv
from datetime import datetime

try:
    import serial
    import serial.tools.list_ports
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

# Safe characters for signal meters
USE_UNICODE = True
try:
    "█░★☆".encode(sys.stdout.encoding or "utf-8")
except Exception:
    USE_UNICODE = False

BAR_FULL = "█" if USE_UNICODE else "#"
BAR_EMPTY = "░" if USE_UNICODE else "-"
STAR_FULL = "★" if USE_UNICODE else "*"
STAR_EMPTY = "☆" if USE_UNICODE else "."

try:
    import colorama
    from colorama import Fore, Style, Back
    colorama.init(autoreset=True)
    HAS_COLOR = True
except ImportError:
    HAS_COLOR = False
    class Fore:
        GREEN = RED = YELLOW = CYAN = MAGENTA = BLUE = WHITE = RESET = ""
    class Style:
        BRIGHT = DIM = NORMAL = RESET_ALL = ""

try:
    import segno
    HAS_SEGNO = True
except ImportError:
    HAS_SEGNO = False

# Standard Matter Test Pairing Data
MATTER_PAYLOAD = "MT:Y.K9042C00KA0648G00"
MANUAL_CODE    = "3497-011-2332"
PASSCODE       = 20202021
DISCRIMINATOR  = 3840
VENDOR_ID      = "0xFFF1 (Test VID)"
PRODUCT_ID     = "0x8000 (Test PID)"
DEVICE_TYPE    = "Matter On/Off Light (0x0100)"

def render_rssi_bar(rssi_dbm):
    """Render a colored ASCII signal strength meter from dBm."""
    if rssi_dbm is None:
        return "[          ] N/A"
    
    # Scale from -100 dBm (0 bars) to -40 dBm (10 bars)
    clamped = max(-100, min(-40, rssi_dbm))
    num_bars = int(round((clamped - (-100)) / 60.0 * 10))
    bar_str = BAR_FULL * num_bars + BAR_EMPTY * (10 - num_bars)

    if rssi_dbm >= -60:
        color = Fore.GREEN
    elif rssi_dbm >= -70:
        color = Fore.CYAN
    elif rssi_dbm >= -80:
        color = Fore.YELLOW
    else:
        color = Fore.RED

    return f"{color}[{bar_str}] {rssi_dbm:>4} dBm{Style.RESET_ALL}"

def render_lqi(lqi):
    """Render 0-3 LQI rating with stars."""
    if lqi is None:
        return "N/A"
    stars = STAR_FULL * min(3, max(0, int(lqi))) + STAR_EMPTY * (3 - min(3, max(0, int(lqi))))
    if lqi == 3:
        return f"{Fore.GREEN}{stars} (Best){Style.RESET_ALL}"
    elif lqi == 2:
        return f"{Fore.CYAN}{stars} (Good){Style.RESET_ALL}"
    elif lqi == 1:
        return f"{Fore.YELLOW}{stars} (Weak){Style.RESET_ALL}"
    else:
        return f"{Fore.RED}{stars} (Poor){Style.RESET_ALL}"

def print_matter_pairing_card():
    """Print the Google Home Matter onboarding card and QR code."""
    border = "=" * 70
    print(f"\n{Fore.CYAN}{Style.BRIGHT}{border}")
    print("       GOOGLE HOME MATTER COMMISSIONING ASSISTANT")
    print(f"{border}{Style.RESET_ALL}")
    print(f"Device Type:       {Fore.WHITE}{Style.BRIGHT}{DEVICE_TYPE}{Style.RESET_ALL}")
    print(f"Vendor / Product:  {VENDOR_ID} / {PRODUCT_ID}")
    print(f"Discriminator:     {DISCRIMINATOR} (0x{DISCRIMINATOR:03X})")
    print(f"Setup Passcode:    {PASSCODE}")
    print(f"\n{Fore.YELLOW}{Style.BRIGHT}MANUAL SETUP CODE:  {Fore.GREEN}{MANUAL_CODE}{Style.RESET_ALL}")
    print(f"Matter QR Payload: {Fore.WHITE}{MATTER_PAYLOAD}{Style.RESET_ALL}")
    
    if HAS_SEGNO:
        print(f"\n{Fore.CYAN}[ SCAN WITH GOOGLE HOME APP ]:{Style.RESET_ALL}\n")
        qr = segno.make(MATTER_PAYLOAD)
        qr.terminal(compact=True)
    else:
        qr_url = f"https://api.qrserver.com/v1/create-qr-code/?size=250x250&data={MATTER_PAYLOAD}"
        print(f"\nScan QR Code in browser: {Fore.BLUE}{qr_url}{Style.RESET_ALL}")
        print(f"{Style.DIM}(Tip: install 'segno' to render QR code directly in this terminal: pip install segno){Style.RESET_ALL}")

    print(f"\n{Fore.CYAN}GOOGLE HOME APP PAIRING STEPS:{Style.RESET_ALL}")
    print("  1. Open Google Home app on your phone (Bluetooth enabled).")
    print("  2. Tap '+' in top left -> 'Set up a device' -> 'Matter-enabled device'.")
    print(f"  3. Scan the QR code above, OR tap 'Set up without barcode' and enter: {Fore.GREEN}{MANUAL_CODE}{Style.RESET_ALL}")
    print("  4. Google Home will provision your Nest Thread network credentials via BLE.")
    print(f"{Fore.CYAN}{border}\n{Style.RESET_ALL}")

def auto_detect_port():
    """Find the SuperMini nRF52840 COM port."""
    if not HAS_SERIAL:
        return None

    ports = list(serial.tools.list_ports.comports())
    if not ports:
        return None

    for p in ports:
        desc = (p.description or "").lower()
        mfg = (p.manufacturer or "").lower()
        vid = p.vid or 0
        pid = p.pid or 0

        # Nordic semiconductor VID 0x1915 or Adafruit 0x239A
        if vid in (0x1915, 0x239A) or "supermini" in desc or "nordic" in desc or "cdc" in desc or "nrf" in desc:
            return p.device

    # Fallback to the first available COM port
    return ports[0].device

class ThreadTelemetryLogger:
    def __init__(self, filename=None):
        self.filename = filename
        self.file = None
        self.writer = None
        if filename:
            self.file = open(filename, "w", newline="", encoding="utf-8")
            self.writer = csv.writer(self.file)
            self.writer.writerow(["Timestamp", "Role", "Channel", "PAN_ID", "Leader_RSSI", "Neighbors_Count", "BorderRouter_RTT_ms"])

    def log(self, role, channel, pan_id, rssi, neighbors, rtt):
        if self.writer:
            self.writer.writerow([
                datetime.now().isoformat(),
                role, channel, pan_id, rssi, neighbors, rtt
            ])
            self.file.flush()

    def close(self):
        if self.file:
            self.file.close()

def parse_and_enhance_line(line, logger=None):
    """Enhance raw serial output with live signal strength meters."""
    # Pattern for neighbor line
    # Ex: "  1 | 0x3400 | F4:12:... | -54 dBm | -52 dBm | 36 dB | 3 | 0.00%"
    neighbor_match = re.search(r'\|\s*(-?\d+)\s*dBm\s*\|\s*(-?\d+)\s*dBm\s*\|\s*(\d+)\s*dB\s*\|\s*(\d)\s*\|', line)
    if neighbor_match:
        avg_rssi = int(neighbor_match.group(1))
        last_rssi = int(neighbor_match.group(2))
        margin = int(neighbor_match.group(3))
        lqi = int(neighbor_match.group(4))

        meter = render_rssi_bar(last_rssi)
        lqi_str = render_lqi(lqi)
        return f"{line.rstrip()}  {meter}  {lqi_str}\n"

    # Pattern for Parent Link line
    # Ex: "PARENT LINK: Avg RSSI: -54 dBm | LQI: 3/3 | Link Margin: 36 dB"
    parent_match = re.search(r'Avg RSSI:\s*(-?\d+)\s*dBm\s*\|\s*LQI:\s*(\d)/3', line)
    if parent_match:
        rssi = int(parent_match.group(1))
        lqi = int(parent_match.group(2))
        return f"{line.rstrip()}  => {render_rssi_bar(rssi)} {render_lqi(lqi)}\n"

    # Pattern for Google Nest Border Router line
    # Ex: "GOOGLE NEST BORDER ROUTER: ONLINE | Ping Latency: 18 ms"
    ping_match = re.search(r'Ping Latency:\s*(\d+)\s*ms', line)
    if ping_match:
        rtt = int(ping_match.group(1))
        if rtt < 30:
            color = Fore.GREEN
        elif rtt < 80:
            color = Fore.YELLOW
        else:
            color = Fore.RED
        return f"{line.rstrip()}  {color}[Fast RTT: {rtt} ms]{Style.RESET_ALL}\n"

    return line

def serial_reader_thread(ser, stop_event, logger=None):
    """Read lines from serial port and print enhanced dashboard."""
    while not stop_event.is_set():
        try:
            if ser.in_waiting > 0:
                raw_line = ser.readline().decode('utf-8', errors='replace')
                if raw_line:
                    enhanced = parse_and_enhance_line(raw_line, logger)
                    sys.stdout.write(enhanced)
                    sys.stdout.flush()
            else:
                time.sleep(0.02)
        except Exception as e:
            if not stop_event.is_set():
                print(f"\n{Fore.RED}Serial read error: {e}{Style.RESET_ALL}")
            break

def run_simulation():
    """Run a simulated telemetry session for demo/testing without hardware."""
    print(f"\n{Fore.MAGENTA}*** RUNNING IN SIMULATION / DEMO MODE ***{Style.RESET_ALL}\n")
    print_matter_pairing_card()

    sim_lines = [
        "\r\n======================================================================",
        "[THREAD HEALTH CHECKER - SuperMini nRF52840]",
        "State: ROUTER           | Channel: 15 | PAN ID: 0x4A21",
        "Network Name: Google-Thread-4A21 | RLOC16: 0x3401 | Partition ID: 0x82A1B012",
        "----------------------------------------------------------------------",
        "PARENT LINK: Avg RSSI: -55 dBm | LQI: 3/3 | Link Margin: 35 dB",
        "----------------------------------------------------------------------",
        "NEIGHBOR NODES (2 discovered):",
        "  # | RLOC16 | Extended MAC Address   | Avg RSSI | Last RSSI | Margin | LQI | FER",
        "  1 | 0x3400 | F4:12:FA:01:23:45:67:89 |   -54 dBm |   -52 dBm |  36 dB |  3  | 0.00%",
        "  2 | 0x5802 | E2:34:BC:55:AA:01:22:11 |   -78 dBm |   -79 dBm |  12 dB |  2  | 1.50%",
        "----------------------------------------------------------------------",
        "GOOGLE NEST BORDER ROUTER: ONLINE | Ping Latency: 18 ms",
        "======================================================================\r\n"
    ]

    for line in sim_lines:
        enhanced = parse_and_enhance_line(line)
        sys.stdout.write(enhanced + ("\n" if not enhanced.endswith("\n") else ""))
        time.sleep(0.1)

    print(f"\n{Fore.GREEN}Simulation finished successfully.{Style.RESET_ALL}")

def main():
    parser = argparse.ArgumentParser(description="SuperMini nRF52840 Thread Health Checker Monitor")
    parser.add_argument("-p", "--port", default=None, help="Serial port (e.g. COM3 or /dev/ttyACM0)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("-l", "--log", default=None, help="Save telemetry to CSV file")
    parser.add_argument("--sim", action="store_true", help="Run simulated telemetry demo without hardware")
    parser.add_argument("--info-only", action="store_true", help="Display Matter pairing card and exit")

    args = parser.parse_args()

    if args.info_only:
        print_matter_pairing_card()
        return

    if args.sim or not HAS_SERIAL:
        if not HAS_SERIAL and not args.sim:
            print(f"{Fore.YELLOW}Warning: pyserial is not installed. Running in simulation mode.{Style.RESET_ALL}")
        run_simulation()
        return

    port = args.port or auto_detect_port()
    if not port:
        print(f"{Fore.RED}No serial port detected! Connect your SuperMini nRF52840 via USB.{Style.RESET_ALL}")
        print("Available ports:")
        for p in serial.tools.list_ports.comports():
            print(f"  - {p.device}: {p.description}")
        sys.exit(1)

    print(f"{Fore.CYAN}Connecting to SuperMini on {Fore.WHITE}{Style.BRIGHT}{port}{Style.RESET_ALL} at {args.baud} baud...")

    try:
        ser = serial.Serial(port, args.baud, timeout=0.5)
    except Exception as e:
        print(f"{Fore.RED}Failed to open port {port}: {e}{Style.RESET_ALL}")
        sys.exit(1)

    print_matter_pairing_card()

    logger = ThreadTelemetryLogger(args.log) if args.log else None
    if args.log:
        print(f"{Fore.GREEN}Logging telemetry to: {args.log}{Style.RESET_ALL}")

    print(f"{Fore.GREEN}Live monitor active. Type OpenThread CLI commands (e.g., 'state', 'neighbor table', 'ping') below.{Style.RESET_ALL}")
    print(f"{Style.DIM}(Press Ctrl+C to exit){Style.RESET_ALL}\n")

    stop_event = threading.Event()
    reader = threading.Thread(target=serial_reader_thread, args=(ser, stop_event, logger), daemon=True)
    reader.start()

    try:
        while True:
            cmd = input()
            if cmd.strip():
                # Send command to OpenThread CLI over USB
                ser.write((cmd + "\r\n").encode('utf-8'))
    except KeyboardInterrupt:
        print(f"\n{Fore.YELLOW}Exiting monitor...{Style.RESET_ALL}")
    finally:
        stop_event.set()
        ser.close()
        if logger:
            logger.close()

if __name__ == "__main__":
    main()
