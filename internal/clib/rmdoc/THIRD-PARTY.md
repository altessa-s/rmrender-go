# Third-party and generated inputs

* `vendor/miniz/miniz.c`, `miniz.h`, `LICENSE.txt`: unmodified copies of the
  miniz 3.1.2 amalgamation. Miniz is MIT licensed; its original notices are
  retained. The private wrapper disables ZIP APIs, stdio, time and zlib
  aliases. All external codec functions are prefixed `rmd_private_` and hidden
  from the shared-library API, avoiding collisions with other miniz users even
  when linking static libraries.
* `src/rmd_unicode_data.h`: generated Unicode 16.0.0 canonical decomposition,
  combining-class and format-category tables. See `UNICODE-LICENSE.txt` and
  `scripts/generate-unicode.py`. The UTF-8/NFD implementation is shared with
  the MIT-licensed [rmmarks-lib](https://github.com/altessa-s/rmmarks-lib) and
  compiled here with no package dependency.
* `tests/fixtures.h` and `tests/fuzz-seeds`: generated original test payloads,
  ZIP headers and raw deflate. `tests/generate-fixtures.py` documents the recipe.
  They contain no reMarkable device files or copyrighted document samples.
