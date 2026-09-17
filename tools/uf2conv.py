#!/usr/bin/env python3
#
# Microsoft UF2 conversion tool (with nRF52840 family ID support)
# SPDX-License-Identifier: MIT
#

import sys
import struct
import argparse
import os

UF2_MAGIC_START0 = 0x0A324655 # "UF2\n"
UF2_MAGIC_START1 = 0x9E5D5157 # Randomly selected
UF2_MAGIC_END    = 0x0AB165E2 # "-ELF"

INFO_FILE = "/INFO_UF2.TXT"

# Family IDs
FAMILY_MAP = {
    "nrf52840": 0xada52840,
    "nrf52": 0xada52840,
    "rp2040": 0xe484ff56,
    "samd21": 0x68ed2b88,
    "samd51": 0x55114460,
    "stm32f4": 0x57755a57,
    "esp32s2": 0xbfdd4eee,
    "esp32s3": 0xc47e5767,
}

def is_hex(filename):
    try:
        with open(filename, "r") as f:
            first_line = f.readline()
            return first_line.startswith(":")
    except:
        return False

def convert_hex(filename):
    data = bytearray()
    upper = 0
    with open(filename, "r") as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            byte_count = int(line[1:3], 16)
            address = int(line[3:7], 16)
            record_type = int(line[7:9], 16)
            rec_data = bytes.fromhex(line[9:9 + 2 * byte_count])
            
            if record_type == 0: # Data
                full_addr = upper + address
                if len(data) < full_addr + byte_count:
                    data.extend(b'\xff' * (full_addr + byte_count - len(data)))
                data[full_addr:full_addr + byte_count] = rec_data
            elif record_type == 1: # EOF
                break
            elif record_type == 4: # Extended Linear Address
                upper = int(line[9:13], 16) << 16
    return data

def convert_to_uf2(data, base_addr, family_id):
    num_blocks = (len(data) + 255) // 256
    out = bytearray()
    for block_no in range(num_blocks):
        ptr = block_no * 256
        chunk = data[ptr:ptr + 256]
        flags = 0x00002000 if family_id else 0x00000000
        header = struct.pack(
            "<IIIIIIII",
            UF2_MAGIC_START0,
            UF2_MAGIC_START1,
            flags,
            base_addr + ptr,
            256,
            block_no,
            num_blocks,
            family_id or 0
        )
        block = header + chunk + b'\x00' * (476 - len(chunk)) + struct.pack("<I", UF2_MAGIC_END)
        out.extend(block)
    return out

def main():
    parser = argparse.ArgumentParser(description="Convert binary/hex to UF2 format")
    parser.add_argument("input", help="Input .bin or .hex file")
    parser.add_argument("-b", "--base", default="0x26000", help="Base address (default: 0x26000)")
    parser.add_argument("-o", "--output", default=None, help="Output .uf2 file")
    parser.add_argument("-f", "--family", default="0xada52840", help="Family ID or name (default: 0xada52840 / nrf52840)")
    parser.add_argument("-c", "--convert", action="store_true", help="Perform conversion")

    args = parser.parse_args()

    base_addr = int(args.base, 0)
    if args.family.lower() in FAMILY_MAP:
        family_id = FAMILY_MAP[args.family.lower()]
    else:
        family_id = int(args.family, 0)

    if not args.output:
        base, _ = os.path.splitext(args.input)
        args.output = base + ".uf2"

    if is_hex(args.input):
        print(f"Parsing Intel HEX: {args.input}")
        raw = convert_hex(args.input)
        if base_addr < len(raw):
            data = raw[base_addr:]
        else:
            data = raw
    else:
        with open(args.input, "rb") as f:
            data = f.read()

    print(f"Converting {len(data)} bytes to UF2 (Base Address: 0x{base_addr:X}, Family: 0x{family_id:X})...")
    uf2 = convert_to_uf2(data, base_addr, family_id)

    with open(args.output, "wb") as f:
        f.write(uf2)

    print(f"Wrote {len(uf2)} bytes to {args.output}")

if __name__ == "__main__":
    main()
