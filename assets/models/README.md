# Face detection model

`face_detection_yunet.onnx` is the unmodified
`face_detection_yunet_2023mar.onnx` model from
[OpenCV Zoo](https://github.com/opencv/opencv_zoo/tree/47534e27c9851bb1128ccc0102f1145e27f23f98/models/face_detection_yunet).
Its SHA-256 is
`8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4`.

The optional Linux face-network test backend verifies these bytes and embeds
them in its test binary. Detection runs locally. The macOS and Windows
applications use native detectors and do not bundle this model. The
[MIT license](../../licenses/YuNet.txt) accompanies the model.
