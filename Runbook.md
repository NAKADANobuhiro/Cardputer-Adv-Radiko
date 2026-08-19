# Runbook — CardputerRadiko のビルドと書き込み

## 必要なもの

- M5Stack **Cardputer ADV** 本体 + USB-C ケーブル
- PC（Windows / macOS / Linux）
- **PlatformIO**（VS Code 拡張、または CLI `pip install platformio`）

## 手順

### 1. プロジェクトを開く
`CardputerRadiko/` フォルダを VS Code (PlatformIO) で開くか、CLI ならこのフォルダで作業。

### 2.（任意）Wi-Fi 初期値を書く
`src/config.h`:
```c
#define WIFI_SSID_DEFAULT  "あなたのSSID"
#define WIFI_PASS_DEFAULT  "あなたのパスワード"
```
※ 書かなくても、書き込み後に本体で `w` キーから設定できます。

### 3. ビルド
```bash
pio run
```
初回は ESP32 ツールチェーンと M5Cardputer / libhelix を自動ダウンロードします（数分）。
成功すると `Flash: ~38% / RAM: ~16%` 程度の表示。

### 4. 書き込み
Cardputer を USB 接続して電源ON、
```bash
pio run -t upload
```
ポートが自動検出されない場合は `platformio.ini` に `upload_port = COMx`（Windows）や
`/dev/ttyACM0`（Linux）を追記。

### 5. 動作確認（シリアル）
```bash
pio device monitor
```
- `[boot] CardputerRadiko / PSRAM=yes|no` … 起動
- `[wifi] IP=...` … Wi-Fi 接続OK
- `[radiko] auth OK area=JP14` … 認証成功
- 画面が「再生中 : TBSラジオ」になり、スピーカーから音が出れば成功

## うまくいかないとき

| 症状 | 見るところ |
|---|---|
| コンパイルで M5 のAPIエラー | M5Cardputer / M5Unified を最新へ（`pio pkg update`） |
| 画面は出るが無音 | ES8311 対応の M5Unified か確認。`ASM_SAMPLES`/DMA バッファ調整（README「実機で確認」参照） |
| `auth failed` | radiko の API 変更の可能性。`src/radiko.cpp` を最新仕様へ更新 |
| `stream 403/401` 連発 | authtoken 期限切れ→自動再認証するが、繰り返す場合はエリア/局IDを確認 |
| 音が途切れる | Wi-Fi 電波、`NET_READ_CHUNK`、`dma_buf_count`、`ASM_SAMPLES` を調整 |
| 局が鳴らない | `stations.h` の局IDを `https://radiko.jp/v3/station/list/JP14.xml` で確認 |

## 局を増やす / 変える
`src/stations.h` の `STATIONS[]` に `{ "局ID", "表示名" }` を追加。
局IDは `https://radiko.jp/v3/station/list/JP14.xml`（神奈川）の `<id>` を参照。
