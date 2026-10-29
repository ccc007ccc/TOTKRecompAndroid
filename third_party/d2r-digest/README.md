# D2R Mobile portable digest implementation

Unmodified `dm/android/compat/digest.c` and its CommonCrypto compatibility header
from [D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile), commit
`1e0e7934c9ccdf95494aee5065d1d07ab3cc3918`. MIT license: see `LICENSE`.

The native shader preparation path and metadata audit use SHA-256 to compare
game inputs with MacNativeGames' identifiers. This implementation is compiled
into the application and does not require an additional shared library.
