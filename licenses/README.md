# Embedded artwork and font notices

These notices accompany source-embedded icons and the default fonts embedded
by Dear ImGui. They are copied without modification from their upstream projects:

- [Lucide](https://github.com/lucide-icons/lucide/blob/main/LICENSE)
- [ProggyClean](https://github.com/bluescan/proggyfonts/blob/master/LICENSE)
- [ProggyForever](https://github.com/ocornut/proggyforever/blob/master/LICENSE.txt)

The browser's Inter and Roboto Mono notices live beside their subsets in
`src/web/fonts`. CMake collects these and the fetched dependency notices into
each distribution. macOS bundles keep them in `Contents/Resources/Licenses`;
Windows and browser builds keep them in `licenses` beside the executable or
loader. Standalone browser pages include the same text in the document source.
