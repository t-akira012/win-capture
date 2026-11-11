# Win Capture 設計

状態: 初期実装中（2026-11-11）。この文書を設計の正本とし、変更履歴はGitで追う。

## 目的と制約

Windows 11でmacOSに近いスクリーンショット・画面収録操作を提供する小型の常駐アプリ。C++20とWindows標準APIを使い、Web UI、外部ランタイム、インストーラー、管理サービスは使わない。ZIPから実行し、アプリ設定はEXEと同じ場所の必須`capture.ini`のみで管理する。日常操作をキーボードで完結できるようにする。

言語の第一候補はC++20。Win32とWindowsの公式資料・サンプルに直接対応するためで、C++/WinRTはWindows Runtime APIを呼ぶために使用する。Rustと`windows` crateはMac miniから`cargo-xwin`でクロスビルドする代案。C# Native AOTも候補だが、今回は採用しない。言語間の常駐メモリ差を、計測前から決定的な理由にはしない。

## 操作仕様

| 操作 | 動作 | 状態 |
| --- | --- | --- |
| Alt+Shift+3 | 操作時にカーソルがあるモニターの静止画を即時保存 | 確定 |
| Alt+Shift+4 | マウスで矩形選択して静止画保存。Escで中止 | 確定 |
| Alt+Shift+5 | 撮影・録画モードの小型パネルを表示 | 確定 |
| 静止画保存後 | 対象モニターの右下に画像のミニ表示を約5秒表示。クリックで保存ファイルを既定アプリで開く | 確定 |
| 録画中にトレイアイコンをダブルクリック | 録画停止、MP4を確定して保存 | 確定 |
| 録画中に終了 | MP4を確定して保存してから終了 | 確定 |

「全画面」は仮想デスクトップ全体ではなく、開始時にカーソルがあるモニター1台を指す。録画音声は不要。録画中はトレイアイコンを変更する。範囲選択UIは完了・キャンセル時に消す。パネルの詳細なキーボード操作は実装時に定義する。

ミニ表示は保存成功後だけ表示し、連続撮影時は最新の画像に置き換える。次の撮影前に隠して映り込みを防ぐ。保存失敗は成功表示を出さず、理由を通知する。EXEとトレイには専用アイコンを使用し、録画実装時には録画中の状態別アイコンを用意する。

## 設定と保存

INIがない初回起動時だけ生成する。以後はINIを正本とし、欠落・不正な値を黙って補完しない。`%USERPROFILE%`などの環境変数を展開し、保存先を作成する。画像はPNG/JPEG、動画はH.264/MP4。JPEG品質は1～100のINI値をWICの0.0～1.0へ変換する。

初期INI（数値は初期実装が採用した値であり、ユーザー指定値ではない）:

```ini
[General]
save_dir=%USERPROFILE%\capture
filename=capture-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}

[Image]
format=png
jpeg_quality=90

[Video]
fps=30
bitrate=8000000
```

月は`{MM}`、分は`{mm}`で区別する。同じ秒に同名ファイルがあれば拡張子の前に`-1`、`-2`と連番を付け、上書きしない。拡張子は出力形式から決める。初期値とトークン記法は初期実装で採用済みであり、今後の変更はこの文書と実装を同時に更新する。

## 内部構成

- 常駐コア: `WinMain`、メッセージループ、`RegisterHotKey`、`WM_HOTKEY`、`Shell_NotifyIconW`、INI、ファイル命名。
- 静止画: 初期実装はGDIで取得し、WICでPNG/JPEG保存。
- 動画: `Windows.Graphics.Capture`、Direct3D 11、Media FoundationでH.264/MP4。BGRAからエンコーダー対応形式への変換、時刻管理、終了時の確定が必要。
- 撮影・録画の処理は要求時だけ動かす。

## Windows固有の条件

Per-Monitor DPI Aware V2を使用し、負の座標を含む複数モニターを扱う。ホットキー競合は検出して通知する。`TaskbarCreated`時にトレイアイコンを再登録する。多重起動は名前付きMutexで防ぐ。UIの映り込みを防ぎ、録画操作UIにはキャプチャ除外属性を検討する。HDRの色変換・トーンマッピングは検証対象。Alt+Shiftが入力言語切替と競合する環境もある。

録画UIのキャプチャ除外には`SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`が候補。`Windows.Graphics.Capture`の録画対象枠を非表示にするにはWindows側の許可要件があるため、初期実装で無条件に非表示を約束しない。

## 開発・配布

Mac miniでソース管理し、GitHub ActionsのWindows x64でMSVCとWindows SDKを使ってビルドする。`/MT`でC++ランタイムを静的リンクし、ZIPをReleaseから配布する。Windows 11 / Intel N100でDPI、UI、画質、動画性能を確認する。

## 実装状況

`v0.1.0-alpha.2`までに常駐コア、静止画、保存後のミニ表示、専用アイコンを実装し、Windows x64でビルド・ZIP化・Release公開済み。録画、録画中アイコン変更、録画終了時の確定は後続の実装対象。Windows実機での動作確認は未了。

## 参考

- [Microsoft: RegisterHotKey](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerhotkey)
- [Microsoft: WIC bitmap encoder](https://learn.microsoft.com/en-us/windows/desktop/wic/-wic-imp-iwicbitmapencoder)
- [Microsoft: Media Foundation sink writer](https://learn.microsoft.com/en-us/windows/win32/medfound/using-the-sink-writer)
- [Microsoft: Windows Graphics Capture](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture)
- [Microsoft: SetWindowDisplayAffinity](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity)
- [Win32CaptureSample](https://github.com/robmikh/Win32CaptureSample)
