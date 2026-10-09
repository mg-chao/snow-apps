# MicroTeX source and font notices

These notices are copied unchanged from NanoMichael/MicroTeX revision
`0e3707f6dafebb121d98b53c64364d16fefe481d`:
https://github.com/NanoMichael/MicroTeX/tree/0e3707f6dafebb121d98b53c64364d16fefe481d.

The immutable codeload ZIP used by `cmake/MicroTeX.cmake` has SHA-512:

```text
4b6e410fd80488b59a190a9e17658dfb26e02c5c0694a64a981146eef38c4756b2f2d5196b926d8ba1b5e8460cf5082ba99dfee7169716b6117edd15b9e68189
```

| Collected file | Original source | License |
| --- | --- | --- |
| LICENSE-MIT.txt | LICENSE | MIT, Copyright (c) 2020 Nano Michael |
| OFL.txt | res/fonts/licences/OFL.txt | SIL Open Font License 1.1; American Mathematical Society |
| Knuth_License.txt | res/fonts/licences/Knuth_License.txt | Original Knuth permission notice |
| License_for_dsrom.txt | res/fonts/licences/License_for_dsrom.txt | Original dsrom permission notice |
| GPL-3.0-greek.txt | res/greek/LICENSE | GNU GPL version 3 |
| GPL-3.0-cyrillic.txt | res/cyrillic/LICENSE | GNU GPL version 3 |
| RES_README | res/RES_README | Original resource inventory |

The OFL notice names the reserved fonts `eufb10`, `eufm10`, `msam10`, and `msbm10`.
Snow Shot embeds the original font bytes without renaming or modifying fonts.
The Greek and Cyrillic packages retain their upstream XML and font notices.

Snow Shot applies a maintained integration patch in `cmake/microtex/patch_source.py`
to its private build copy: Qt resource loading, formula session isolation, parser
budgets, safe render dimensions, and logical-pixel Qt font sizing. The downloaded
upstream source is preserved, and the library source remains under its MIT notice.
