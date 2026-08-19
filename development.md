# development.md — 技術詳細・設計ノート

CardputerRadiko の内部構造と、実装上のハマりどころ・設計判断をまとめます。PSRAM 非搭載の
Cardputer ADV（内蔵 RAM のみ）で radiko(HTTPS/HLS/AAC) を安定再生させるための工夫が中心です。

---

## 全体アーキテクチャ

2 つの FreeRTOS タスクで、ネットワークと音声デコードを分離しています。

```
[ネット取得タスク: core0]                     [デコード再生タスク: core1]
 認証(auth1/auth2)                             FIFOから圧縮AACを取り出す
   → マスターplaylist取得                        → Helix(libhelix)でAACデコード
   → medialist(チャンクリスト)取得               → PCMをリングバッファに組み立て
   → .aacセグメントをHTTPSでDL                    → M5Cardputer.Speaker.playRaw()
   → 圧縮したまま FIFO へ push  ────(StreamBuffer)──▶  （isPlayingでペーシング）
```

- **ネット取得**と**デコード**を別タスク・別コアにすることで、TLS ハンドシェイクや
  ポーリングで音が途切れないようにしています（デコードを core0 に置くと WiFi/ネットと
  競合して音が乱れる。**core1 が正解**）。
- 両者の間は圧縮 AAC を貯める **FIFO（FreeRTOS StreamBuffer）**。圧縮なので 48kbps ≒ 6KB/秒と
  安価に数秒ぶん先読みでき、ポーリング中の途切れを吸収します。
- UI（画面・キーボード）は Arduino の `loop()`（core1）。

主要ファイル:

```
src/
├─ config.h          Wi-Fi初期値・エリア・音量・省電力・バッファ調整値
├─ stations.h        プリセット選局（局ID・表示名。先頭が初期選局）
├─ radiko.h / .cpp   認証(auth1/auth2/partialkey)・ライブm3u8 URL 組み立て
├─ audio_player.h/.cpp  2タスク本体・FIFO・リングバッファ・playRaw
└─ main.cpp          UI（描画・キー入力・Wi-Fi接続・省電力）
disable_sbr.py       ビルド前にHE-AAC SBRを無効化（省メモリ）
platformio.ini       依存ライブラリ・ビルドフラグ・extra_scripts
```

---

## radiko の現行仕様（実装準拠）

1. **auth1** `GET https://radiko.jp/v2/api/auth1`（ヘッダ `X-Radiko-App: pc_html5` など）
   → 応答ヘッダの `X-Radiko-AuthToken` / `X-Radiko-KeyOffset` / `X-Radiko-KeyLength`
2. **partialkey**: 固定鍵 `bcd151073c03b352e1ef2fd66c32209da9ca0afa` から offset/length を切り出し Base64
3. **auth2** `GET https://radiko.jp/v2/api/auth2`（`X-Radiko-AuthToken` + `X-Radiko-PartialKey`）
   → 本文先頭がエリアコード（例 `JP14,...`）
4. **ライブ配信**:
   `https://alliance-stream-radiko.smartstream.ne.jp/so/playlist.m3u8?station_id=XXX&l=15&lsid=<md5>&type=b`
   に `X-Radiko-AuthToken` を付けて GET → `medialist?session=...` を得る → その medialist を
   GET して `.aac` セグメント URL を取得 → 各セグメントを順次 DL。

実機で判明した重要な挙動:

- **メディアは HTTPS 必須**。平文 HTTP は `Connection reset by peer` で拒否される。
- **radiko はレスポンス毎に接続を閉じる**ため、実質**セグメント毎に TLS ハンドシェイク**が走る
  （keep-alive はほぼ効かない）。1 回のハンドシェイクで ~40KB 程度を消費するのが最大のメモリ圧。
- **medialist(`session=...`) は短命**。同じ URL を再ポーリングすると 404 になる。
  → 404 になったらマスターから取り直す。それまでは使い回してハンドシェイク回数を減らす。
- keep-alive しない前提だが、セグメント終端は **Content-Length** で判定（接続断待ちだと
  1 本目で固まるケースがある）。
- 403 が返る局はそのエリアの**無料配信対象外**。再認証しても無駄なので「配信対象外」表示で停止。

---

## メモリ制約と対策（このプロジェクトの肝）

Cardputer ADV は **PSRAM 非搭載**（ESP32-S3FN8・内蔵 SRAM のみ、実測 heap ≒ 100KB 前後）。
以下が主要な落とし穴と対策です。

### 1. HE-AAC の SBR（50KB 超）が確保できない → SBR 無効化

radiko は HE-AAC。Helix は最初の SBR フレームで `PSInfoSBR`（**50788 バイト**）を `malloc` する。
断片化した heap では 50KB の連続領域が取れず `OOM in SBR` → 無音／クラッシュ。予約トリックや
タスク一時停止でも、解放直後に別コアの `malloc` が穴を奪い**確実性が出ない**。

**解決**: `disable_sbr.py`（`extra_scripts = pre:disable_sbr.py`）が libhelix の
`ConfigHelix.h` にある `HELIX_FEATURE_AUDIO_CODEC_AAC_SBR` をビルド前に自動でコメントアウトし、
**SBR を無効化**（AAC-LC ベースのみ復号＝24kHz、高域控えめ）。あわせてデコーダは
`AACInitDecoderPre()` に **静的 32KB バッファ**（`.bss`）を渡して確保（`StaticAAC` で
`allocateDecoder()` を override）。**malloc を使わないので OOM が原理的に起きない**。
RAM 静的使用は約 26%。

