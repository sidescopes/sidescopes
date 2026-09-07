# Face detection model

`face_detection_yunet.onnx` is the unmodified
`face_detection_yunet_2023mar.onnx` model from
[OpenCV Zoo](https://github.com/opencv/opencv_zoo/tree/47534e27c9851bb1128ccc0102f1145e27f23f98/models/face_detection_yunet).
Its SHA-256 is
`8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4`.

The Windows build verifies these bytes and embeds them in the executable.
Detection runs locally; the application does not download models or send
captured images elsewhere. The MIT license is in
[licenses/YuNet.txt](../../licenses/YuNet.txt).
