// =============================================================================
//  main.cpp - CardputerRadiko  UI / Wi-Fi / 選局 / エリア / 時計
// -----------------------------------------------------------------------------
//  操作:
//    ; (↑) / . (↓)  : 局を選ぶ
//    Enter           : 選んだ局を再生
//    space           : 停止
//    - / =           : 音量 down / up （_ と + でも可）
//    w               : Wi-Fi 設定（キーボードで SSID→パスワード入力, 本体保存）
//    a               : エリア設定（都道府県を選んで局リストを取り込む, 本体保存）
//  画面:
//    上段 = WiFi/エリア/時刻/音量、中央 = 局リスト、
//    番組バンド = 再生中の番組名、下段 = 状態 + キーヒント
// =============================================================================
#include <M5Cardputer.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include "lwip/dns.h"
#include "config.h"
#include "stations.h"
#include "prefectures.h"
#include "radiko.h"
#include "audio_player.h"

static Preferences prefs;
static int   g_sel   = 0;         // 選択カーソル
static int   g_cur   = -1;        // 再生中の局index(-1=停止)
static int   g_vol   = VOLUME_DEFAULT;
static bool  g_wifiOk = false;

// ---- 実行時の局リスト（既定 stations.h か、NVS保存の取得結果）----
static char   g_stId[MAX_STATIONS][20];
static char   g_stName[MAX_STATIONS][48];
static int    g_stCount = 0;
static String g_areaCode = RADIKO_AREA_ID;   // 局リストを取り込んだエリア(表示/保存用)

