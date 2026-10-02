This overlay retains vcpkg x265 4.1 and its existing patches.

`short-propagation-row.patch` skips the SIMD loop when a CU-tree row
contains fewer elements than a vector. The original do/while-style loop
processed a full vector and then processed the remainder again, writing
past `Lookahead::m_scratch` for very narrow recordings. Both SSE2 and AVX
variants now enter their existing scalar remainder paths directly.
Normal-sized rows keep the same instructions and quality settings.
The recording-export HDR trim fixtures exercise 32-pixel-wide clips.
