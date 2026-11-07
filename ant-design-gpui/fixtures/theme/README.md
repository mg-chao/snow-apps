# Theme reference fixtures

`reference.json` contains independently computed seed, map and alias values from
Ant Design 6.6.7 at `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, using
`@ant-design/colors` 8.0.1 and `@ant-design/fast-color` 3.0.1. Its provenance lists
SHA-256 hashes of the original TypeScript and JavaScript inputs.

The fixture generator executes those original sources. It does not import the
Rust implementation. Ordinary Cargo builds and tests read the checked-in data
and require neither Node.js nor an Ant Design checkout.

The complete native inventory contains 47 seed values, 313 map values and 406
alias values when typed palettes are expanded. Twenty cases cover all built-in
algorithm combinations, every seed field, every map and alias override, palette
overrides, transparency, motion disabling, decimal rounding and radius thresholds.

Native normalization converts colors to RGBA, durations to seconds, font stacks
to ordered names, easing curves to four coordinates, and shadows to structured
logical pixel dimensions and colors. Typed palettes replace duplicate legacy
`blue-1` and current `blue1` spellings; comparisons use the current spelling.
This deliberately avoids modeling two independently mutable copies of one tone.

To regenerate using the repository author's existing pnpm cache:

```powershell
node scripts/generate_theme_fixtures.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10
```

Alternatively, install the exact versions of `@ant-design/colors`,
`@ant-design/fast-color` and TypeScript 5.9.3 into a temporary build directory,
then pass `--node-modules <that-directory>/node_modules` instead of `--pnpm-store`.
The generator rejects a different upstream revision or dependency version.

The original MIT license texts are preserved in `licenses/`.
