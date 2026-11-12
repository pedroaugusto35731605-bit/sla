#!/usr/bin/env python3
"""
Gera uma imagem XDVDFS/GDF mínima e válida para testar o XOne-Emu
(parser de ISO, extração de capa e importação na biblioteca).

Não contém nenhum conteúdo protegido: apenas um MicrosoftGame.config
de exemplo, uma capa PNG gerada aqui e um executável homebrew vazio.

uso: python3 tools/make_test_iso.py saida.iso [--title "Nome"] [--title-id 1A2B3C4D]
"""
from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"


def make_png(width: int, height: int, top: tuple[int, int, int], bottom: tuple[int, int, int]) -> bytes:
    """PNG RGB com gradiente vertical (sem dependências externas)."""
    rows = bytearray()
    for y in range(height):
        t = y / max(1, height - 1)
        r, g, b = (int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        rows += b"\x00" + bytes((r, g, b)) * width

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")


def dir_table(entries: list[tuple[str, int, int, bool]]) -> bytes:
    """Tabela de diretório XDVDFS: cada entrada aponta para a próxima pela subárvore direita."""
    out = bytearray()
    offsets = []
    blobs = []
    for name, sector, size, is_dir in entries:
        nb = name.encode("ascii")
        blob = bytearray(struct.pack("<HHIIBB", 0, 0, sector, size, 0x10 if is_dir else 0x20, len(nb)) + nb)
        while len(blob) % 4:
            blob += b"\xff"
        blobs.append(blob)
    pos = 0
    for blob in blobs:
        offsets.append(pos)
        pos += len(blob)
    for i, blob in enumerate(blobs):
        if i + 1 < len(blobs):
            struct.pack_into("<H", blob, 2, offsets[i + 1] // 4)  # right = próxima entrada
        out += blob
    while len(out) % SECTOR:
        out += b"\xff"
    return bytes(out)


def build(out: Path, title: str, title_id: str, publisher: str) -> None:
    config = f"""<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1" xmlns:xone="urn:xone-emu">
  <Identity Name="XOneEmu.{title.replace(' ', '')}" Publisher="CN=Homebrew" Version="1.0.2.0"/>
  <TitleId>{title_id}</TitleId>
  <ShellVisuals DefaultDisplayName="{title}" PublisherDisplayName="{publisher}"
                StoreLogo="Assets\\StoreLogo.png" Square150x150Logo="Assets\\Logo150.png"/>
  <xone:Performance xone:TargetResolution="1920x1080" xone:TargetFps="60"/>
</Game>
""".encode()
    logo = make_png(300, 400, (16, 124, 16), (8, 32, 8))
    exe = b"\xF4" * 64  # homebrew: só HLT

    files = {"MicrosoftGame.config": config, "Assets/StoreLogo.png": logo, "game.xbin": exe}

    # Alocação de setores: 32 = volume descriptor, 33 = raiz, 34 = Assets, 35+ = dados
    next_sector = 35
    placed: dict[str, tuple[int, int]] = {}
    for path, data in files.items():
        placed[path] = (next_sector, len(data))
        next_sector += (len(data) + SECTOR - 1) // SECTOR

    assets = dir_table([("StoreLogo.png", *placed["Assets/StoreLogo.png"], False)])
    root = dir_table([
        ("Assets", 34, len(assets), True),
        ("MicrosoftGame.config", *placed["MicrosoftGame.config"], False),
        ("game.xbin", *placed["game.xbin"], False),
    ])

    img = bytearray(next_sector * SECTOR)
    vd = bytearray(SECTOR)
    vd[0:20] = MAGIC
    struct.pack_into("<IIQ", vd, 20, 33, len(root), 0)
    vd[0x7EC:0x7EC + 20] = MAGIC
    img[32 * SECTOR:33 * SECTOR] = vd
    img[33 * SECTOR:33 * SECTOR + len(root)] = root
    img[34 * SECTOR:34 * SECTOR + len(assets)] = assets
    for path, data in files.items():
        sector, _ = placed[path]
        img[sector * SECTOR:sector * SECTOR + len(data)] = data

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(img)
    print(f"imagem XDVDFS gerada: {out} ({len(img)} bytes, title id {title_id})")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output", type=Path)
    ap.add_argument("--title", default="XOne Homebrew Demo")
    ap.add_argument("--title-id", default="58E0A001")
    ap.add_argument("--publisher", default="XOne-Emu Project")
    a = ap.parse_args()
    build(a.output, a.title, a.title_id.upper(), a.publisher)
