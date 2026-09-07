# Bundled component notices

These notices accompany source-embedded icons and the default fonts embedded
by Dear ImGui. They are copied without modification from their upstream projects:

- [Lucide](https://github.com/lucide-icons/lucide/blob/main/LICENSE)
- [ProggyClean](https://github.com/bluescan/proggyfonts/blob/master/LICENSE)
- [ProggyForever](https://github.com/ocornut/proggyforever/blob/master/LICENSE.txt)
- [YuNet](https://github.com/opencv/opencv_zoo/blob/47534e27c9851bb1128ccc0102f1145e27f23f98/models/face_detection_yunet/LICENSE)

The browser's Inter and Roboto Mono notices live beside their subsets in
`src/web/fonts`. CMake collects these and the fetched dependency notices into
each distribution. macOS bundles keep them in `Contents/Resources/Licenses`;
Windows and browser builds keep them in `licenses` beside the executable or
loader. Standalone browser pages include the same text in the document source.

Windows distributions also carry the OpenCV, protobuf and zlib notices from
the pinned dependency source used for the build. The complete SoftFloat and
FDLIBM notices are preserved from that source's `modules/core/src/softfloat.cpp`
in `OpenCV-SoftFloat-FDLIBM.txt`. The YuNet notice accompanies the model
embedded in the executable.
