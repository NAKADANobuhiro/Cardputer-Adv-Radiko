# CLAUDE.md — CardputerRadiko（作業メモ / 将来のセッション向け）

## これは何
M5Stack **Cardputer ADV**（ESP32-S3 / ES8311 コーデック）を **radiko 受信機**にする
Arduino/PlatformIO ファーム。地域=神奈川(JP14)、初期局=TBS。
`Personal\Radiko\Radiko受信機_実現可能性検討書.md`（先行の検討書）の実装フェーズにあたる。

## 構成の要点
- **フレームワーク**: Arduino + PlatformIO、board=`m5stack-stamps3`、lib=`M5Cardputer`＋`arduino-libhelix`。
- **音声パイプライン**: 自前HLS取得（WiFiClientSecure+HTTPClient, 認証ヘッダ付与）→ Helix AACデコード
  → PCMを0.25秒ぶん組み立て → `M5Cardputer.Speaker.playRaw()`（バックプレッシャで実時間ペーシング）。
  ※ ESP-ADF や ESP32-audioI2S を使わないのは、radiko の authtoken ヘッダを全リクエストに
    自由に付けたいため（既存ライブラリは任意ヘッダ付与が難しい）。
- **再生は別FreeRTOSタスク**（core0）。UIは`loop()`（core1）。共有状態は `s_gen`（世代番号）で
  局切替/停止を検知。
- **UI**: `M5Cardputer.update()` がキーボードも更新（`Keyboard.update()` は存在しない）。
  KeysState の `.enter/.space/.del/.word`。矢印は `;`(↑)`.`(↓)`,`(←)`/`(→)`。
- **日本語表示**: `setFont(&fonts::lgfxJapanGothic_12)` 必須（局名が日本語）。

## radiko 現行仕様（2026）
- auth1/auth2 = `radiko.jp/v2/api/auth1|auth2`、固定鍵 `bcd151073c03b352e1ef2fd66c32209da9ca0afa`。
- ライブ = `alliance-stream-radiko.smartstream.ne.jp/so/playlist.m3u8?station_id=..&l=15&lsid=<md5>&type=b`。
- **年1回ほどAPI変更**あり（直近 2026-01-16）。動かなくなったら `src/radiko.cpp` を更新。

## 既知の未検証・リスク（README「実機で確認」に詳細）
1. PSRAM 非搭載の可能性（StampS3系）。`psramFound()` で分岐、無ければ内部RAMに約48KB確保。
2. ES8311 が鳴るかは M5Unified のバージョン依存（新しめ必須）。
3. HE-AAC(SBR)。`-DAAC_ENABLE_SBR` 付与済み。
4. セグメントが TS 容器だった場合の demux は未実装（ADTS前提）。
5. TLS 再接続コストで途切れ得る → メディアhostのTLSクライアントは使い回し済み。

## ビルド確認済み
PlatformIO で `pio run` 成功（Flash 38.4% / RAM 15.6%、M5Cardputer 1.1.1, libhelix 0.9.4）。
実機書き込み・音出しは未確認。

## 次の一手候補
- 実機での音出し確認と `ASM_SAMPLES`/`dma_buf_count` の実測チューニング。
- 局リストを起動時に `v3/station/list/JP14.xml` から動的取得。
- OTA更新（radiko API変更に備える）。バッテリ運用・スリープ・時計表示。
