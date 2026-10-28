# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Compare full, unaligned reference/native frames. Pillow is developer-only."""
import argparse
import hashlib
import json
import re
from pathlib import Path
from PIL import Image, ImageChops, ImageStat

p = argparse.ArgumentParser()
p.add_argument("--browser", type=Path, default=Path("../build/ant-design-gpui/modal-reference"))
p.add_argument("--native", type=Path, default=Path("../build/ant-design-gpui/modal-native"))
p.add_argument("--output", type=Path, default=Path("fixtures/modal/visual-validation-windows.json"))
a = p.parse_args()
digest = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
results = []
for scale in [100, 150, 200]:
    browser = json.loads((a.browser / f"live-{scale}/browser-metrics.json").read_text())
    for case in browser["cases"]:
        name = case["scenario"]["name"]
        bpath = a.browser / f"live-{scale}/{name}.png"
        npath = a.native / f"{scale}/{name}.png"
        expected = Image.open(bpath).convert("RGB")
        actual = Image.open(npath).convert("RGB")
        assert expected.size == actual.size, (name, scale, expected.size, actual.size)
        geometry = json.loads(npath.with_suffix(".json").read_text())
        nodes = case["metrics"]["elements"]
        rect = next(e for e in nodes if e["selector"] == ".ant-modal")
        deltas = {k:geometry[k]-rect[k] for k in ["x", "y", "w", "h"]}
        assert geometry["scale"] == scale / 100
        probes = [("mask", 4, 4), ("surface", rect["x"]+rect["w"]/2, rect["y"]+4)]
        for index, button in enumerate(e for e in nodes if e["selector"] == ".ant-btn"):
            if button["y"] + button["h"] < actual.height / (scale / 100):
                probes.append((f"button-{index}", button["x"]+4, button["y"]+button["h"]/2))
        for index, row in enumerate(e for e in nodes if e["selector"] == ".ant-skeleton-paragraph > li"):
            probes.append((f"skeleton-{index}", row["x"]+4, row["y"]+row["h"]/2))
        icon_geometry = []
        icon_solid_core = []
        for icon in (e for e in nodes if e["selector"] == ".ant-modal-confirm-body > .anticon"):
            probes.append(("status-icon", icon["x"]+icon["w"]*.25, icon["y"]+icon["h"]*.4))
            color = tuple(int(v) for v in re.findall(r"[0-9]+", icon["color"])[:3])
            scale_factor = scale / 100
            box = tuple(round(v * scale_factor) for v in (icon["x"]-2, icon["y"]-2, icon["x"]+icon["w"]+2, icon["y"]+icon["h"]+2))
            background = next(e for e in nodes if e["selector"] == ".ant-modal-container")["background"]
            background = tuple(int(v) for v in re.findall(r"[0-9]+", background)[:3])
            vector = tuple(c-b for c,b in zip(color,background))
            norm = sum(v*v for v in vector)
            def covered(pixel):
                # Estimate SVG coverage from its foreground/background colors. Exact-color
                # bounds measure the opaque core, whose edge depends on antialiasing.
                coverage = sum((p-b)*v for p,b,v in zip(pixel,background,vector)) / norm
                residual = max(abs(p-b-coverage*v) for p,b,v in zip(pixel,background,vector))
                return coverage >= .5 and residual <= 4
            bounds, core_bounds = [], []
            for frame in [expected, actual]:
                crop = frame.crop(box)
                pixels = crop.load()
                for target, predicate in [(bounds,covered),(core_bounds,lambda pixel:max(abs(v-w) for v,w in zip(pixel,color))<=2)]:
                    points = [(x,y) for y in range(crop.height) for x in range(crop.width) if predicate(pixels[x,y])]
                    assert points, (name, scale, "missing status icon color")
                    target.append([min(x for x,y in points),min(y for x,y in points),max(x for x,y in points),max(y for x,y in points)])
            icon_geometry.append(max(abs(v-w) for v,w in zip(*bounds))/scale_factor)
            icon_solid_core.append(max(abs(v-w) for v,w in zip(*core_bounds))/scale_factor)
        solid = []
        for label,x,y in probes:
            pixel = (round(x*scale/100), round(y*scale/100))
            b = expected.getpixel(pixel)
            n = actual.getpixel(pixel)
            solid.append({"slot":label, "browser_rgb":b, "native_rgb":n, "max_channel_delta":max(abs(v-w) for v,w in zip(b,n))})
        diff = ImageChops.difference(expected, actual)
        results.append({"name":name, "scale":scale/100, "geometry_delta":deltas,
            "mean_absolute_channel_error":sum(ImageStat.Stat(diff).mean)/3,
            "solid_probes":solid,"icon_geometry_error_logical_px":max(icon_geometry,default=0),"icon_solid_core_error_logical_px":max(icon_solid_core,default=0),"browser_sha256":digest(bpath),"native_sha256":digest(npath)})
sources = [Path("examples/modal.rs"),Path("fixtures/modal/visual-cases.json"),Path(__file__),Path("src/behavior/text.rs"),Path("src/behavior/paragraph.rs"),Path("src/icons/mod.rs")] + sorted(Path("src/modal").glob("*.rs"))
report = {"comparisons":len(results), "browser":browser["renderer"],
    "upstream":json.loads(Path("fixtures/modal/reference.json").read_text(encoding="utf-8"))["provenance"]["revision"],
    "max_geometry_error_logical_px":max(abs(v) for case in results for v in case["geometry_delta"].values()),
    "max_icon_geometry_error_logical_px":max(c["icon_geometry_error_logical_px"] for c in results),
    "max_icon_solid_core_error_logical_px":max(c["icon_solid_core_error_logical_px"] for c in results),
    "max_solid_channel_delta":max(probe["max_channel_delta"] for case in results for probe in case["solid_probes"]),
    "mean_full_frame_channel_error":sum(c["mean_absolute_channel_error"] for c in results)/len(results),
    "all_image_dimensions_match":True,
    "measurement":"Full unaligned, unmasked canvases; no interpolation or font masking. Geometry is native minus browser. Solid probes avoid text, antialiased edges and shadows. Skeleton is frozen at frame zero. Icon geometry uses >=50% color coverage; exact-color core bounds are also reported separately to expose antialiasing differences.",
    "source_sha256":{str(f).replace('\\','/'):digest(f) for f in sources},
    "native_executable_sha256":digest(Path("../build/ant-design-gpui/debug/examples/modal.exe")),
    "cases":results}
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
print({k:v for k,v in report.items() if k not in ["cases","source_sha256"]})
assert report["max_geometry_error_logical_px"] <= 1, "Inspect layout discrepancies in the report"
assert report["max_solid_channel_delta"] <= 1, "Inspect solid color discrepancies in the report"

assert report["max_icon_geometry_error_logical_px"] <= 1, "Inspect icon geometry discrepancies in the report"
