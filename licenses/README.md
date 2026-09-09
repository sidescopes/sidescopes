# Bundled component notices

These notices accompany source-embedded icons and the default fonts embedded
by Dear ImGui. They are copied without modification from their upstream projects:

- [Lucide](https://github.com/lucide-icons/lucide/blob/main/LICENSE)
- [ProggyClean](https://github.com/bluescan/proggyfonts/blob/master/LICENSE)
- [ProggyForever](https://github.com/ocornut/proggyforever/blob/master/LICENSE.txt)

The browser's Inter and Roboto Mono notices live beside their subsets in
`src/web/fonts`. CMake collects the notices for each distribution's dependencies.
Desktop applications embed the complete texts for offline reading under
**About → Licenses**. The Windows archive contains only `SideScopes.exe`, and
the Mac archive contains one application bundle. Browser builds copy notices
to `licenses` beside the loader; standalone pages include them in the document
source.

The [YuNet notice](https://github.com/opencv/opencv_zoo/blob/47534e27c9851bb1128ccc0102f1145e27f23f98/models/face_detection_yunet/LICENSE)
is retained with the model used by the optional Linux face-network tests.
macOS and Windows use native detectors and do not bundle YuNet or OpenCV.
When packaging the optional backend, its notices must also include OpenCV,
protobuf, zlib, and the complete SoftFloat and FDLIBM notices from the pinned
source's `modules/core/src/softfloat.cpp`.
