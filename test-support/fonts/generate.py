"""Regenerate the original recording-test fonts (requires fonttools==4.65.0).

These deliberately simple geometric glyphs are test data, not UI assets.
Run from any directory; the checked-in TTFs require no generator at test time.
"""

from pathlib import Path

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen


def generate(family, style, characters, mono=False):
    bold = style == "Bold"
    glyphs = {".notdef": TTGlyphPen(None).glyph()}
    metrics = {".notdef": (600, 0)}
    cmap = {}
    for character in sorted(set(characters)):
        code = ord(character)
        name = f"uni{code:04X}"
        cmap[code] = name
        width = 600 if mono else (900 if character == "W" or code > 127 else 600)
        pen = TTGlyphPen(None)
        if not character.isspace():
            # Two original geometric strokes, with visibly different bold outlines.
            stroke = 170 if bold else 80
            for left, bottom, right, top in [
                (60, 0, 60 + stroke, 700),
                (60, 700 - stroke, width - 60, 700),
            ]:
                pen.moveTo((left, bottom))
                pen.lineTo((right, bottom))
                pen.lineTo((right, top))
                pen.lineTo((left, top))
                pen.closePath()
        glyphs[name] = pen.glyph()
        metrics[name] = (width, 60)
    builder = FontBuilder(1000, isTTF=True)
    builder.setupGlyphOrder(list(glyphs))
    builder.setupCharacterMap(cmap)
    builder.setupGlyf(glyphs)
    builder.setupHorizontalMetrics(metrics)
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    builder.setupNameTable({
        "familyName": family,
        "styleName": style,
        "uniqueFontIdentifier": f"Snow recording test: {family} {style} 1.0",
        "fullName": f"{family} {style}",
        "psName": f"{family.replace(' ', '')}-{style}",
        "version": "Version 1.0",
        "copyright": "Original Snow Apps test data; SPDX-License-Identifier: Apache-2.0",
    })
    builder.setupOS2(sTypoAscender=800, sTypoDescender=-200,
                     usWinAscent=800, usWinDescent=200,
                     usWeightClass=700 if bold else 400,
                     fsSelection=0x20 if bold else 0x40)
    builder.setupPost()
    builder.setupMaxp()
    builder.setupHead(created=2082844800, modified=2082844800, macStyle=1 if bold else 0)
    builder.font.recalcTimestamp = False
    builder.save(Path(__file__).with_name(f"{family.replace(' ', '')}-{style}.ttf"))


if __name__ == "__main__":
    latin = "".join(chr(code) for code in range(32, 127)) + "←"
    han = "汉鼠标左键鼠標右鍵空格退中汉字按钮测试\U00020000"
    for style in ["Regular", "Bold"]:
        generate("Snow Recording Test Sans", style, latin)
        generate("Snow Recording Test Mono", style, latin, mono=True)
        generate("Snow Recording Test Han", style, han)
