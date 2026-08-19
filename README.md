# CardputerRadiko

M5Stack **Cardputer ADV**（ESP32-S3 / ES8311）を、電源を入れるだけで [radiko](https://radiko.jp/) が聴ける **専用ラジオ受信機**にするファームウェアです。起動すると Wi-Fi 接続 → radiko 認証 → 自動再生。キーボードで選局・音量・Wi-Fi 設定ができます。

> A firmware that turns the M5Stack Cardputer ADV into a standalone [radiko](https://radiko.jp/) internet-radio receiver. (radiko is a Japan-only service.)

- **フレームワーク**: Arduino / PlatformIO（M5Cardputer ライブラリ）
- **方式**: radiko 認証付き HLS(AAC) を取得 → Helix でデコード → ES8311 コーデックへ出力
- **既定エリア**: 神奈川県（JP14）／**既定局**: TBSラジオ（どちらも変更可・後述）
- 実機で**連続再生を確認済み**

---

## 特長

- 電源を入れるだけで自動再生する「ラジオらしい」専用機
- キーボードでの選局・音量（0〜15）・Wi-Fi 設定（本体に保存）
- 圧縮 AAC の先読みバッファ＋PCM リングバッファで安定再生
- 無操作 1 分でバックライト自動消灯（音声は継続）の省電力
- ルーターの DNS が不調でも解決できるフォールバック DNS 内蔵

---

## 必要なもの

- **M5Stack Cardputer ADV**（ESP32-S3 / ES8311 搭載版）＋ USB-C ケーブル
- **PlatformIO**（VS Code 拡張、または CLI）
- radiko が聴取できる**日本国内の Wi-Fi 回線**（radiko はエリアを接続元 IP で判定します）

---

## クイックスタート

```bash
# 1) 取得
git clone <this-repo-url>
cd CardputerRadiko

# 2) （任意）Wi-Fi 初期値を書く … 実機の w キーからでも設定可
#    src/config.h の WIFI_SSID_DEFAULT / WIFI_PASS_DEFAULT

# 3) ビルド & 書き込み（Cardputer を USB 接続）
pio run
pio run -t upload

# 4) 動作ログ
pio device monitor      # 115200 baud
```

> `pio` が見つからない場合は、PlatformIO 同梱の CLI をフルパスで呼びます（例: Windows なら
> `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run`）。詳細は [Runbook.md](Runbook.md)。

書き込み後、`w` キーで Wi-Fi（SSID→パスワード）を設定すると本体に保存され、次回以降は自動接続します。

---

## 操作方法

| キー | 動作 |
|---|---|
| `;`（↑） / `.`（↓） | 局を選ぶ（カーソル移動） |
| `Enter` | 選んだ局を再生 |
| `space` | 停止 |
| `-` / `=` | 音量 down / up（0〜15 の 16 段階、`_` `+` でも可）。上段に `Vol : 06 [-][+]` 表示 |
| `w` | Wi-Fi 設定（SSID→パスワードをキーボード入力、本体に保存） |
| （無操作 1 分） | バックライト自動消灯（省電力・音は継続）。任意キーで復帰（その 1 打は復帰専用） |

画面は 上段=Wi-Fi/エリア/音量、中央=局リスト（`>` が再生中）、下段=状態（認証中/バッファ中/再生中/エラー）。

---

## エリア（地域）の変更

radiko は**接続元の IP アドレスでエリアを自動判定**します。つまり **その地域のネット回線から接続していること**が前提で、設定を変えても別エリアの無料放送は聴けません（エリアフリーは radiko の有料機能で、本ソフトは非対応）。

「地域の設定を変える」とは実質、**その地域で聴ける局をプリセットに入れ替える**ことです。手順:

1. **自分のエリアコードを調べる**。`JP` + 都道府県コード（2 桁）。例: 東京=`JP13`、神奈川=`JP14`、大阪=`JP27`、愛知=`JP23`、北海道=`JP01`、福岡=`JP40`。

2. **そのエリアの局一覧を取得**。ブラウザで次を開く（`JP13` を自分のコードに置換）:

   ```
   https://radiko.jp/v3/station/list/JP13.xml
   ```

   各 `<station>` の `<id>`（局ID）と `<name>`（局名）を控えます。

3. **`src/stations.h` を編集**。`STATIONS[]` を、その局ID・表示名に書き換えます。**先頭の局が起動時の初期選局**になります。

   ```cpp
   static const Station STATIONS[] = {
     { "TBS",     "TBSラジオ" },   // ← 先頭が初期選局
     { "QRR",     "文化放送" },
     { "JOAK",    "NHKラジオ第1" },
     // ... その地域の局IDを追加 ...
   };
   ```

4. **`src/config.h` の `RADIKO_AREA_ID`** を自分のエリア（例 `"JP13"`）に変更（これは主に表示用。実エリアは自動判定）。

5. ビルド＆書き込み。

> 局を選んで下段に「**配信対象外**」と出る場合、その局はそのエリアの無料配信対象外（HTTP 403）です。`stations.h` から外してください（例: 神奈川では放送大学・NHKラジオ第2 が対象外）。
>
> NHK も radiko で配信されています（例: `JOAK`=NHKラジオ第1、`JOAK-FM`=NHK FM）。

---

## 設定（`src/config.h`）

| 定義 | 意味 | 既定 |
|---|---|---|
| `WIFI_SSID_DEFAULT` / `WIFI_PASS_DEFAULT` | Wi-Fi 初期値（空なら実機で設定） | 空 |
| `RADIKO_AREA_ID` | 表示用エリア | `"JP14"` |
| `DEFAULT_STATION_INDEX` | 起動時に自動再生する局（`stations.h` の並び順） | `0` |
| `VOLUME_DEFAULT` / `VOLUME_MAX` | 音量（0〜15） | `6` / `15` |
| `SCREEN_OFF_MS` | 無操作で消灯するまでの時間(ms) | `60000` |
| `DISPLAY_BRIGHTNESS` | 通常時の画面の明るさ(0-255) | `80` |
| `FIFO_BYTES` / `PREBUF_BYTES`（`audio_player.cpp`） | 先読みバッファ量（途切れ調整） | 28KB / 14KB |

---

## 音質について

本機は **PSRAM を搭載しない Cardputer ADV** の限られた RAM で動かすため、HE-AAC の **SBR（高域再生）を無効化**して AAC-LC ベースのみを復号しています（`disable_sbr.py` がビルド前に自動設定）。**高域は控えめ**ですが、トーク番組など実用上は十分な音質で、**安定した連続再生**を優先しています。技術的背景は [development.md](development.md) を参照してください。

---

## 注意 / 利用規約

個人が**自分の居住エリアの放送を聴く**範囲での利用に留めてください。再配信・録音の公開や、エリア外の受信（地域偽装）は radiko の規約に反する可能性があります。本実装はエリア判定をサーバ任せにしており、居住エリアの受信を前提としています。

radiko の API は年に一度ほど変更されることがあります。動かなくなった場合は `src/radiko.cpp` の認証・URL を最新仕様へ更新してください。

---

## ドキュメント

- [Runbook.md](Runbook.md) — ビルド／書き込みの詳しい手順・トラブルシューティング
- [development.md](development.md) — 技術詳細・設計・ハマりどころ（メモリ制約、リングバッファ、TLS など）

---

## ライセンス

本リポジトリのソースコードは **MIT License**（[LICENSE](LICENSE)）です。

ただし AAC デコードにビルド時リンクする [pschatzmann/arduino-libhelix](https://github.com/pschatzmann/arduino-libhelix) は **GPLv3** です（本リポジトリには同梱していません）。そのため、**ビルドして生成したバイナリ（`.bin`）は GPLv3 の対象となる結合著作物**になります。バイナリを再配布する場合は GPLv3 に従ってください。ソースコードの配布であれば MIT のまま扱えます。

依存ライブラリの一覧とライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。

---

## 謝辞 / 出典

- [streamlink radiko プラグイン](https://github.com/streamlink/streamlink/blob/master/src/streamlink/plugins/radiko.py)（現行 API の参考）
- [radiko API 解説 - sho10case](https://sho10case.com/2025/12/22/post-795/)
- [Cardputer-Adv ドキュメント - m5-docs](https://docs.m5stack.com/en/core/Cardputer-Adv)
- [pschatzmann/arduino-libhelix](https://github.com/pschatzmann/arduino-libhelix)（AAC デコーダ）
- [inodx/radiko-esp32](https://github.com/inodx/radiko-esp32) / [jitenshap/radiko-esp32](https://github.com/jitenshap/radiko-esp32)（ESP32 先行事例）

---

※ 本ソフトは radiko および M5Stack の非公式なファンプロジェクトです。radiko / NHK / M5Stack とは関係ありません。
