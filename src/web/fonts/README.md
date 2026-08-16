# The lab's fonts

A page has no system fonts to enumerate, so the web build carries two of
its own. The desktop builds do not use these: there `interfaceFontFiles()`
returns real paths and the application loads SF Pro or Segoe UI.

Both are subset to **exactly** the range `loadInterfaceFont` asks for —
Latin-1 plus U+0394, the delta the colour picker labels its differences
with. That is what takes them from 325 KB and 79 KB to 17 KB and 20 KB.

| File | Face | Licence |
| --- | --- | --- |
| `ui.ttf` | Inter Regular | SIL Open Font License 1.1 |
| `mono.ttf` | Roboto Mono Regular | SIL Open Font License 1.1 |

Inter is chosen because it is a screen UI face and reads close to both SF
Pro and Segoe UI, so the lab looks like the application on either
desktop rather than like neither.

Regenerate with:

    uv run --with fonttools python -c "
    from fontTools import subset
    subset.main(['Inter-Regular.ttf', '--output-file=ui.ttf',
                 '--unicodes=U+0020-00FF,U+0394', '--layout-features=',
                 '--no-hinting', '--desubroutinize'])"
