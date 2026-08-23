# Snow Shot Third-Party Notices

Snow Shot incorporates third-party software and assets under their respective
licenses. The release build generates a complete, versioned notice bundle from
the resolved Rust dependency graph, installed vcpkg packages, the audited
static Qt build, and repository-owned attribution files.

Installed releases place that bundle under:

```text
share/snow-shot/licenses/third-party/
```

`INDEX.md` in that directory records every collected package and source
license file. The bundle includes the Ant Design Icons MIT notice from
`ant_design_qt/THIRD_PARTY_NOTICES.md`.

The generated bundle is authoritative for a particular binary because its
contents are produced from that build environment. Dependency licenses and
copyright notices remain the property of their respective owners.
