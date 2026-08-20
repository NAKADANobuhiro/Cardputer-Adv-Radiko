// =============================================================================
//  audio_player.cpp
// =============================================================================
#include "audio_player.h"
#include "config.h"
#include "radiko.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/stream_buffer.h"
#include "AACDecoderHelix.h"

namespace player {

// ---- 圧縮AAC の先読みFIFO（ネット取得タスク → デコード再生タスク）----
//  48kbps(=6KB/s)なので数十KBで数秒ぶん。ポーリング中の途切れを吸収する肝。
static const size_t   FIFO_BYTES   = 28 * 1024;   // 圧縮AAC先読み(約4.7s)。TLSハンドシェイク用heapとの両立点
static const size_t   PREBUF_BYTES = 14 * 1024;   // 開始/切替直後に貯めてから鳴らす(約2.3s)
static StreamBufferHandle_t s_fifo = nullptr;
static volatile bool  s_flush      = false;       // 局切替/停止でFIFOを捨てる指示

// ---- 診断カウンタ ----
static volatile uint32_t s_spkUnder  = 0;   // スピーカーが空になった回数(=PCM枯れ)
static volatile uint32_t s_fifoStarve= 0;   // FIFOが空でデコードが待たされた回数

// ---- 認証トークン期限切れ/配信対象外の判別 ----
//  一度でも再生できた後の403=トークン期限切れ(→再認証)。一度も再生できない403が
//  続く場合のみ配信対象外(エリア外)とみなして諦める。
static volatile bool s_playedSeg    = false; // 今回の再生開始後にセグメントを1本でも流したか
static int           s_noPlayStreak = 0;     // 「認証したが1本も再生できず」の連続回数
static volatile bool s_blocked      = false; // 配信対象外で停止中(新規play()まで再試行しない)

// ---- 認証時のデコーダ解放協調（TLSハンドシェイクにメモリを譲る）----
static volatile bool  s_wantIdle    = false; // (未使用)
static volatile bool  s_decoderIdle = false; // (未使用)

// ---- SBR用メモリの事前予約（断片化対策）----
//  HE-AACのSBRデコーダは最初のSBRフレームで sizeof(PSInfoSBR)=50788B を malloc する。
//  再生開始時には heap が断片化していてこの連続領域が取れない。そこで起動直後(heapが綺麗)
//  に同サイズを予約確保しておき、最初のデコード直前に解放→その穴をSBRが確保する。
//  50788ちょうどだと、解放後の穴が malloc ヘッダ分だけ足りず再確保に使えない。
//  余裕を足しておく。
static const size_t   SBR_RESERVE_BYTES = 50788 + 4096;
static void*          g_sbrReserve      = nullptr;
// SBR確保の一瞬、ネット取得タスクを安全地点で止めて割り込みmallocを防ぐ（協調パーク）
static volatile bool  s_parkNet         = false;  // dec→net: 止まってほしい
static volatile bool  s_netParked       = false;  // net→dec: 安全地点で停止中
static volatile bool  g_pcmProduced     = false;  // pcmCallbackが最初のPCMを出したか

// ---- 状態（タスク間共有）----
static volatile State   s_state    = STOPPED;
static volatile bool    s_playing  = false;
static volatile uint32_t s_gen     = 0;          // play()毎に++。再生ループの中断検知に使う
static String           s_stationId = "";
static String           s_token     = "";
static String           s_area      = "";
static String           s_msg       = "";
static String           s_program   = "";   // 現在放送中の番組名(now.xmlから)
static portMUX_TYPE     s_mux       = portMUX_INITIALIZER_UNLOCKED;

// ---- 再生用スピーカーチャンネル ----
static const int SPK_CH = 0;

// ---- メディア取得は HTTPS（radikoのCDNは平文HTTPを拒否）。接続は使い回す。----
//  TLSハンドシェイクが起きる瞬間だけデコーダを一時解放してメモリを確保する
//  （keep-alive中は解放しないので音は途切れない）。
static WiFiClientSecure s_media;

static String forceHttps(const String& url) {
  if (url.startsWith("http://")) return "https://" + url.substring(7);
  return url;
}

// ---- PCM 組み立てバッファ（リング）----
//  ★M5Speaker.playRaw は「バッファをコピーせずポインタ参照」で再生する。
//    各chは current + キュー2枠 = 最大3個を同時参照するため、1個を使い回すと
//    再生中の波形を上書きして音が壊れる（サンプル順が乱れる=“remix”）。
//    → リングで複数用意し、再生中(=isPlayingで最大2キュー+現在1)を上書きしない。
static const int    ASM_RING     = 5;      // 最大3個参照(現在+キュー2) + 余裕2。省メモリ化
static const size_t ASM_SAMPLES  = 4608;   // int16/バッファ(=frames*ch, 24kHz stereoで約0.096s)
static int16_t*     s_asmBuf[ASM_RING] = { nullptr };
static int          s_asmIdx     = 0;      // 現在書込み中のバッファ
static size_t       s_asmLen     = 0;      // 現バッファの充填量(int16)
static uint32_t     s_rate       = 24000;
static bool         s_stereo     = true;

// ---- Helix AAC デコーダ ----
static void pcmCallback(_AACFrameInfo& info, short* pcm, size_t len, void* ref);

// ★SBR(50788B)を含む全デコーダ構造体を「静的バッファ」から確保する版。
//  malloc を一切使わないので、断片化しても OOM in SBR は絶対に起きない。
//  必要量 = AACDecInfo(120)+PSInfoBase(28752)+PSInfoSBR(50788) ≒ 79.7KB → 80KB確保。
class StaticAAC : public libhelix::AACDecoderHelix {
 public:
  StaticAAC(libhelix::AACDataCallback cb) : libhelix::AACDecoderHelix(cb) {}
 protected:
  bool allocateDecoder() override {
    if (decoder == nullptr) {
      // SBR無効時は AACDecInfo+PSInfoBase(約29KB)のみ。余裕をみて32KB。
      static uint8_t s_decBuf[32768];   // .bss に常時確保
      decoder = AACInitDecoderPre(s_decBuf, (int)sizeof(s_decBuf));
    }
    memset(&aacFrameInfo, 0, sizeof(_AACFrameInfo));
    return decoder != nullptr;
  }
};
static StaticAAC s_aac(pcmCallback);

// ---------------------------------------------------------------------------
static void setState(State st, const String& msg = "") {
  portENTER_CRITICAL(&s_mux);
  s_state = st;
  if (msg.length()) s_msg = msg;
  portEXIT_CRITICAL(&s_mux);
}
static bool interrupted(uint32_t gen) { return (!s_playing) || (s_gen != gen); }

// ---- 現バッファを playRaw に渡し、リングを次へ進める ----
//  キュー2枠が埋まっている間は待つ（=実時間ペーシング＆上書き防止）。
//  playRaw はコピーせず参照するので、渡したバッファは次にこのidxへ戻るまで触らない。
static void flushAsm(uint32_t gen) {
  if (s_asmLen == 0) return;
  while (M5Cardputer.Speaker.isPlaying(SPK_CH) >= 2) {   // 両キュー満杯→空くまで待機
    if (interrupted(gen)) { s_asmLen = 0; return; }
    delay(1);
  }
  M5Cardputer.Speaker.playRaw(s_asmBuf[s_asmIdx], s_asmLen, s_rate, s_stereo, 1, SPK_CH, false);
  s_asmIdx = (s_asmIdx + 1) % ASM_RING;    // 次のバッファへ（再生中のものを上書きしない）
  s_asmLen = 0;
}

// ---- Helix からのデコード済みPCMコールバック ----
static void pcmCallback(_AACFrameInfo& info, short* pcm, size_t len, void* ref) {
  if (len == 0) return;
  g_pcmProduced = true;
  static bool logged = false;
  if (!logged) { logged = true;
    LOG("[dec] fmt rate=%d ch=%d samps=%u\n", (int)info.sampRateOut, (int)info.nChans, (unsigned)len);
  }
  s_rate   = info.sampRateOut ? info.sampRateOut : 24000;
  s_stereo = (info.nChans == 2);
  if (s_state != PLAYING) setState(PLAYING);

  size_t i = 0;
  while (i < len) {
    if (!s_playing) { s_asmLen = 0; return; }
    size_t space = ASM_SAMPLES - s_asmLen;
    size_t n = (len - i < space) ? (len - i) : space;
    memcpy(s_asmBuf[s_asmIdx] + s_asmLen, pcm + i, n * sizeof(int16_t));
    s_asmLen += n;
    i += n;
    if (s_asmLen >= ASM_SAMPLES) flushAsm(s_gen);
  }
}

// ---------------------------------------------------------------------------
//  メディアhostへの GET（本文取得）。TLSクライアント使い回し。
// ---------------------------------------------------------------------------
// TLSハンドシェイクが必要(=未接続)なら、その間だけデコーダを解放してメモリを確保。
//  keep-aliveで接続維持中は何もしない（音を切らない）。
// デコーダ解放は断片化の原因になるため廃止（keep-aliveで再接続はまれ）。no-op。
static void handshakeGuardBegin() {}
static void handshakeGuardEnd() {}

static int mediaGet(const String& urlIn, String& body) {
  String url = forceHttps(urlIn);
  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(8000);
  handshakeGuardBegin();
  if (!http.begin(s_media, url)) { handshakeGuardEnd(); return -1; }
  if (!s_token.isEmpty()) http.addHeader("X-Radiko-AuthToken", s_token);
  int code = http.GET();
  handshakeGuardEnd();
  if (code == 200) body = http.getString();
  http.end();
  return code;
}

// ---- FIFOへ n バイト送る（満杯なら空くまで待機＝ネットを実時間ペーシング）----
static void fifoSend(const uint8_t* data, size_t n, uint32_t gen) {
  size_t sent = 0;
  while (sent < n && !interrupted(gen)) {
    sent += xStreamBufferSend(s_fifo, data + sent, n - sent, pdMS_TO_TICKS(50));
  }
}

// ---- セグメント(.aac)をストリーム受信し、FIFOへ流し込む ----
//  ※ keep-alive(接続使い回し)では「接続クローズ」で終端判定できないため、
//    Content-Length ぶんだけ読んで確実に抜ける（これが無いと1本目で停止する）。
static bool streamSegment(const String& urlIn, uint32_t gen) {
  String url = forceHttps(urlIn);
  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(8000);
  bool wasConnected = s_media.connected();   // keep-alive診断: 接続維持されているか
  handshakeGuardBegin();
  if (!http.begin(s_media, url)) { handshakeGuardEnd(); LOG("[seg] begin fail\n"); return false; }
  if (!s_token.isEmpty()) http.addHeader("X-Radiko-AuthToken", s_token);
  http.addHeader("Connection", "keep-alive");
  int code = http.GET();
  if (!wasConnected) LOG("[seg] handshake (was disconnected) heap=%u\n", (unsigned)ESP.getFreeHeap());
  handshakeGuardEnd();
  if (code != 200) { LOG("[seg] HTTP %d\n", code); http.end(); return false; }

  int total = http.getSize();          // Content-Length（-1=不明/チャンク）
  WiFiClient* st = http.getStreamPtr();
  static uint8_t buf[NET_READ_CHUNK];
  int got = 0;
  uint32_t idleStart = millis();
  while (!interrupted(gen)) {
    size_t avail = st->available();
    if (avail) {
      size_t cap = sizeof(buf);
      if (total > 0) { int rem = total - got; if ((int)cap > rem) cap = rem; }
      int n = st->readBytes(buf, avail > cap ? cap : avail);
      if (n > 0) { fifoSend(buf, n, gen); got += n; idleStart = millis(); }  // ★FIFOへ(デコードは別タスク)
      if (total > 0 && got >= total) break;         // 規定バイト読み切り=終端
    } else {
      if (total > 0 && got >= total) break;
      if (!http.connected()) break;                 // 長さ不明時は接続断で終端
      if (millis() - idleStart > 6000) { LOG("[seg] idle timeout got=%d/%d\n", got, total); break; }
      delay(2);
    }
  }
  http.end();
  return (total <= 0) || (got >= total);
}

// ---- m3u8 から #EXT-X-MEDIA-SEQUENCE を取得（無ければ 0）----
static long parseMediaSequence(const String& pl) {
  int p = pl.indexOf("#EXT-X-MEDIA-SEQUENCE:");
  if (p < 0) return 0;
  p += strlen("#EXT-X-MEDIA-SEQUENCE:");
  int nl = pl.indexOf('\n', p);
  if (nl < 0) nl = pl.length();
  return pl.substring(p, nl).toInt();
}

// ---- 相対URLを chunkUrl 基準で絶対化 ----
static String resolveUrl(const String& base, const String& ref) {
  if (ref.startsWith("http")) return ref;
  int schemeEnd = base.indexOf("://");
  int hostEnd   = base.indexOf('/', schemeEnd + 3);
  if (ref.startsWith("/")) return base.substring(0, hostEnd) + ref;
  int lastSlash = base.lastIndexOf('/');
  return base.substring(0, lastSlash + 1) + ref;
}

// ---------------------------------------------------------------------------
//  現在番組名の取得（now.xml をストリーム解析）
// ---------------------------------------------------------------------------
static void setProgram(const String& t) {
  portENTER_CRITICAL(&s_mux);
  s_program = t;
  portEXIT_CRITICAL(&s_mux);
}

// XMLエンティティ/CDATA を軽く整える
static void tidyTitle(String& s) {
  s.trim();
  if (s.startsWith("<![CDATA[")) {
    s = s.substring(9);
    int e = s.indexOf("]]>");
    if (e >= 0) s = s.substring(0, e);
  }
  s.replace("&amp;", "&");
  s.replace("&lt;", "<");
  s.replace("&gt;", ">");
  s.replace("&quot;", "\"");
  s.replace("&#39;", "'");
  s.trim();
}

// now.xml を GET し、指定局の「最初の<title>(=現在番組)」を抜き出す。
//  ※ メモリ節約のため getString せず、少量ずつ読みながら検索してヒットで打ち切る。
//  ※ 呼び出し前に s_media を停止しておくこと（TLSを1本に保つ）。
static bool fetchNowTitle(const String& stationId, const String& area, uint32_t gen, String& out) {
  if (area.isEmpty()) return false;
  String url = radiko::nowProgramUrl(area);
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(8000);
  if (!http.begin(cli, url)) { LOG("[prog] begin fail\n"); return false; }
  int code = http.GET();
  if (code != 200) { LOG("[prog] HTTP %d\n", code); http.end(); return false; }

  WiFiClient* st = http.getStreamPtr();
  String needle = "<station id=\"" + stationId + "\"";
  String buf; buf.reserve(4096);
  char tmp[256];
  bool inStation = false;
  bool ok = false;
  uint32_t t0 = millis();
  while (true) {
    int avail = st->available();
    if (avail > 0) {
      int n = st->readBytes(tmp, avail > (int)sizeof(tmp) ? (int)sizeof(tmp) : avail);
      if (n > 0) buf.concat((const char*)tmp, n);
      if (!inStation) {
        int p = buf.indexOf(needle);
        if (p >= 0) { inStation = true; buf.remove(0, p + needle.length()); }
      }
      if (inStation) {
        int tp = buf.indexOf("<title>");
        if (tp >= 0) {
          int te = buf.indexOf("</title>", tp);
          if (te >= 0) { out = buf.substring(tp + 7, te); ok = true; break; }
        }
        // 次局に達したのに title 無し → 諦める
        if (buf.indexOf("<station id=\"") > 0) break;
      }
      // 肥大化防止（末尾だけ残して境界の取りこぼしを防ぐ）
      if (buf.length() > 8192) buf.remove(0, buf.length() - 2048);
    } else {
      if (!http.connected() && st->available() == 0) break;
      delay(3);
    }
    if (interrupted(gen)) { http.end(); return false; }
    if (millis() - t0 > 8000) break;
  }
  http.end();
  if (ok) tidyTitle(out);
  else LOG("[prog] miss (station=%d)\n", inStation ? 1 : 0);
  return ok && out.length() > 0;
}

// ---------------------------------------------------------------------------
//  1局ぶんの再生ループ（停止/局切替/認証切れで抜ける）
// ---------------------------------------------------------------------------
static void playStation(const String& stationId, uint32_t gen) {
  // マスタープレイリストURL（lsidを固定したいのでループ外で1回だけ組む）
  String master = radiko::buildPlaylistUrl(stationId);
  LOG("[play] station=%s\n", stationId.c_str());
  setState(BUFFERING, "buffering");
  long lastSeq = -1;
  int failStreak = 0;
  String chunkUrl = "";     // medialist URL をキャッシュ（404まで使い回し=マスター取得を減らす）
  uint32_t nextProg = 0;    // 0=即時。FIFOが十分貯まった最初の機会に番組名を取得

  while (!interrupted(gen)) {
    // FIFO(先読みバッファ)に余裕ができるまで待つ（常に数秒ぶん先読み）
    while (!interrupted(gen) && xStreamBufferSpacesAvailable(s_fifo) < FIFO_BYTES / 2) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (interrupted(gen)) break;

    // chunkUrl 未取得なら、マスターから medialist URL を取得（ハンドシェイク削減のため毎回はしない）
    if (chunkUrl.isEmpty()) {
      String body;
      int code = mediaGet(master, body);
      if (code == 401 || code == 403) {   // トークン期限切れ/失効 → 再認証(playerTaskで判定)
        LOG("[play] master HTTP %d -> reauth\n", code);
        s_token = ""; break;
      }
      if (code != 200) {
        if (++failStreak >= 5) { LOG("[play] master fail x%d code=%d\n", failStreak, code); break; }
        delay(300); continue;
      }
      chunkUrl = radiko::pickNextM3U8(body);
      if (chunkUrl.isEmpty()) chunkUrl = master;   // body自体がメディアPL
    }

    // medialist を取得（短命。404になったら chunkUrl を捨てて次回マスター再取得）
    String pl;
    int code = mediaGet(chunkUrl, pl);
    if (code == 401 || code == 403) {   // トークン期限切れ/失効 → 再認証(playerTaskで判定)
      LOG("[play] chunklist HTTP %d -> reauth\n", code);
      s_token = ""; break;
    }
    if (code != 200) {
      chunkUrl = "";        // 期限切れ等 → 次ループでマスターから取り直す
      if (++failStreak >= 5) { LOG("[play] chunklist fail x%d code=%d\n", failStreak, code); break; }
      delay(200); continue;
    }
    failStreak = 0;

    long mseq = parseMediaSequence(pl);
    // セグメントURLを順に処理（既再生ぶんは mseq でスキップ）
    int start = 0; long idx = mseq; int played = 0;
    while (start < (int)pl.length() && !interrupted(gen)) {
      int nl = pl.indexOf('\n', start);
      if (nl < 0) nl = pl.length();
      String line = pl.substring(start, nl); line.trim();
      start = nl + 1;
      if (line.length() == 0 || line[0] == '#') continue;   // タグ/空行はスキップ
      String segUrl = resolveUrl(chunkUrl, line);
      if (idx > lastSeq) {
        bool okSeg = streamSegment(segUrl, gen);
        for (int r = 0; !okSeg && r < SEG_RETRY && !interrupted(gen); r++) {
          // 取得失敗(TLS -32512 等)。TLS文脈を解放(=その領域が空く)→整理を促し再確保。
          s_media.stop();
          delay(60);
          okSeg = streamSegment(segUrl, gen);
          if (okSeg) LOG("[seg] recovered on retry %d\n", r + 1);
        }
        lastSeq = idx;          // 短命セグメントなので、再試行しても駄目なら諦めて次へ
        if (okSeg) played++;
      }
      idx++;
    }
    if (played > 0) s_playedSeg = true;   // 1本でも流せた=このトークンで配信を受けられている
    LOG("[play] mseq=%ld played=%d fifo=%u under=%u starve=%u heap=%u\n", mseq, played,
        (unsigned)xStreamBufferBytesAvailable(s_fifo),
        (unsigned)s_spkUnder, (unsigned)s_fifoStarve, (unsigned)ESP.getFreeHeap());
    if (interrupted(gen)) break;

    // 番組名の取得/更新。FIFOが3/4以上貯まっているときだけ実施し、取得中(≈1〜2s)の
    //  ドレインで音が枯れないようにする。s_media を落として TLS を1本に保つ。
    if (millis() >= nextProg &&
        xStreamBufferBytesAvailable(s_fifo) >= (FIFO_BYTES * 3) / 4) {
      s_media.stop();
      String t;
      if (fetchNowTitle(stationId, s_area, gen, t)) {
        setProgram(t);
        LOG("[prog] %s\n", t.c_str());
        nextProg = millis() + PROG_REFRESH_MS;   // 成功: 次は通常間隔(5分)後
      } else {
        nextProg = millis() + PROG_RETRY_MS;     // 失敗: 短い間隔で再試行(取得中の放置を防ぐ)
      }
      if (interrupted(gen)) break;
    }
    // 新セグメントが無い時だけ待つ（あれば即・上のFIFO空き待ちでペーシング）
    if (played == 0) delay(700);
  }
}

// ---------------------------------------------------------------------------
//  デコード再生タスク: FIFOから圧縮AACを引き出し、Helixでデコード→スピーカー。
//  ネット取得タスクとは独立に動くので、ポーリング中も途切れず供給できる。
// ---------------------------------------------------------------------------
static void decodeTask(void* arg) {
  static uint8_t rb[1024];
  bool prebuf = true;
  bool decoderReady = false;               // Helixの確保は最初のデータ受信時まで遅延
  for (;;) {                               // →起動時の認証(TLS)にメモリを譲る
    if (s_flush) {                         // 局切替/停止: 古い音を捨てて貯め直す
      s_flush = false;
      M5Cardputer.Speaker.stop(SPK_CH);    // 参照中の古いバッファを解放
      xStreamBufferReset(s_fifo);
      s_asmLen = 0;
      s_asmIdx = 0;
      prebuf = true;
    }
    if (prebuf) {                          // 開始直後は少し貯めてから鳴らす
      uint32_t t0 = millis();
      while (!s_flush && xStreamBufferBytesAvailable(s_fifo) < PREBUF_BYTES) {
        if (millis() - t0 > 4000) break;
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      prebuf = false;
    }
    size_t n = xStreamBufferReceive(s_fifo, rb, sizeof(rb), pdMS_TO_TICKS(100));
    if (n > 0) {
      if (!decoderReady) {
        s_aac.begin();              // 静的バッファから確保(mallocなし)。以後 end() しない。
        LOG("[dec] decoder ready heap=%u\n", (unsigned)ESP.getFreeHeap());
        decoderReady = true;
      }
      s_aac.write(rb, n);
    } else if (decoderReady && s_playing && !prebuf) {
      s_fifoStarve++;                     // FIFOが空でデコードが待たされた(=先読み枯れ)
    }
  }
}

// ---------------------------------------------------------------------------
//  ネット取得タスク本体（認証→マスター取り直し→セグメントをFIFOへ）
// ---------------------------------------------------------------------------
static void playerTask(void* arg) {
  for (;;) {
    // SBR確保中は安全地点(=ここ、mallocを保持していない)で停止する
    while (s_parkNet) { s_netParked = true; delay(5); }
    s_netParked = false;

    if (!s_playing) { setState(STOPPED); delay(80); continue; }
    if (s_blocked)  { delay(200); continue; }  // 配信対象外: ERROR表示を保持し再試行しない

    uint32_t gen = s_gen;
    String station;
    portENTER_CRITICAL(&s_mux); station = s_stationId; portEXIT_CRITICAL(&s_mux);

    // 認証（未取得/期限切れ時）
    if (s_token.isEmpty()) {
      setState(AUTHENTICATING, "auth...");
      s_flush = true;
      s_media.stop();          // メディアTLSを解放し、認証ハンドシェイクにメモリを譲る
      LOG("[radiko] pre-auth heap=%u\n", (unsigned)ESP.getFreeHeap());

      String tok, area;
      bool ok = radiko::authenticate(tok, area);
      if (!ok) {
        setState(ERROR, "auth failed");
        delay(3000);
        continue;
      }
      s_token = tok; s_area = area;
    }

    s_playedSeg = false;
    playStation(station, gen);

    // playStation が 401/403 でトークンを破棄した = 再認証して再試行する。
    //  ただし「再認証しても1本も再生できない」が連続する局はエリア配信対象外とみなし、
    //  無限ループを避けて停止する（放送大学・NHK第2 等）。一度でも再生できていれば
    //  期限切れ扱いで即再認証（連続カウントはリセット）。
    if (s_playing && s_token.isEmpty()) {
      if (s_playedSeg) {
        s_noPlayStreak = 0;                    // 期限切れ: 再認証して継続
      } else if (++s_noPlayStreak >= 3) {
        setState(ERROR, "配信対象外");
        s_blocked = true;                      // これ以上ループしない(新規play()まで保持)
        s_noPlayStreak = 0;
        LOG("[play] give up (area-restricted?)\n");
      }
    }

    // ループを抜けた=停止/切替/エラー。停止音のため軽くフラッシュ。
    if (!s_playing) M5Cardputer.Speaker.stop(SPK_CH);
  }
}

// ---------------------------------------------------------------------------
// 旧: SBR用メモリの事前予約。静的バッファ方式に変更したため現在は不要（no-op）。
void reserveSbrEarly() {}

// ---------------------------------------------------------------------------
void begin() {
  // PCM組み立てバッファ(リング)を確保
  for (int i = 0; i < ASM_RING; i++) {
    if (s_asmBuf[i] == nullptr) s_asmBuf[i] = (int16_t*)malloc(ASM_SAMPLES * sizeof(int16_t));
  }
  s_media.setInsecure();   // 証明書検証はスキップ（省メモリ）

  // 圧縮AACの先読みFIFOを生成
  if (s_fifo == nullptr) s_fifo = xStreamBufferCreate(FIFO_BYTES, 1);

  LOG("[player] begin heap=%u\n", (unsigned)ESP.getFreeHeap());

  // スピーカーの DMA を厚めにして途切れ耐性を上げる
  //  ※ M5Cardputer.begin() で既に開始済みのため、一度 end() してから再設定する。
  {
    M5Cardputer.Speaker.end();
    auto cfg = M5Cardputer.Speaker.config();
    cfg.dma_buf_len   = 512;         // ※大きくし過ぎるとDMAバッファがheapを圧迫しTLS OOMになる
    cfg.dma_buf_count = 8;           //   (512x8x2ch x2byte=32KB程度に抑える)
    cfg.task_priority = 5;           // ★出力タスクをデコードより高優先に(I2S枯れ防止)
    cfg.task_pinned_core = 1;
    M5Cardputer.Speaker.config(cfg);
    M5Cardputer.Speaker.begin();
  }

  // ネット取得(core0) と デコード再生(core1) を分離。
  //  優先度: スピーカー出力(5) > デコード(3) > UI loop(1)。出力タスクを最優先にして
  //  デコードのCPU占有でI2Sが枯れる(=周期的な音切れ)のを防ぐ。
  //  Helix HE-AAC(SBR)はスタックを多く使うため decode は大きめ(20KB)に。
  //  デコードは core1(スピーカーと同居だが、ネット/WiFiのcore0競合を避けられる方が安定)。
  //  ※ core0 に置くとネットと競合して音が乱れることを実機で確認済み。
  xTaskCreatePinnedToCore(decodeTask, "radiko-dec", 16384, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(playerTask, "radiko-net", 12288, nullptr, 2, nullptr, 0);
}

void play(const String& stationId) {
  portENTER_CRITICAL(&s_mux);
  s_stationId = stationId;
  s_gen++;                 // 進行中の再生を中断させる
  s_playing = true;
  s_program = "";          // 前局の番組名をクリア（新局で取り直す）
  portEXIT_CRITICAL(&s_mux);
  s_noPlayStreak = 0;      // 局選択でリセット（配信対象外判定を新しく始める）
  s_playedSeg = false;
  s_blocked = false;       // 配信対象外ラッチを解除（新しい局で再挑戦）
  s_flush = true;          // 先読みFIFOを捨てて新局で貯め直す
}

void stop() {
  s_playing = false;
  s_gen++;
  s_flush = true;          // 残りの先読みを破棄
  s_blocked = false;       // 配信対象外ラッチも解除
  setProgram("");          // 番組名表示もクリア
  M5Cardputer.Speaker.stop();
}

void releaseNetwork() {
  s_media.stop();          // メディアhostのTLSコンテキストを解放（約40KB）
}

void stats(uint32_t& spkUnder, uint32_t& fifoStarve) {
  spkUnder   = s_spkUnder;
  fifoStarve = s_fifoStarve;
}

State  state()   { return s_state; }
String area()    { return s_area; }
String message() { return s_msg; }
String program() {
  String p;
  portENTER_CRITICAL(&s_mux);
  p = s_program;
  portEXIT_CRITICAL(&s_mux);
  return p;
}

} // namespace player
