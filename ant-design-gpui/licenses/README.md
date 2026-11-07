# Dependency license fallbacks

`cargo/<name>-<version>/` preserves upstream notices omitted from published crate
archives. Each `SOURCE.md` records the verified package revision, the actual
notice revision, and its SHA-256 digest.

`Apache-2.0.txt` and `WTFPL.txt` are byte-for-byte copies of the repository's
canonical texts in `../licenses/`. They support the existing license collector's
recognized fallback rules. They do not replace package-specific copyright
notices.
