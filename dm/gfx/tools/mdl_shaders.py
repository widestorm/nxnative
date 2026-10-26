#!/usr/bin/env python3

"""
Extract NVN shaders embedded in model files.

Supported layout:

    [NVN code area][0x880-byte NVN control section]

The code-area length is stored at control +0x704. The code immediately
precedes the control section.

Optional control ranges can be normalized before calculating shader identity.
War in the North patches non-semantic identifiers at:

    0x778:0x780
    0x7d0:0x7e0

Usage:

    mdl_shaders.py <RomFS> <package-dir> <shader-map.tsv> \
        [--zero START:END[,START:END...]]

Example:

    mdl_shaders.py RomFS shader/pkg-mdl shader_map-mdl.tsv \
        --zero 0x778:0x780,0x7d0:0x7e0
"""

import argparse
import hashlib
import struct
from collections import Counter, defaultdict
from pathlib import Path


CTL_MAGIC = bytes.fromhex("34127698")
GFX_MAGIC = bytes.fromhex("78563412")
CS_MAGIC = bytes.fromhex("79563412")

STAGE_NAMES = {
    "vs": "vertex",
    "hs": "hull",
    "ds": "domain",
    "gs": "geometry",
    "ps": "pixel",
    "cs": "compute",
}

SPH_STAGES = {
    1: "vs",
    2: "hs",
    3: "ds",
    4: "gs",
    5: "ps",
}


def parse_ranges(text):
    ranges = []

    if not text:
        return ()

    for entry in text.split(","):
        entry = entry.strip()

        if not entry:
            continue

        try:
            start_text, end_text = entry.split(":", 1)
            start = int(start_text, 0)
            end = int(end_text, 0)
        except (ValueError, TypeError):
            raise SystemExit(f"[RED] invalid normalization range: {entry!r}")

        if start < 0 or end <= start or end > 0x880:
            raise SystemExit(f"[RED] invalid normalization range: {entry!r}")

        ranges.append((start, end))

    ranges.sort()

    for previous, current in zip(ranges, ranges[1:]):
        if current[0] < previous[1]:
            raise SystemExit(
                f"[RED] overlapping normalization ranges: "
                f"{previous!r} and {current!r}"
            )

    return tuple(ranges)


def control_length(control):
    if len(control) < 0x24:
        return len(control)

    own_length = struct.unpack_from("<I", control, 0x20)[0]

    if 0x760 <= own_length < 0x880:
        return own_length

    return 0x880


def normalized_control(control, zero_ranges):
    length = control_length(control)
    output = bytearray(control[:length])

    for start, end in zero_ranges:
        if start >= length:
            continue

        end = min(end, length)
        output[start:end] = b"\0" * (end - start)

    return bytes(output)


def shader_key(control, code, zero_ranges):
    return hashlib.sha256(
        normalized_control(control, zero_ranges) + code
    ).hexdigest()[:16]


def shader_stage(code):
    if len(code) < 4:
        return None

    if code[:4] == CS_MAGIC:
        return "cs"

    if code[:4] != GFX_MAGIC or len(code) < 0x34:
        return None

    sph = struct.unpack_from("<I", code, 0x30)[0]
    stage_number = (sph >> 10) & 0xF

    # Ryujinx and shader_split.py interpret SPH stage 0 with VTG type 1
    # as a vertex shader.
    if stage_number == 0 and (sph & 0x1F) == 1:
        return "vs"

    return SPH_STAGES.get(stage_number)


def make_record(control, code):
    record = bytearray(b"BTCDBTCD" + bytes(0x30))

    struct.pack_into(
        "<2I",
        record,
        0x28,
        len(control),
        len(code),
    )

    record += code
    record += control

    if record.count(b"BTCDBTCD") != 1:
        raise ValueError("record separator appears inside shader data")

    return bytes(record)