// =============================================================================
//  NVS: Wi-Fi 資格情報
// =============================================================================
static String loadStr(const char* key, const char* def) {
  prefs.begin("radiko", true);
  String v = prefs.getString(key, def);
  prefs.end();
  return v;
}
static void saveWifi(const String& ssid, const String& pass) {
  prefs.begin("radiko", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

// =============================================================================
//  局リスト（既定 / NVS / radiko から取得）
// =============================================================================
// CDATA / 主要エンティティを軽く整える
static void tidyText(String& s) {
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

// ブロック内の <tag>...</tag> の中身を out へ（最初の1件）
static void extractTag(const String& s, const char* tag, String& out) {
  String open = String("<") + tag + ">";
  String close = String("</") + tag + ">";
  int a = s.indexOf(open);
  if (a < 0) { out = ""; return; }
  a += open.length();
  int b = s.indexOf(close, a);
  if (b < 0) { out = ""; return; }
  out = s.substring(a, b);
  tidyText(out);
}

static void loadStationsDefault() {
  g_stCount = 0;
  for (int i = 0; i < STATION_COUNT && g_stCount < MAX_STATIONS; i++) {
    snprintf(g_stId[g_stCount],   sizeof(g_stId[0]),   "%s", STATIONS[i].id);
    snprintf(g_stName[g_stCount], sizeof(g_stName[0]), "%s", STATIONS[i].name);
    g_stCount++;
  }
  g_areaCode = RADIKO_AREA_ID;
}

static void saveStationsNVS() {
  String blob;
  blob.reserve(g_stCount * 40);
  for (int i = 0; i < g_stCount; i++) {
    blob += g_stId[i]; blob += '\t'; blob += g_stName[i]; blob += '\n';
  }
  prefs.begin("radiko", false);
  prefs.putString("stlist", blob);
  prefs.putString("area", g_areaCode);
  prefs.end();
  LOG("[stlist] saved %d stations (%u bytes) area=%s\n",
      g_stCount, (unsigned)blob.length(), g_areaCode.c_str());
}

static bool loadStationsFromNVS() {
  prefs.begin("radiko", true);
  String blob = prefs.getString("stlist", "");
  String ar   = prefs.getString("area", "");
  prefs.end();
  if (blob.length() == 0) return false;

  g_stCount = 0;
  int start = 0;
  while (start < (int)blob.length() && g_stCount < MAX_STATIONS) {
    int nl = blob.indexOf('\n', start);
    if (nl < 0) nl = blob.length();
    String line = blob.substring(start, nl);
    start = nl + 1;
    int tab = line.indexOf('\t');
    if (tab <= 0) continue;
    String id = line.substring(0, tab);
    String nm = line.substring(tab + 1);
    snprintf(g_stId[g_stCount],   sizeof(g_stId[0]),   "%s", id.c_str());
    snprintf(g_stName[g_stCount], sizeof(g_stName[0]), "%s", nm.c_str());
    g_stCount++;
  }
  if (ar.length()) g_areaCode = ar;
  return g_stCount > 0;
}

// 局一覧XMLを解析して実行時リストへ
static int parseStationList(const String& xml) {
  g_stCount = 0;
  int pos = 0;
  while (g_stCount < MAX_STATIONS) {
    int sp = xml.indexOf("<station", pos);
    if (sp < 0) break;
    int se = xml.indexOf("</station>", sp);
    if (se < 0) break;
    String blk = xml.substring(sp, se);
    pos = se + 10;
    String id, nm;
    extractTag(blk, "id", id);
    extractTag(blk, "name", nm);
    if (id.length() == 0) continue;
    if (nm.length() == 0) nm = id;
    snprintf(g_stId[g_stCount],   sizeof(g_stId[0]),   "%s", id.c_str());
    snprintf(g_stName[g_stCount], sizeof(g_stName[0]), "%s", nm.c_str());
    g_stCount++;
  }
  return g_stCount;
}

// 指定エリアの局リストを radiko から取得して保存（要: 再生停止でheap確保）
static bool fetchStationList(const String& area) {
  player::stop();
  delay(300);
  player::releaseNetwork();     // メディアTLSを解放してheapを空ける
  delay(100);

  String xml;
  int code = radiko::httpGetString(radiko::stationListUrl(area), "", xml, 8000);
  LOG("[stlist] fetch %s HTTP=%d len=%u\n", area.c_str(), code, (unsigned)xml.length());
  if (code != 200 || xml.length() == 0) return false;

  int n = parseStationList(xml);
  if (n <= 0) return false;
  g_areaCode = area;
  saveStationsNVS();
  return true;
}

// =============================================================================
//  時計(NTP)
// =============================================================================
// "HH:MM" を out(6byte以上)へ。時刻未同期なら false。
static bool nowHHMM(char* out) {
  struct tm ti;
  if (!getLocalTime(&ti, 5)) return false;   // 5ms だけ待つ（同期済みなら即返る）
  if (ti.tm_year + 1900 < 2021) return false; // 未同期(1970付近)は表示しない
  snprintf(out, 6, "%02d:%02d", ti.tm_hour, ti.tm_min);
  return true;
}

// =============================================================================
//  画面描画（レイアウト）
//    0- 16 : ヘッダ（WiFi/エリア/時刻/音量）
//   16-100 : 局リスト
//  100-118 : 番組バンド（再生中の番組名）
//  118-135 : フッタ（状態 + キーヒント）
// =============================================================================
static void drawHeader() {
  auto& d = M5Cardputer.Display;
  d.fillRect(0, 0, 240, 16, g_wifiOk ? 0x0320 : 0x8000);
  d.setTextColor(TFT_WHITE);
  d.setTextSize(1);

  // 左: WiFi / エリア / 時刻
  String ar = player::area(); if (ar.length() == 0) ar = g_areaCode;
  String left = (g_wifiOk ? "OK " : "-- ") + ar;
  char clk[6];
  if (nowHHMM(clk)) { left += "  "; left += clk; }
  d.setCursor(3, 3);
  d.print(left);

  // 右: 音量
  char vb[20];
  snprintf(vb, sizeof(vb), "Vol:%02d [-][+]", g_vol);
  int vw = d.textWidth(vb);
  d.setCursor(240 - vw - 3, 3);
  d.print(vb);
}

static const char* stateStr() {
  switch (player::state()) {
    case player::AUTHENTICATING: return "認証中";
    case player::BUFFERING:      return "バッファ中";
    case player::PLAYING:        return "再生中";
    case player::ERROR:          return "エラー";
    default:                     return "停止";
  }
}

static void drawList() {
  auto& d = M5Cardputer.Display;
  d.fillRect(0, 16, 240, 100 - 16, TFT_BLACK);
  d.setTextSize(1);
  const int rows = 6;
  int top = g_sel - rows / 2;
  if (top > g_stCount - rows) top = g_stCount - rows;
  if (top < 0) top = 0;
  for (int r = 0; r < rows && (top + r) < g_stCount; r++) {
    int i = top + r;
    int y = 18 + r * 13;
    bool selected = (i == g_sel);
    bool playing  = (i == g_cur);
    if (selected) d.fillRect(0, y - 1, 240, 13, 0x001F);
    d.setTextColor(selected ? TFT_WHITE : (playing ? TFT_GREEN : TFT_LIGHTGREY));
    d.setCursor(4, y + 1);
    d.printf("%s%s", playing ? "> " : "  ", g_stName[i]);
  }
}

static void drawProgram() {
  auto& d = M5Cardputer.Display;
  d.fillRect(0, 100, 240, 18, TFT_BLACK);
  if (g_cur < 0) return;
  d.setTextSize(1);
  d.setTextColor(TFT_YELLOW);
  d.setCursor(4, 102);
  String p = player::program();
  if (p.length()) d.print("番組: " + p);
  else            d.print("番組: 取得中...");
}

static void drawFooter() {
  auto& d = M5Cardputer.Display;
  d.fillRect(0, 118, 240, 17, 0x2104);
  d.setTextColor(TFT_WHITE);
  d.setTextSize(1);
  // 左: 状態（＋再生中の局名）
  String s = String(stateStr());
  if (g_cur >= 0) s += " : " + String(g_stName[g_cur]);
  d.setCursor(3, 120);
  d.print(s);
  // 右: キーヒント
  const char* hint = "W:WiFi A:Area";
  int hw = d.textWidth(hint);
  d.setCursor(240 - hw - 3, 120);
  d.setTextColor(TFT_DARKGREY);
  d.print(hint);
}

static void redraw() {
  drawHeader();
  drawList();
  drawProgram();
  drawFooter();
}

// =============================================================================
//  簡易ラインエディタ（Wi-Fi 入力用）
// =============================================================================
static String lineEditor(const char* label, bool mask) {
  auto& d = M5Cardputer.Display;
  String buf = "";
  for (;;) {
    d.fillScreen(TFT_BLACK);
    d.setTextSize(1);
    d.setTextColor(TFT_CYAN);  d.setCursor(4, 6);  d.print(label);
    d.setTextColor(TFT_WHITE); d.setCursor(4, 24);
    if (mask) { for (size_t i = 0; i < buf.length(); i++) d.print('*'); }
    else d.print(buf);
    d.setTextColor(TFT_DARKGREY); d.setCursor(4, 118);
    d.print("Enter=確定  DEL=削除  `=中止");

    while (true) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        auto st = M5Cardputer.Keyboard.keysState();
        if (st.enter) return buf;
        if (st.del && buf.length()) buf.remove(buf.length() - 1);
        for (auto c : st.word) {
          if (c == '`') return String("\x01");
          buf += c;
        }
        break;
      }
      delay(10);
    }
  }
}

static void wifiSetupUI() {
  String ssid = lineEditor("Wi-Fi SSID:", false);
  if (ssid == "\x01" || ssid.length() == 0) return;
  String pass = lineEditor("Wi-Fi パスワード:", true);
  if (pass == "\x01") return;
  saveWifi(ssid, pass);
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setCursor(4, 40);
  M5Cardputer.Display.print("保存しました。再接続します...");
  delay(800);
}

// =============================================================================
//  エリア設定UI（都道府県を選び、その局リストを取り込む）
// =============================================================================
static void areaSelectUI() {
  auto& d = M5Cardputer.Display;
  int sel = 0;
  for (int i = 0; i < PREF_COUNT; i++) if (g_areaCode == PREFS[i].code) { sel = i; break; }

  for (;;) {
    d.fillScreen(TFT_BLACK);
    d.setTextSize(1);
    d.setTextColor(TFT_CYAN);
    d.setCursor(4, 2);
    d.print("エリア選択  Enter=決定  `=中止");
    const int rows = 8;
    int top = sel - rows / 2;
    if (top > PREF_COUNT - rows) top = PREF_COUNT - rows;
    if (top < 0) top = 0;
    for (int r = 0; r < rows && (top + r) < PREF_COUNT; r++) {
      int i = top + r;
      int y = 18 + r * 14;
      if (i == sel) d.fillRect(0, y - 1, 240, 14, 0x001F);
      d.setTextColor(i == sel ? TFT_WHITE : TFT_LIGHTGREY);
      d.setCursor(6, y + 1);
      d.printf("%-5s %s", PREFS[i].code, PREFS[i].name);
    }

    while (true) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        auto st = M5Cardputer.Keyboard.keysState();
        if (st.enter) {
          d.fillScreen(TFT_BLACK);
          d.setTextColor(TFT_WHITE);
          d.setCursor(4, 40);
          d.printf("%s の局リスト取得中...", PREFS[sel].name);
          bool ok = fetchStationList(PREFS[sel].code);
          d.fillScreen(TFT_BLACK);
          d.setCursor(4, 40);
          if (ok) { d.printf("取得しました（%d局）", g_stCount); g_sel = 0; g_cur = -1; }
          else    { d.print("取得に失敗しました"); }
          delay(1000);
          return;
        }
        if (st.del) return;                 // DEL でも中止
        bool handled = false;
        for (auto c : st.word) {
          if (c == '`') return;             // 中止
          else if (c == ';') { if (sel > 0) sel--; handled = true; }
          else if (c == '.') { if (sel < PREF_COUNT - 1) sel++; handled = true; }
        }
        (void)handled;
        break;   // 再描画
      }
      delay(10);
    }
  }
}

