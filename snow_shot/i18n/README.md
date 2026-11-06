# Snow Shot translations

Edit the Qt Linguist `.ts` catalogs in the feature directories. Each feature has
`en_US`, `zh_CN`, and `zh_TW` catalogs. English is the source language and its
translations are identity strings, except for the native language name and
language-specific plural forms.

Use sentence case for all English UI text: capitalize the first word of each
sentence and lowercase other ordinary words. Preserve acronyms, proper names,
official system setting names, keyboard key names, and unit symbols (for example,
`API`, `Snow Shot`, `macOS`, `System Settings`, `Ctrl`, `px`, and `mm`).

User-visible messages do not end with a sentence-ending period (`.` or `。`). Keep
punctuation between sentences and ellipses that indicate an ongoing operation.

## UI terminology

Use the Simplified Chinese feature names as the semantic reference. Check the
action and its implementation before changing a translation; for example, system
audio capture does not enable or disable a speaker device. Use these names across
menus, settings, tooltips, accessibility text, and status messages:

| 简体中文 | English | 繁體中文 |
| --- | --- | --- |
| 截图 | Screenshot | 截圖 |
| 滚动截图 | Scrolling screenshot | 捲動截圖 |
| 固定到屏幕 | Pin to screen | 固定到螢幕 |
| 固定到屏幕管理 | Pin to screen management | 固定到螢幕管理 |
| 图片编辑 | Image editing | 圖片編輯 |
| 图片拼接 | Image stitching | 圖片拼接 |
| 图片分割 | Image splitting | 圖片分割 |
| 文本识别 | Text recognition | 文字辨識 |
| 原图翻译 | Original image translation | 原圖翻譯 |
| 屏幕录制 | Screen recording | 螢幕錄製 |
| 系统音频 | System audio | 系統音訊 |
| 全局快捷键 | Global shortcuts | 全域快速鍵 |
| 悬浮工具栏 | Floating toolbar | 浮動工具列 |
| 序号 | Sequence number | 序號 |
| 画笔 | Brush | 畫筆 |
| 保存为文件 | Save as file | 儲存為檔案 |
| 历史记录 | History | 歷史記錄 |
| 选区 | Selection | 選取範圍 |
| 字体大小 | Font size | 字型大小 |
| 渲染 | Rendering | 算繪 |

Use `Stitch images` for the stitching action and `Image stitching` for the feature
or window title. Keep official operating-system setting names and keyboard key
names as published by the platform. Stable IDs, configuration keys, and protocol
values are independent of UI wording and must not be renamed with these labels.

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