def scan_model(path, romfs, zero_ranges):
    data = path.read_bytes()
    records = []
    position = 0

    while True:
        position = data.find(CTL_MAGIC, position)

        if position < 0:
            break

        if position + 0x880 > len(data):
            position += 4
            continue

        control = data[position:position + 0x880]
        code_length = struct.unpack_from("<I", control, 0x704)[0]

        if (
            not code_length
            or code_length >= (1 << 24)
            or code_length > position
        ):
            position += 4
            continue

        code_position = position - code_length
        code = data[code_position:position]
        stage = shader_stage(code)

        if stage is None:
            position += 4
            continue

        key = shader_key(control, code, zero_ranges)

        source = (
            f"/{path.relative_to(romfs).as_posix()}"
            f"#MDL@{code_position:#x}"
        )

        records.append(
            {
                "key": key,
                "stage": stage,
                "control": control,
                "code": code,
                "source": source,
            }
        )

        position += 4

    return records


def main():
    parser = argparse.ArgumentParser(
        description="Extract preceding-code NVN shaders from .mdl files"
    )

    parser.add_argument("romfs", type=Path)
    parser.add_argument("package_dir", type=Path)
    parser.add_argument("shader_map", type=Path)

    parser.add_argument(
        "--zero",
        default="",
        help=(
            "comma-separated half-open control ranges to zero before "
            "hashing, for example 0x778:0x780,0x7d0:0x7e0"
        ),
    )

    args = parser.parse_args()

    romfs = args.romfs.resolve()
    package_dir = args.package_dir.resolve()
    shader_map = args.shader_map.resolve()
    zero_ranges = parse_ranges(args.zero)

    if not romfs.is_dir():
        raise SystemExit(f"[RED] RomFS directory not found: {romfs}")

    package_dir.mkdir(parents=True, exist_ok=True)
    shader_map.parent.mkdir(parents=True, exist_ok=True)

    unique = {}
    stats = Counter()
    duplicates = Counter()

    model_files = sorted(romfs.rglob("*.mdl"))

    for number, path in enumerate(model_files, 1):
        try:
            extracted = scan_model(path, romfs, zero_ranges)
        except OSError as error:
            print(f"[RED] cannot read {path}: {error}")
            continue

        stats["model_files"] += 1
        stats["shader_records"] += len(extracted)

        for record in extracted:
            key = record["key"]

            if key in unique:
                duplicates[key] += 1
                continue

            unique[key] = record
            stats[record["stage"]] += 1

        if number % 250 == 0:
            print(
                f"scanned {number}/{len(model_files)} models, "
                f"{len(unique)} unique shaders"
            )

    packages = defaultdict(list)
    map_rows = []

    for key, record in sorted(
        unique.items(),
        key=lambda item: (item[1]["stage"], item[0]),
    ):
        stage = record["stage"]
        package_name = f"{stage}_mdl.bin"
        index = len(packages[package_name])

        packages[package_name].append(
            make_record(record["control"], record["code"])
        )

        map_rows.append(
            (
                package_name,
                index,
                STAGE_NAMES[stage],
                key,
                record["source"],
            )
        )

    for package_name, records in sorted(packages.items()):
        output = package_dir / package_name
        output.write_bytes(b"".join(records))

        print(
            f"{package_name}: {len(records)} records, "
            f"{output.stat().st_size:#x} bytes"
        )

    with shader_map.open("w", encoding="utf-8") as output:
        output.write(
            "# kind: evidence  NVN shaders embedded in MDL model resources; "
            "code immediately precedes the control section\n"
        )

        if zero_ranges:
            ranges = ",".join(
                f"{start:#x}:{end:#x}"
                for start, end in zero_ranges
            )

            output.write(
                f"# control normalization before hashing: {ranges}\n"
            )

        output.write(
            "package\tindex\tstage\tdm_ctl_sha16\tsource\n"
        )

        for row in map_rows:
            output.write("\t".join(map(str, row)) + "\n")

    print()
    print("=== EXTRACTION SUMMARY ===")
    print(f"Model files scanned:    {stats['model_files']}")
    print(f"Shader records found:   {stats['shader_records']}")
    print(f"Unique shader keys:     {len(unique)}")
    print(f"Duplicate records:      {sum(duplicates.values())}")

    for stage in ("vs", "hs", "ds", "gs", "ps", "cs"):
        if stats[stage]:
            print(
                f"{STAGE_NAMES[stage]:22}"
                f"{stats[stage]}"
            )

    print(f"Shader map:             {shader_map}")
    print(f"Package directory:      {package_dir}")


if __name__ == "__main__":
    main()