// =============================================================================
//  Wi-Fi 接続 + NTP
// =============================================================================
static bool connectWiFi() {
  String ssid = loadStr("ssid", WIFI_SSID_DEFAULT);
  String pass = loadStr("pass", WIFI_PASS_DEFAULT);
  if (ssid.length() == 0) return false;

  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_WHITE); d.setTextSize(1);
  d.setCursor(4, 20); d.printf("接続中: %s", ssid.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    d.print("."); delay(400);
    M5Cardputer.update();
  }
  if (WiFi.status() == WL_CONNECTED) {
    // フォールバックDNS(Google/Cloudflare)。ルーターのDNS不調対策。
    ip_addr_t d0, d1;
    IP_ADDR4(&d0, 8, 8, 8, 8);
    IP_ADDR4(&d1, 1, 1, 1, 1);
    dns_setserver(0, &d0);
    dns_setserver(1, &d1);
    // NTP（JST）。時刻はヘッダの時計表示に使用。
    configTime(JST_OFFSET_SEC, 0, NTP_SERVER1, NTP_SERVER2);
    LOG("[wifi] IP=%s DNS=8.8.8.8/1.1.1.1 NTP=%s\n",
        WiFi.localIP().toString().c_str(), NTP_SERVER1);
    return true;
  }
  return false;
}

