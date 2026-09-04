#!/usr/bin/env python3
"""Generate the small, deterministic MOBI7 corpus used by parser tests."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def be16(value: int) -> bytes:
    return struct.pack(">H", value)


def be32(value: int) -> bytes:
    return struct.pack(">I", value)


def set16(buffer: bytearray, offset: int, value: int) -> None:
    buffer[offset : offset + 2] = be16(value)


def set32(buffer: bytearray, offset: int, value: int) -> None:
    buffer[offset : offset + 4] = be32(value)


def palmdoc_compress(source: bytes) -> bytes:
    output = bytearray()
    cursor = 0
    while cursor < len(source):
        value = source[cursor]
        if value == 0 or 9 <= value <= 0x7F:
            if value == 0x20 and cursor + 1 < len(source) and 0x40 <= source[cursor + 1] <= 0x7F:
                output.append(source[cursor + 1] ^ 0x80)
                cursor += 2
            else:
                output.append(value)
                cursor += 1
            continue
        start = cursor
        while cursor < len(source) and cursor - start < 8:
            value = source[cursor]
            if value == 0 or 9 <= value <= 0x7F:
                break
            cursor += 1
        output.append(cursor - start)
        output.extend(source[start:cursor])
    return bytes(output)


def exth_record(tag: int, value: bytes) -> bytes:
    return be32(tag) + be32(len(value) + 8) + value


def mobi_record0(
    *,
    compression: int,
    text_length: int,
    text_record_count: int,
    encoding: int,
    version: int,
    title: str,
    author: str,
    language: str,
    encrypted: bool = False,
    extra_flags: int = 0,
) -> bytes:
    codec = "utf-8" if encoding == 65001 else "cp1252"
    title_bytes = title.encode(codec)
    exth_records = [
        exth_record(99, title_bytes),
        exth_record(100, author.encode(codec)),
        exth_record(524, language.encode(codec)),
    ]
    exth = b"EXTH" + be32(12 + sum(map(len, exth_records))) + be32(len(exth_records))
    exth += b"".join(exth_records)

    record0 = bytearray(244)
    set16(record0, 0, compression)
    set32(record0, 4, text_length)
    set16(record0, 8, text_record_count)
    set16(record0, 10, 4096)
    set16(record0, 12, 2 if encrypted else 0)
    record0[16:20] = b"MOBI"
    set32(record0, 20, 228)
    set32(record0, 24, 2)
    set32(record0, 28, encoding)
    set32(record0, 32, 0x0F0E0D0C)
    set32(record0, 36, version)
    for offset in range(40, 80, 4):
        set32(record0, offset, 0xFFFFFFFF)
    set32(record0, 80, text_record_count + 1)
    full_name_offset = 244 + len(exth)
    set32(record0, 84, full_name_offset)
    set32(record0, 88, len(title_bytes))
    set32(record0, 108, 0xFFFFFFFF)
    set32(record0, 112, 0xFFFFFFFF)
    set32(record0, 120, 0xFFFFFFFF)
    set32(record0, 128, 0x40)
    set32(record0, 164, 0xFFFFFFFF)
    set32(record0, 168, 0xFFFFFFFF)
    set32(record0, 224, 0xFFFFFFFF)
    set16(record0, 244 - 2, extra_flags)
    return bytes(record0) + exth + title_bytes


def palm_database(records: list[bytes], name: str = "LoreForge Fixture") -> bytes:
    header = bytearray(78)
    name_bytes = name.encode("ascii")[:31]
    header[: len(name_bytes)] = name_bytes
    header[60:64] = b"BOOK"
    header[64:68] = b"MOBI"
    set16(header, 76, len(records))

    table = bytearray()
    offset = 78 + len(records) * 8 + 2
    for index, record in enumerate(records):
        table += be32(offset) + bytes([0]) + (index + 1).to_bytes(3, "big")
        offset += len(record)
    return bytes(header) + bytes(table) + b"\x00\x00" + b"".join(records)


def make_book(
    html: str,
    *,
    encoding: int,
    compression: int,
    version: int,
    title: str,
    author: str,
    language: str,
    encrypted: bool = False,
    split: int | None = None,
    trailers: bool = False,
) -> bytes:
    codec = "utf-8" if encoding == 65001 else "cp1252"
    text = html.encode(codec)
    split = split if split is not None else len(text)
    raw_records = [text[:split], text[split:]] if split < len(text) else [text]
    records = [palmdoc_compress(record) if compression == 2 else record for record in raw_records]
    if trailers:
        records = [record + b"\x00" for record in records]
    record0 = mobi_record0(
        compression=compression,
        text_length=len(text),
        text_record_count=len(records),
        encoding=encoding,
        version=version,
        title=title,
        author=author,
        language=language,
        encrypted=encrypted,
        extra_flags=1 if trailers else 0,
    )
    return palm_database([record0, *records])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    utf8_html = (
        "<html><body><h1>Arrival</h1><p>“Hello,” said Café.</p>"
        "<h1>Return</h1><hr/><p>Home — at last.</p></body></html>"
    )
    utf8_split = utf8_html.encode("utf-8").index("é".encode("utf-8")) + 1
    args.output.joinpath("utf8_uncompressed.mobi").write_bytes(
        make_book(
            utf8_html,
            encoding=65001,
            compression=1,
            version=6,
            title="Fixture Chronicle",
            author="Ada Example",
            language="en",
            split=utf8_split,
        )
    )

    cp1252_html = "<html><body><p>Chapter One</p><p>“Café” — déjà vu.</p></body></html>"
    args.output.joinpath("cp1252_palmdoc.mobi").write_bytes(
        make_book(
            cp1252_html,
            encoding=1252,
            compression=2,
            version=6,
            title="PalmDOC Café",
            author="Renée Writer",
            language="fr",
            split=37,
        )
    )

    backref_html = "<html><body><p>repeat repeat</p></body></html>"
    backref_text = backref_html.encode("ascii")
    repeated = backref_text.index(b"repeat repeat")
    second_repeat = repeated + len(b"repeat ")
    pair = 0x8000 + (len(b"repeat ") << 3) + (len(b"repeat") - 3)
    backref_record = backref_text[:second_repeat] + be16(pair) + backref_text[second_repeat + 6 :]
    backref_record0 = mobi_record0(
        compression=2,
        text_length=len(backref_text),
        text_record_count=1,
        encoding=65001,
        version=6,
        title="Back-reference Fixture",
        author="Fixture Author",
        language="en",
    )
    args.output.joinpath("palmdoc_backref.mobi").write_bytes(
        palm_database([backref_record0, backref_record])
    )

    trailer_html = "<html><body><p>Trailer removed.</p></body></html>"
    args.output.joinpath("record_trailer.mobi").write_bytes(
        make_book(
            trailer_html,
            encoding=65001,
            compression=1,
            version=6,
            title="Trailer Fixture",
            author="Fixture Author",
            language="en",
            trailers=True,
        )
    )

    corrupt_record0 = mobi_record0(
        compression=2,
        text_length=1,
        text_record_count=1,
        encoding=65001,
        version=6,
        title="Corrupt Fixture",
        author="Fixture Author",
        language="en",
    )
    args.output.joinpath("corrupt_backref.mobi").write_bytes(
        palm_database([corrupt_record0, b"\x80\x00"])
    )

    common = dict(
        html="<html><body><p>Visible failure fixture.</p></body></html>",
        encoding=65001,
        title="Unsupported Fixture",
        author="Fixture Author",
        language="en",
    )
    args.output.joinpath("encrypted.mobi").write_bytes(
        make_book(compression=1, version=6, encrypted=True, **common)
    )
    args.output.joinpath("huff_cdic.mobi").write_bytes(
        make_book(compression=17480, version=6, **common)
    )
    args.output.joinpath("kf8_only.mobi").write_bytes(
        make_book(compression=1, version=8, **common)
    )


if __name__ == "__main__":
    main()
