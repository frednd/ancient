# ancient for WebAssembly

Builds the decompressor library as an ES module (`ancient.mjs` + `ancient.wasm`, about 270 KB) for browsers, web workers and Node.

```sh
source /path/to/emsdk/emsdk_env.sh
wasm/build.sh            # output in wasm/dist
node wasm/smoke_test.mjs # checks against testing/test_files
```

## Exported functions
All take pointers into the module heap (`_anc_malloc` / `_anc_free`).

| Function | Purpose |
|---|---|
| `anc_scan(data, size, start, end)` | Scan offsets `start..end` for compressed streams. Returns a JSON string `{"next":n,"hits":[{"offset","packed","raw","name"}]}`. Same two-pass verification as the CLI `scan` command. Call it in chunks to report progress. |
| `anc_try_exact(data, len)` | Decompress and verify `data[0..len)` as one complete stream. Returns the raw size or -1. Used to find the end of formats that keep their header at the end (PowerPacker). |
| `anc_decompress(data, size, exactSizeKnown)` | Decompress one stream; result at `anc_out_ptr()`, name at `anc_name()`. Returns size or -1. |
| `anc_identify(data, size)` | Name, packed and raw size as JSON, or `null`. |

## Limits for scanning inside blobs
- PowerPacker (and its clones) cannot be found by `anc_scan`, because its decoder starts from the end. Locate the magic and probe candidate lengths with `anc_try_exact`.
- GRND has no end marker, so its length inside a blob cannot be verified.
- Weakly identified formats (Freeze, Compact and other Unix formats) can give short false positives inside other compressed data.

Built with `-fwasm-exceptions`, since the library uses exceptions for control flow.
