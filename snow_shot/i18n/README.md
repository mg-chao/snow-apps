# Snow Shot translations

Edit the Qt Linguist `.ts` catalogs in the feature directories. Each feature has
`en_US`, `zh_CN`, and `zh_TW` catalogs. English is the source language and its
translations are identity strings, except for the native language name and
language-specific plural forms.

`modules.json` assigns each Qt translation context to exactly one feature. Keep
the same context and source text when moving messages between files: catalog
paths are not part of Qt's translation lookup key. New contexts must be assigned
explicitly; an unassigned context fails extraction before any source catalog is
written. Empty feature catalogs are retained so every feature has all languages.

## Updating translations

After changing translatable C++ strings, use the existing target:

```powershell
cmake --build --preset build-windows-msvc-debug --target snow_shot_update_translations
```

The target merges the current feature catalogs into temporary files, runs Qt
`lupdate` against the complete application source targets, then distributes the
result back to the feature catalogs. Qt retains responsibility for extraction,
translation reuse, plural handling, and obsolete-message removal. If extraction
reports an unassigned context, add it to `modules.json` and rerun the target.

Fill all unfinished translations in all three languages, preserving `%1`, `%n`,
and other Qt placeholders. Qt Linguist can open each feature catalog directly.
Then validate and compile:

```powershell
python snow_shot/scripts/translation_catalogs.py check
cmake --build --preset build-windows-msvc-debug --target snow_shot_release_translations
ctest --preset test-windows-msvc-debug -R '^snow-shot-(translation-catalog|language-manager)-tests$'
```

Build `snow-shot-language-manager-tests` before running its CTest entry if it is
not already built. The catalog tooling tests require only the Python standard
library and can also run directly:

```powershell
python snow_shot/tests/translation_catalog_tests.py
```

Catalogs use stable context/message ordering, LF line endings, and no source
locations. Moving code or changing line numbers therefore does not modify
translations. This means Qt Linguist's source-location navigation is unavailable;
search for the context or source string in the repository instead. Unchanged
catalogs are not rewritten, including their modification times.

## Build and runtime

`cmake/SnowShotTranslations.cmake` merges feature catalogs into
`build/<preset>/snow_shot/i18n/merged/`. One shared `lrelease` rule compiles them,
merges Qt's own translations, and retains `-fail-on-unfinished`. Applications and
tests embed the existing `:/i18n/snow_shot_<locale>.qm` resources, so language
discovery and switching require no special handling for feature catalogs.

Only the application-wide update target extracts strings. Test executables
consume the complete catalogs and do not have separate update targets that could
remove messages outside their partial source lists. The global Qt
`update_translations` target also runs the complete merge/extract/split workflow.

Generated aggregate `.ts` and `.qm` files belong in the build directory and must
not be committed. Do not run `lupdate` against all feature catalogs directly:
that would copy the whole application's messages into every feature file.

To reassign an existing context, first run `translation_catalogs.py merge` with
`--output-dir` pointing to a temporary build directory, change its assignment in
`modules.json`, then run `translation_catalogs.py split` with `--input-dir`
pointing to that directory. Review all languages together. The script checks for
missing files, incorrect ownership, duplicate keys, and language coverage; the
`check` command also verifies completeness, placeholders, and canonical format.

Ant Design Qt maintains its smaller catalogs independently under
`ant_design_qt/packages/ant_design_qt/i18n/`.
