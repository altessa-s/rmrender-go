The pages in this directory are original work of this project, written for
its tests with the writer of github.com/altessa-s/rmlines-go
(`rmlines.Write`), not taken from a tablet. They are covered by this
module's LICENSE.

`Image_placed.rm` is a page of a 1404 by 1872 screen and canvas, with the
medium typed-text column and no typed text, written under the author key
01 02 03 ... 10 (the bytes 1 to 16). It holds:

- one black fineliner stroke (`ToolFineliner2`, `ColorBlack`, thickness 2)
  through (200, 200), (600, 400) and (900, 300), each point with the
  writer's defaults (`rmlines.DefaultPoint`: width 12, pressure 6);
- over it, the image `picture.png`, attachment key a0 01 02 ... 0f, placed
  at (300, 600), 400 units wide and 300 high, unanchored.

The image's pixels are not in the page: a lone page resolves them through
`Options.Images`, by that name.

The pages are written by `internal/coexist/authored`, a command of the
`internal/coexist` module, which depends on rmlines-go where the main module
does not. From the repository root:

    go -C internal/coexist run ./authored ../../testdata/authored

The writer answers the same bytes for the same draft, and the command's test
checks that it reproduces the committed files byte for byte, so change a page
by changing the command, not the file.