// =============================================================================
//  起動
// =============================================================================
void setup() {
  player::reserveSbrEarly();

  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setBrightness(DISPLAY_BRIGHTNESS);
  Serial.begin(115200);
  delay(200);
  LOG("\n[boot] CardputerRadiko / PSRAM=%s\n", psramFound() ? "yes" : "no");

  // 局リスト: NVS(前回のエリア取得) があればそれ、無ければ既定(stations.h)
  if (loadStationsFromNVS()) {
    LOG("[stlist] loaded %d stations from NVS area=%s\n", g_stCount, g_areaCode.c_str());
  } else {
    loadStationsDefault();
    LOG("[stlist] default %d stations area=%s\n", g_stCount, g_areaCode.c_str());
  }

  M5Cardputer.Speaker.setVolume(VOLUME_TO_M5(g_vol));

  g_wifiOk = connectWiFi();
  if (!g_wifiOk) {
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    M5Cardputer.Display.setCursor(4, 30);
    M5Cardputer.Display.print("Wi-Fi 未設定 / 接続失敗");
    M5Cardputer.Display.setCursor(4, 50);
    M5Cardputer.Display.print("何かキーを押して設定...");
    while (true) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) break;
      delay(20);
    }
    wifiSetupUI();
    g_wifiOk = connectWiFi();
  }

  player::begin();
  M5Cardputer.Speaker.setVolume(VOLUME_TO_M5(g_vol));

  // 起動直後に先頭局を自動再生
  if (g_wifiOk && g_stCount > 0) {
    g_cur = 0;
    g_sel = 0;
    player::play(g_stId[g_cur]);
  }
  redraw();
}

