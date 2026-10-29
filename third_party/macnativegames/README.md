# MacNativeGames shader metadata tools

Unmodified MIT sources from [MacNativeGames](https://github.com/ly05010419/MacNativeGames),
commit `f024ef04c707c4b4bb5b6328bfc55ae89642af40`. See `LICENSE` and `UPSTREAM.json`.
`shader_list.tsv.gz` is the upstream TotK identifier/state list compressed with gzip;
it contains no game binaries or compiled shaders. Its recorded checksum is for the
uncompressed upstream file.

`tools/import_shader_warm_list.py` imports the upstream `ShaderList` parser,
canonicalization, pipeline normalization, ordering and seed planning directly.
The adapter runs in the development container. It does not install Python on phones.

Metal pipeline descriptors are evidence of observed states, not Vulkan pipeline
binaries. In particular, upstream `deps_match` deliberately ignores vertex and
fragment types because Metal redeclares them later. A Vulkan adapter must retain
these types and the linked vertex outputs; it must not equate these variants merely
because the upstream matcher serves them with one Metal source.

To update, copy both scripts and the list from the same reviewed commit, preserve
the license, update the provenance/checksums, and rerun the importer tests and the
selected game's raw-key audit. A larger upstream list is not proof of more inputs
in this game's version.
