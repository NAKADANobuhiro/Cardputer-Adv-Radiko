# Third-Party Notices / 依存ライブラリのライセンス

このリポジトリのソースコードは **MIT License**（[LICENSE](LICENSE)）で提供します。
ただし本プロジェクトはビルド時に以下の第三者ライブラリを利用します（いずれも本リポジトリには
**同梱しておらず**、PlatformIO 等が取得します）。各ライブラリの権利は各著作権者に帰属します。

## 重要（ライセンスの相互作用）

AAC デコードに使う **arduino-libhelix が GPLv3** です。これをリンクして生成した
**ビルド済みバイナリ（`.bin`）は GPLv3 の対象となる結合著作物**です。
バイナリを再配布する場合は GPLv3 に従ってください。
本リポジトリのソースを配布するだけであれば、あなたのコードは MIT のまま扱えます。

## 依存ライブラリ一覧

| ライブラリ | 用途 | ライセンス | 取得元 |
|---|---|---|---|
| arduino-libhelix (pschatzmann) | AAC(HE-AAC)デコード | **GPLv3** | https://github.com/pschatzmann/arduino-libhelix |
| └ 内部の Helix AAC decoder (RealNetworks) | AAC 復号コア | RPSL / RCSL | RealNetworks Public/Community Source License |
| M5Cardputer / M5Unified / M5GFX (M5Stack) | 画面・キーボード・音声(ES8311)・ハード抽象化 | MIT | https://github.com/m5stack |
| Arduino core for ESP32 (Espressif) | Arduino フレームワーク | LGPL-2.1 / Apache-2.0 等 | https://github.com/espressif/arduino-esp32 |
| ESP-IDF 由来コンポーネント (lwIP, mbedTLS, FreeRTOS 等) | ネットワーク/TLS/RTOS | 各コンポーネントのライセンス（BSD 系 / Apache-2.0 等） | https://github.com/espressif/esp-idf |

各ライブラリの正確なライセンス条文は、それぞれの配布元・`LICENSE` ファイルを参照してください。

## 補足

- SBR 無効化のためのビルド前スクリプト `disable_sbr.py` は、取得済みの libhelix の
  `ConfigHelix.h` を**ローカルで**書き換えます（配布物には影響しません）。
- 本ソフトは radiko / NHK / M5Stack の非公式なファンプロジェクトであり、これらの団体とは関係ありません。
