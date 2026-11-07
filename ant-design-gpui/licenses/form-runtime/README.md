# Form runtime dependencies

The native Form implementation uses these upstream packages without modifying
their source. Cargo.lock records the exact registry archives and checksums.

| Package | Version | License | Source |
| --- | --- | --- | --- |
| imbl | 7.0.2 | MPL-2.0-or-later | https://github.com/jneem/imbl |
| imbl-sized-chunks | 0.2.0 | MPL-2.0-or-later | https://github.com/jneem/imbl-sized-chunks |
| archery | 1.2.3 | MIT | https://github.com/orium/archery |
| rand_xoshiro | 0.7.0 | MIT OR Apache-2.0 | https://github.com/rust-random/rngs |
| safe_arch | 0.7.4 | Zlib OR Apache-2.0 OR MIT | https://github.com/Lokathor/safe_arch |
| wide | 0.7.33 | Zlib OR Apache-2.0 OR MIT | https://github.com/Lokathor/wide |

`imbl` provides immutable shared collections. Its MPL-covered source is available
from the linked repository and the versioned crates.io source distribution;
original library code in this repository remains Apache-2.0.

The repository license collector supplies the complete resolved runtime notices,
including the existing regex and URL validation dependency trees. Its generated
bundle and validation report belong under the ignored build directory.
