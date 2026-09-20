# Recording font fixtures

These fonts contain original geometric test glyphs, created for Snow Apps and
licensed under Apache-2.0. They contain no outlines or other data from external
fonts. They are only loaded by tests and are never installed system-wide.

Sans and Mono cover ASCII and a left arrow. Han covers the Chinese recording
test labels, the Qt fallback probe, and one supplementary-plane character. Each
family has regular (400) and bold (700) faces with different outlines. The Latin
families deliberately lack Han glyphs so a fallback test cannot succeed by
rendering its base font alone.

The checked-in TTFs make tests independent of operating-system language packs.
To regenerate them, install `fonttools==4.65.0` in a temporary environment and run
`python test-support/fonts/generate.py`. FontTools is only a regeneration tool;
building and running the tests does not require it.
