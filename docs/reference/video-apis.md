# 録画APIの要点

- [H.264 encoder](https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder): 入力はNV12などのYUV形式。初期実装ではGDIのBGRAをNV12へ変換する。
- [Sink Writerのチュートリアル](https://learn.microsoft.com/en-us/windows/win32/medfound/tutorial--using-the-sink-writer-to-encode-video): 出力・入力のメディア型、サンプル時刻・継続時間、`Finalize`が必要。
- [MFCreateSinkWriterFromURL](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-mfcreatesinkwriterfromurl): 既存のByte StreamとMP4コンテナ属性を渡せる。
- [MF_TRANSCODE_CONTAINERTYPE](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-transcode-containertype): MP4は`MFTranscodeContainerType_MPEG4`を指定する。
- [MF_MT_DEFAULT_STRIDE](https://learn.microsoft.com/en-us/windows/win32/medfound/mf-mt-default-stride-attribute): top-down画像は正のstride。NV12入力は幅をstrideにする。
