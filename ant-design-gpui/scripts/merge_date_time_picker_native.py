# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
"""Merge attested disjoint native capture shards without discarding provenance."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shard", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        raise ValueError("Merge destination must be empty")
    cases, names, images, provenance = [], set(), [], []
    identity = None
    for shard in args.shard:
        attest_path = shard / "native-provenance.json"
        attest = read(attest_path)
        metrics_path = shard / "native-metrics.json"
        if attest["metrics_sha256"] != digest(metrics_path):
            raise ValueError(f"Unattested metrics: {shard}")
        current = {key: attest[key] for key in ("source_sha256", "executable", "toolchain", "profile", "source_root", "git_head")}
        if identity is None:
            identity = current
        if identity != current:
            raise ValueError(f"Shards used different source/binaries/toolchains: {shard}")
        context = attest["capture_context"]
        chunk = read(metrics_path)["cases"]
        if context["cases"] != len(chunk) or context["foreground_violations"] != 0 or context["foreground_checks"] < len(chunk)*6:
            raise ValueError(f"Incomplete foreground attestation: {shard}")
        manifest = {item["file"]: item["sha256"] for item in read(shard / "image-manifest.json")}
        for case in chunk:
            name = case["name"]
            if name in names:
                raise ValueError(f"Duplicate native case: {name}")
            names.add(name)
            filename = f"{name}.png"
            if manifest.get(filename) != digest(shard / filename):
                raise ValueError(f"Unattested image: {shard / filename}")
            cases.append(case)
            images.append({"file": filename, "sha256": manifest[filename], "source": shard / filename})
        provenance.append({"path": str(shard.resolve()), "attestation_sha256": digest(attest_path), "attestation": attest})
    args.output.mkdir(parents=True, exist_ok=True)
    for item in images:
        shutil.copy2(item["source"], args.output / item["file"])
    metrics = args.output / "native-metrics.json"
    metrics.write_text(json.dumps({"schema_version": 1, "cases": sorted(cases, key=lambda case: case["name"])}) + "\n", encoding="utf-8")
    (args.output / "image-manifest.json").write_text(json.dumps([{key: item[key] for key in ("file", "sha256")} for item in images]) + "\n", encoding="utf-8")
    context = {"cases": len(cases), "foreground_violations": 0,
               "foreground_checks": sum(p["attestation"]["capture_context"]["foreground_checks"] for p in provenance),
               "process_ids": [p["attestation"]["capture_context"]["process_id"] for p in provenance]}
    (args.output / "capture-context.json").write_text(json.dumps(context, indent=2) + "\n", encoding="utf-8")
    (args.output / "native-provenance.json").write_text(json.dumps({
        "schema_version": 1, **identity, "purpose": "Native GPU visual capture, merged disjoint attested shards",
        "completed_utc": max(p["attestation"]["completed_utc"] for p in provenance),
        "capture_context": context, "metrics_sha256": digest(metrics), "shards": provenance,
        "merge_script_sha256": digest(Path(__file__)),
    }, indent=2) + "\n", encoding="utf-8")
    print(f"Merged {len(cases)} attested native captures from {len(provenance)} disjoint shards")


if __name__ == "__main__":
    main()
