# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Pack independently rendered browser metrics and content-addressed provenance."""
import hashlib
import json
from pathlib import Path
import argparse
p = argparse.ArgumentParser()
p.add_argument("--reference", type=Path, default=Path("../build/ant-design-gpui/modal-reference"))
p.add_argument("--output", type=Path, default=Path("fixtures/modal/reference.json"))
a = p.parse_args()
source = json.loads((a.reference / "browser/provenance.json").read_text())
metrics = json.loads((a.reference / "live-150/browser-metrics.json").read_text())
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
inputs = {k.replace("E:/ant-design/", "upstream/"): v for k,v in source["inputs"].items() if "ant-design/components/" in k}
provenance = {"revision":source["revision"],"version":source["version"],"renderer":metrics["renderer"],"lock_sha256":source["lock_sha256"],"upstream_inputs":inputs,
    "packages":{k:source["packages"]["node_modules/"+k]["version"] for k in ["react","react-dom","@rc-component/dialog","@rc-component/util","@ant-design/cssinjs"]},
    "scripts":{str(f):sha(f) for f in [Path("scripts/modal_reference.tsx"),Path("scripts/prepare_modal_reference.mjs"),Path("scripts/capture_modal_reference.mjs")]},
    "note":"Original React Modal and useModal with upstream reset.css; mask blur disabled for the agreed GPUI fallback. No native values used in reference generation."}
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(json.dumps({"provenance":provenance,"cases":metrics["cases"]},ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
print(a.output)
