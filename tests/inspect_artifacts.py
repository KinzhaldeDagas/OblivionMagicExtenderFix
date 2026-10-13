"""Read-only PE identity and OBME adapter signature audit; uses only the standard library."""
from pathlib import Path
import hashlib
import json
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
PATTERNS = {
    "load_adapter": rb"\x55\x54\xFF\x74\x24\x0C\xE8....\x83\xC4\x08\x5D\xC3",
    "icon_adapter": rb"\x83\xE9\x24\xE8....\xC2\x08\x00",
}

def inspect(path):
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    machine, sections = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    image_base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    table = pe + 24 + optional_size
    spans = []
    for i in range(sections):
        offset = table + i * 40
        virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, offset + 8)
        flags = struct.unpack_from("<I", data, offset + 36)[0]
        if flags & 0x20000000:
            spans.append((raw, raw_size, rva))
    signatures = {}
    for name, pattern in PATTERNS.items():
        addresses = []
        for raw, size, rva in spans:
            for match in re.finditer(pattern, data[raw:raw+size], re.DOTALL):
                addresses.append(hex(image_base + rva + match.start()))
        signatures[name] = addresses
    return {"file": str(path), "bytes": len(data), "machine": hex(machine),
            "sha256": hashlib.sha256(data).hexdigest(), "signatures": signatures}

if __name__ == "__main__":
    paths = [ROOT / "Output/Release/OBMEFix.dll"]
    print(json.dumps([inspect(p) for p in paths if p.exists()], indent=2))
