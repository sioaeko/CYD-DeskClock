"""Generate the small VLW font subsets embedded by the clock firmware."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


LABEL_TEXT = """
0123456789% -·
월 일 요일 일요일 월요일 화요일 수요일 목요일 금요일 토요일
오전 오후
맑음 대체로 맑음 구름 조금 흐림 안개 이슬비 어는 이슬비
약한 비 비 강한 비 어는 비 약한 눈 눈 강한 눈 싸락눈
소나기 강한 소나기 눈 소나기 뇌우 우박 뇌우
불러오는 중 시간 동기화 대기 중
"""


def load_font(path: Path, size: int, weight: int) -> ImageFont.FreeTypeFont:
    font = ImageFont.truetype(str(path), size)
    try:
        axes = font.get_variation_axes()
        if axes:
            values = [axis["default"] for axis in axes]
            values[0] = max(axes[0]["minimum"], min(weight, axes[0]["maximum"]))
            font.set_variation_by_axes(values)
    except (AttributeError, OSError):
        pass
    return font


def glyph_record(font: ImageFont.FreeTypeFont, char: str) -> tuple[bytes, bytes]:
    ascent, descent = font.getmetrics()
    advance = max(0, round(font.getlength(char)))
    pad = 8
    image = Image.new("L", (max(1, advance + pad * 2), ascent + descent + pad * 2), 0)
    baseline = pad + ascent
    ImageDraw.Draw(image).text((pad, baseline), char, font=font, fill=255, anchor="ls")
    bbox = image.getbbox()

    if bbox is None:
        width = height = delta_x = delta_y = 0
        bitmap = b""
    else:
        left, top, right, bottom = bbox
        width, height = right - left, bottom - top
        delta_x = left - pad
        delta_y = baseline - top
        bitmap = image.crop(bbox).tobytes()

    metrics = struct.pack(
        ">7I",
        ord(char),
        height,
        width,
        advance,
        delta_y & 0xFFFFFFFF,
        delta_x & 0xFFFFFFFF,
        0,
    )
    return metrics, bitmap


def write_vlw_header(
    output: Path,
    symbol: str,
    font_path: Path,
    size: int,
    weight: int,
    characters: str,
    family: str,
) -> None:
    font = load_font(font_path, size, weight)
    glyphs = sorted(set(characters) - {"\n", "\r", "\t"}, key=ord)
    ascent, descent = font.getmetrics()
    records = [glyph_record(font, char) for char in glyphs]
    payload = struct.pack(">6I", len(glyphs), 11, size, 0, ascent, descent)
    payload += b"".join(metrics for metrics, _ in records)
    payload += b"".join(bitmap for _, bitmap in records)

    lines = []
    for offset in range(0, len(payload), 20):
        chunk = payload[offset : offset + 20]
        lines.append("  " + ",".join(str(value) for value in chunk) + ",")

    output.write_text(
        f"// Auto-generated {size}px AA font ({family}, OFL). "
        f"{len(glyphs)} glyphs, {len(payload)} bytes.\n"
        "#pragma once\n"
        "#include <pgmspace.h>\n"
        f"static const uint8_t {symbol}[] PROGMEM = {{\n"
        + "\n".join(lines)
        + "\n};\n",
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cascadia", type=Path, default=Path(r"C:\Windows\Fonts\CascadiaMono.ttf"))
    parser.add_argument("--noto-kr", type=Path, default=Path(r"C:\Windows\Fonts\NotoSansKR-VF.ttf"))
    parser.add_argument("--src", type=Path, default=Path("src"))
    args = parser.parse_args()

    write_vlw_header(
        args.src / "timefont.h",
        "timefont_vlw",
        args.cascadia,
        96,
        600,
        "-0123456789",
        "Cascadia Mono SemiBold",
    )
    write_vlw_header(
        args.src / "datafont.h",
        "datafont_vlw",
        args.cascadia,
        30,
        500,
        "-0123456789",
        "Cascadia Mono Medium",
    )
    write_vlw_header(
        args.src / "labelfont.h",
        "labelfont_vlw",
        args.noto_kr,
        17,
        500,
        LABEL_TEXT,
        "Noto Sans KR Medium",
    )


if __name__ == "__main__":
    main()
