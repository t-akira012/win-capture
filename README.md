# Win Capture

- Windows 11用のネイティブな常駐キャプチャツール。現在は静止画機能の初期版です。

## 操作

| キー | 動作 |
| --- | --- |
| Alt+Shift+3 | カーソルがあるモニターを保存 |
| Alt+Shift+4 | 範囲をドラッグして保存。Escで中止 |
| Alt+Shift+5 | 操作メニューを開く |

トレイアイコンの右クリックまたはダブルクリックでもメニューを開けます。録画項目は未実装のため無効です。

## 配布と設定

ReleaseのZIPを展開して`capture.exe`を起動します。初回起動時にEXEと同じフォルダーへ`capture.ini`が生成されます。以降はINIを編集してからアプリを再起動してください。保存先は初期設定で`%USERPROFILE%\capture`です。EXEのあるフォルダーに書き込み権限が必要です。

`[Image] format`は`png`、`jpeg`、`jpg`を指定できます。`[General] filename`には`{yyyy}`、`{MM}`（月）、`{dd}`、`{HH}`、`{mm}`（分）、`{ss}`を使えます。必須項目が欠落・不正な場合はエラーを表示します。`[Video]`の設定は将来の録画機能向けで、現在は値を検証します。

## ビルド

Windows 11のMSVC環境で`cmake -S . -B build -A x64`、`cmake --build build --config Release`を実行します。GitHub Actionsも同じ構成でZIPを生成します。設計と実装状況は[docs/design.md](docs/design.md)を参照してください。