// =============================================================================
//  メインループ
// =============================================================================
void loop() {
  M5Cardputer.update();

  static uint32_t lastDraw   = 0;
  static uint32_t lastActive = millis();
  static bool     screenOn   = true;
  bool dirty = false;

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    lastActive = millis();
    // 消灯中のキーは「復帰専用」
    if (!screenOn) {
      M5Cardputer.Display.setBrightness(DISPLAY_BRIGHTNESS);
      screenOn = true;
      redraw();
      delay(15);
      return;
    }
    auto st = M5Cardputer.Keyboard.keysState();

    if (st.enter) {
      if (g_stCount > 0) {
        g_cur = g_sel;
        player::play(g_stId[g_cur]);
        dirty = true;
      }
    }
    if (st.space) {
      player::stop();
      g_cur = -1;
      dirty = true;
    }
    for (auto c : st.word) {
      if (c == ';') { if (g_sel > 0) g_sel--; dirty = true; }
      else if (c == '.') { if (g_sel < g_stCount - 1) g_sel++; dirty = true; }
      else if (c == '-' || c == '_') { g_vol = max(0, g_vol - 1);
                           M5Cardputer.Speaker.setVolume(VOLUME_TO_M5(g_vol)); dirty = true; }
      else if (c == '=' || c == '+') { g_vol = min(VOLUME_MAX, g_vol + 1);
                           M5Cardputer.Speaker.setVolume(VOLUME_TO_M5(g_vol)); dirty = true; }
      else if (c == 'w' || c == 'W') { wifiSetupUI(); g_wifiOk = connectWiFi();
                           if (g_wifiOk && g_cur >= 0) player::play(g_stId[g_cur]);
                           dirty = true; }
      else if (c == 'a' || c == 'A') { areaSelectUI(); dirty = true; }
    }
  }

  // 無操作でバックライト消灯（音声は継続）
  if (screenOn && millis() - lastActive > SCREEN_OFF_MS) {
    M5Cardputer.Display.setBrightness(0);
    screenOn = false;
  }
  if (!screenOn) { delay(20); return; }

  if (dirty) {
    redraw();
    lastDraw = millis();
  } else if (millis() - lastDraw > 500) {
    // 状態/時刻/音量/番組名 のいずれかが変わったときだけ細い帯を更新（チラつき防止）
    static String lastLine;
    char clk[6]; clk[0] = 0; nowHHMM(clk);
    String line = String(stateStr()) + "|" + player::area() + "|" + clk +
                  "|" + String(g_vol) + "|" + player::program();
    if (line != lastLine) { drawHeader(); drawProgram(); drawFooter(); lastLine = line; }
    lastDraw = millis();
  }
  delay(15);
}
