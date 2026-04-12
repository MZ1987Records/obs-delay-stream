# プロジェクト共通ルール

## 応答言語

日本語で応答すること。

## OBS native UI 改修時

OBS native UI（`obs_properties_t` / `obs_property_t`）の改修やクラッシュ調査を行う前に、必ず `.claude/skills/obs-native-ui-review/SKILL.md` を読むこと。

## 用語規約: 遅延 (Latency) と ディレイ (Delay)

- **遅延 / Latency** — 計測された、または想定されているネットワークやバッファリングによるシステムの「遅れ」。例: WS配信遅延、想定アバター遅延、想定環境遅延、OBS配信遅延
- **ディレイ / Delay** — このフィルタープラグインがタイミング調整のために意図的に付加する待機時間。例: チャンネルディレイ、マスターディレイ、OBS出力ディレイ

UI テキスト・コメント・ドキュメントすべてでこの区別を守ること。

## MVVM アーキテクチャ規約

パラメータの追加、ディレイ計算の変更、UI 改修、modified callback の追加・変更を行う際は `.claude/skills/mvvm-architecture/SKILL.md` が自動適用される。背景情報は [`docs/architecture-mvvm.md`](docs/architecture-mvvm.md) を参照。

## コマンド実行時のルール

- Bash でコマンドを実行する前に、移動先が現在のワーキングディレクトリと同じ場合は `cd <dir> &&` を先頭に付けない。別ディレクトリへの移動が必要な場合のみ cd を使う。
