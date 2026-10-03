# snow-memory

`RasterBuffer` owns initialized CPU raster bytes. Buffers of at least 1 MiB use
whole OS regions (`VirtualAlloc`, Mach VM, or Linux anonymous `mmap`), released
with the original allocation capacity when their owner is dropped. Smaller
buffers use `Vec`. Windows preserves privileged large-page allocation when it
is available; failure uses ordinary OS pages.

Keep the owner throughout a raster pipeline. Slice consumers borrow it directly;
`Arc<RasterBuffer>` supports sharing and mapped copy-on-write via `Arc::make_mut`.
`clear`/`truncate` retain capacity for active pools. Dropping the pool releases it.
`into_vec` is a compatibility boundary and copies mapped storage, so production
raster paths should use buffer-aware entry points. Fallible constructors return
allocation errors instead of silently falling back to the heap.

The optional `serde` feature serializes the same byte sequence as `Vec<u8>`.
Legacy vector interoperability is tested for the repository's bincode and JSON
formats, for both slice and reader input. Deserialization requests the same
sequence representation for every format and fills raster storage in bounded
chunks, avoiding capacity checks for every byte. Misleading sequence size hints
cannot allocate a speculative full image, and malformed input remains fallible.
Decoder growth reserves at most twice the verified decoded length and compacts
disproportionate final capacity, while ordinary raster growth keeps its smaller
headroom policy. This avoids repeated full-prefix copies for large sequences.
Formats that provide a byte slice for the sequence request can use a single
fallible allocation and copy. Deserialization does not request a binary byte
string, which could change tagged wire representations or cause reader backends
to allocate a large temporary heap buffer before validating the payload.

`RasterArray<T>` applies the same ownership policy to fixed-size numeric raster
workspaces such as blur intermediates and pixel-index grids. Its element trait
is sealed to numeric primitives, keeping alignment and bit-validity guarantees
inside the storage implementation.