> SBR を戻す（高音質化）には、この 50KB＋αを常時確保できるだけの大幅な省メモリが必要で、
> 本機では現実的でない。

### 2. playRaw のバッファ・エイリアシング → PCM リングバッファ

`M5Cardputer.Speaker.playRaw()` は **バッファをコピーせずポインタ参照**で再生する
（内部 `wav_info.data = data`）。各チャンネルは「再生中 + キュー 2 枠 = 最大 3 個」を同時参照する。
単一バッファを使い回すと**再生中の波形を上書き**し、サンプル順が壊れて“実験音楽”のような音になる
（＝以前の常時プチプチ／音の入れ替わりの正体）。

**解決**: PCM を **リングバッファ**（`ASM_RING` 枚）に組み立て、`isPlaying(ch) >= 2` の間だけ待って
`playRaw` する（＝実時間ペーシング）。渡したバッファは、リングが一周して再び使うまで触らない。

### 3. DMA バッファの取り過ぎに注意

`dma_buf_len × dma_buf_count` を大きくし過ぎると、ステレオ 16bit では巨大なバッファ
（例: 1024×24 ≒ 98KB）になり heap を枯渇させ、セグメントの TLS ハンドシェイクが OOM で
`abort()`。**512×8（≒32KB）程度**に抑える。

### 4. TLS ハンドシェイクと FIFO のトレードオフ

セグメント毎に ~40KB のハンドシェイクが走るため、**FIFO を大きくし過ぎると
ハンドシェイク用 heap が枯れて `SSL - Memory allocation failed (-32512)`**。逆に小さすぎると
medialist 更新時に FIFO が枯れて瞬断。**FIFO ≒ 28KB** が両立点（実機調整値）。

### 5. DNS フォールバック

家庭用ルーターの DNS が不調だと `DNS Failed for radiko.jp` で認証に進めない。
Wi-Fi 接続後に `dns_setserver(8.8.8.8 / 1.1.1.1)` を設定してフォールバックさせている。

---

## 調整パラメータ

| 場所 | 定義 | 役割 |
|---|---|---|
| `audio_player.cpp` | `FIFO_BYTES` / `PREBUF_BYTES` | 圧縮 AAC 先読み量。増やすと途切れに強いが heap を食う（TLS OOM 注意） |
| 〃 | `ASM_RING` / `ASM_SAMPLES` | PCM リングの枚数／1 枚のサンプル数。再生中の上書きを防ぐため最低 4 枚必要 |
| 〃 | `cfg.dma_buf_len` / `dma_buf_count` | I2S DMA。取り過ぎると heap 枯渇 |
| `config.h` | `NET_READ_CHUNK` | 1 回のネット読み出しサイズ |
| `config.h` | `SCREEN_OFF_MS` / `DISPLAY_BRIGHTNESS` | 省電力（消灯時間・明るさ） |

---

## デバッグログの見方（`pio device monitor`, 115200）

```
[boot] CardputerRadiko / PSRAM=no
[wifi] IP=... DNS=8.8.8.8/1.1.1.1
[radiko] auth OK area=JP14
[play] station=TBS
[dec] fmt rate=24000 ch=2 samps=2048          ← 復号フォーマット(SBR無効で24kHz)
[seg] handshake (was disconnected) heap=53740  ← セグメント毎のTLSハンドシェイク(heap注視)
[play] mseq=... played=2 fifo=28000 under=0 starve=0 heap=97000
```

- `under` … スピーカー（PCM）が枯れた回数。**増え続けるなら**下流バッファ不足かリング上書き。
- `starve` … FIFO（先読み）が枯れた回数。**増え続けるなら**通信が追いつかない／FIFO を増やす。
- `[seg] handshake ... heap=` … この値が小さい（~40KB 未満）と TLS が OOM で失敗しやすい。
- `Guru Meditation` / `abort()` … クラッシュ。多くは heap 枯渇かスタック不足。

---

## ビルド環境の注意（Windows）

- PlatformIO CLI は VS Code 拡張同梱。PATH に無い場合は
  `~\.platformio\penv\Scripts\pio.exe` をフルパスで。書き込みは `--upload-port COMx`。
- `run -t upload` と `device monitor` を `;` で 1 行に繋ぐと、起動直後のログを取り逃さない。
- Serial 出力を本体 USB(USB-Serial/JTAG) に出すため、`platformio.ini` に
  `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1` を付与している。
- `MissingPackageManifestError` が出たら `~\.platformio` の `packages` / `platforms` / `.cache`
  を削除して再取得。OneDrive 配下でのビルドはロックの元になりやすいので避ける。
- libhelix はレジストリ解決できないため git 固定（`platformio.ini` の `lib_deps`）。

---

## 既知の制限・今後

- **音質**: SBR 無効のため高域控えめ。PSRAM 無しの本機では SBR と安定再生の両立が困難。
- **セグメント容器**: `.aac`(ADTS) 前提。radiko が MPEG-TS を返す構成には未対応。
- **候補**: 起動時に `v3/station/list/JPxx.xml` から局リストを動的取得、OTA 更新、
  スリープ深化・電池残量表示、ケース／電源の作り込み。
