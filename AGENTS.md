# AGENTS.md

## Makefile

- 多用するコマンドは Makefile を採用する

## ディレクトリ構成

### scripts/

- Makefile から呼び出すスクリプトや、ユーザーがリポジトリ内部で何度も行う処理目的のスクリプトを配置。
- アプリケーション内部に用いるスクリプトは配置しない。

### docs/
- docs/ ドキュメント
- docs/feature/ 機能ドキュメント。新規機能を追加するときは、featureを採番してから実装する。
- docs/ADR/ ADRドキュメント。採番する。
- docs/issue/ バグトラッカー。GitHub Issuesは使わず、`docs/issues/` に採番して修正を進める。
- docs/reference/ 参考資料。Webからfetchした技術情報などを、要点とリンクを保持して保存する。再探索時のトークン節約に利用

### src/
- src/ ソースコードは src/ の下で管理すること。
