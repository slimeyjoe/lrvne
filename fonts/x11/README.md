# X11 bitmap fonts

The bitmap fonts the font builder (`tools/lrvne/xfont.py`) turns into glyph
tables: `jiskan16` for the full-width JIS X 0208 glyphs of a kanji ROM,
`8x16rk` for the half-width ones (ASCII and katakana), and `8x16` (the same
Sony design in ISO 8859-1), extended by `../latin8-pl.txt`, for the Latin text
of translations. They are the BDF sources, unmodified, from the X.Org
releases:

| File           | From                          | Licence                                                    |
|----------------|-------------------------------|------------------------------------------------------------|
| `8x16rk.bdf`   | `font-sony-misc-1.0.4.tar.xz` | Sony, notice kept: `COPYING.sony-misc`                     |
| `8x16.bdf`     | `font-sony-misc-1.0.4.tar.xz` | Sony, notice kept: `COPYING.sony-misc`                     |
| `jiskan16.bdf` | `font-jis-misc-1.0.4.tar.xz`  | JIS X 9051-1984, by permission to use: `COPYING.jis-misc`  |

Neither is public domain. The Sony fonts may be used, modified and distributed
freely provided the copyright and permission notice appear in all copies and
in the supporting documentation (the notice is also in each BDF header).
`jiskan16` carries only the statement in its COPYING file and its `COPYRIGHT`
property; X.Org and the Linux distributions ship it as free.

From https://www.x.org/releases/individual/font/ (signed release tarballs).
SHA-256:

    e6b09f823fccb06e0bd0b2062283b6514153323bd8a7486e9c2e3f55ab84946b  font-sony-misc-1.0.4.tar.xz
    78d1eff6c471f7aa6802a26d62cccf51d8e5185586406d9b6e1ee691b0bffad0  font-jis-misc-1.0.4.tar.xz
    497f1cc5511023b636e6798886a7e43991fa8977a16c01d556e5f18f4ac2fd84  8x16.bdf
    b935021e6f03dce8ccd83ff50efd1723ca2885275c46c02534e8448942afb6e1  8x16rk.bdf
    e9ee1273ce9d6675e8f4a24e49e0d89ec49f003f464d22828af48915dd8f6dbf  jiskan16.bdf

Glyph tables made from these files are byte-identical to the ones made from
the compiled `.pcf.gz` fonts that distributions ship.
