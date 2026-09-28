"""Compare two runs of the Release settings-page benchmark on the same machine."""

import argparse
import json
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    args = parser.parse_args()
    before = json.loads(args.before.read_text(encoding="utf-8"))
    after = json.loads(args.after.read_text(encoding="utf-8"))
    failures = []

    def check(condition, message):
        if not condition:
            failures.append(message)

    for key in ("platform", "qt_version", "samples"):
        check(before[key] == after[key], f"Runs differ in {key}")
    check(after["samples"] >= 15, "Use at least 15 measured samples")
    original = {page["page"]: page for page in before["pages"]}
    optimized = {page["page"]: page for page in after["pages"]}
    check(original.keys() == optimized.keys(), "Runs must cover the same pages")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1

    print("Page | First display ms (before -> after) | All sections ms (before -> after)")
    for name, old in original.items():
        new = optimized[name]
        first_old, first_new = (page["first_display_ms"]["median"] for page in (old, new))
        all_old, all_new = (page["all_sections_ms"]["median"] for page in (old, new))
        print(f"{name} | {first_old:.2f} -> {first_new:.2f} | {all_old:.2f} -> {all_new:.2f}")
        # On pages below 40 ms, OS scheduling and timers dominate small differences.
        if first_old >= 40:
            check(first_new <= first_old * 0.75, f"{name}: first display did not improve by 25%")
        check(new["total_widgets"] >= old["total_widgets"], f"{name}: incomplete control coverage")
        check(new["max_popup_open_ms"]["median"] <= 50,
              f"{name}: first color popup opening exceeds 50 ms")

    def total(pages, key):
        return sum(page[key]["median"] for page in pages.values())

    check(total(optimized, "first_display_ms") <= total(original, "first_display_ms") * 0.75,
          "Aggregate first display did not improve by 25%")
    check(total(optimized, "all_sections_ms") <= total(original, "all_sections_ms") * 1.25,
          "Deferred work increased aggregate complete traversal by more than 25%")
    check(total(optimized, "all_interactions_ms") <= total(original, "all_interactions_ms") * 1.25,
          "Deferred work increased aggregate traversal with popup opening by more than 25%")
    check(total(optimized, "retranslation_ms") <= total(original, "retranslation_ms") * 0.8,
          "Aggregate retranslation did not improve by 20%")
    check(after["refreshes_per_25_notifications"] == 1, "Notification burst was not coalesced")
    check(after["notification_burst_ms"]["median"] <= before["notification_burst_ms"]["median"] * 0.2,
          "Notification burst did not improve by 80%")
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    print("PASS" if not failures else "FAIL")
    return int(bool(failures))


if __name__ == "__main__":
    sys.exit(main())
