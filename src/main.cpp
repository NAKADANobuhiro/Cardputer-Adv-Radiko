// =============================================================================
//  main.cpp - CardputerRadiko  UI / Wi-Fi / 選局
// -----------------------------------------------------------------------------
//  操作:
//    ; (↑) / . (↓)  : 局を選ぶ
//    Enter           : 選んだ局を再生
//    space           : 停止
//    - / =           : 音量 down / up （_ と + でも可）
//    w               : Wi-Fi 設定（キーボードで SSID→パスワード入力, 本体保存)
// =============================================================================
#include <M5Cardputer.h>
#include <WiFi.h>
#include <Preferences.h>
#include "lwip/dns.h"
#include "config.h"
#include "stations.h"
#include "audio_player.h"

static Preferences prefs;
static int   g_sel   = DEFAULT_STATION_INDEX;   // 選択カーソル
static int   g_cur   = -1;                       // 再生中の局index(-1=停止)
static int   g_vol   = VOLUME_DEFAULT;
static bool  g_wifiOk = false;

// ---------- Wi-Fi 資格情報 (NVS) ----------
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

// ---------- 画面描画 ----------
static void drawHeader() {
  auto& d = M5Cardputer.Display;
  d.fillRect(0, 0, 240, 18, g_wifiOk ? 0x0320 : 0x8000);
  d.setTextColor(TFT_WHITE);
  d.setTextSize(1);
  d.setCursor(3, 5);
  String head = g_wifiOk ? ("WiFi:OK  " + player::area()) : "WiFi:--";
  char vb[24];
  snprintf(vb, sizeof(vb), "  Vol : %02d [-][+]", g_vol);
  head += vb;
  d.print(head);
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
  d.fillRect(0, 18, 240, 117 - 18, TFT_BLACK);
  d.setTextSize(1);
  const int rows = 7;                 // 表示行数
  int top = g_sel - rows / 2;
  if (top < 0) top = 0;
  if (top > STATION_COUNT - rows) top = STATION_COUNT - rows;
  if (top < 0) top = 0;
  for (int r = 0; r < rows && (top + r) < STATION_COUNT; r++) {
    int i = top + r;
    int y = 20 + r * 13;
    bool selected = (i == g_sel);
    bool playing  = (i == g_cur);
    if (selected) { d.fillRect(0, y - 1, 240, 13, 0x001F); }
    d.setTextColor(selected ? TFT_WHITE : (playing ? TFT_GREEN : TFT_LIGHTGREY));
    d.setCursor(4, y + 2);
    d.printf("%s%s", playing ? "> " : "  ", STATIONS[i].name);
  }
}

static void drawFooter() {
  auto& d = M5Cardputer.Display;
  d.fillRect(0, 117, 240, 18, 0x2104);
  d.setTextColor(TFT_WHITE);
  d.setTextSize(1);
  d.setCursor(3, 121);
  String s = String(stateStr());
  if (g_cur >= 0) s += " : " + String(STATIONS[g_cur].name);
  d.print(s);
}

static void redraw() {
  drawHeader();
  drawList();
  drawFooter();
}

// ---------- 簡易ラインエディタ（Wi-Fi 入力用）----------
static String lineEditor(const char* label, bool mask) {
  auto& d = M5Cardputer.Display;
  String buf = "";
  for (;;) {
    d.fillScreen(TFT_BLACK);
    d.setTextSize(1);
    d.setTextColor(TFT_CYAN); d.setCursor(4, 6);  d.print(label);
    d.setTextColor(TFT_WHITE); d.setCursor(4, 24);
    if (mask) { for (size_t i = 0; i < buf.length(); i++) d.print('*'); }
    else d.print(buf);
    d.setTextColor(TFT_DARKGREY); d.setCursor(4, 118);
    d.print("Enter=確定  DEL=削除  `=中止");

    // 入力待ち
    while (true) {
      M5Cardputer.update();   // ← キーボード状態もここで更新される
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        auto st = M5Cardputer.Keyboard.keysState();
        if (st.enter) return buf;
        if (st.del && buf.length()) buf.remove(buf.length() - 1);
        for (auto c : st.word) {
          if (c == '`') return String("\x01");   // 中止マーカー
          buf += c;
        }
        break;   // 再描画へ
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

// ---------- Wi-Fi 接続 ----------
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
    // フォールバックDNS(Google/Cloudflare)を設定。ルーターのDNSが不調でも
    // radiko.jp を解決できるようにする（DNS Failed 対策）。DHCPは維持。
    ip_addr_t d0, d1;
    IP_ADDR4(&d0, 8, 8, 8, 8);
    IP_ADDR4(&d1, 1, 1, 1, 1);
    dns_setserver(0, &d0);
    dns_setserver(1, &d1);
    LOG("[wifi] IP=%s DNS=8.8.8.8/1.1.1.1\n", WiFi.localIP().toString().c_str());
    return true;
  }
  return false;
}

// ---------- 起動 ----------
void setup() {
  // ★最優先: SBR用の連続50KBを、Wi-Fi/画面初期化より前(heapが綺麗)に予約する
  player::reserveSbrEarly();

  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  // 局名やステータスに日本語を出すため日本語フォントを指定
  M5Cardputer.Display.setFont(&fonts::lgfxJapanGothic_12);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setBrightness(DISPLAY_BRIGHTNESS);
  Serial.begin(115200);
  delay(200);
  LOG("\n[boot] CardputerRadiko / PSRAM=%s\n", psramFound() ? "yes" : "no");

  M5Cardputer.Speaker.setVolume(VOLUME_TO_M5(g_vol));

  // Wi-Fi（無ければセットアップ画面）
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
  M5Cardputer.Speaker.setVolume(VOLUME_TO_M5(g_vol));   // 再設定後にボリュームを反映

  // 起動直後に初期局(TBS)を自動再生
  if (g_wifiOk) {
    g_cur = DEFAULT_STATION_INDEX;
    g_sel = DEFAULT_STATION_INDEX;
    player::play(STATIONS[g_cur].id);
  }
  redraw();
}

// ---------- メインループ ----------
void loop() {
  M5Cardputer.update();

  static uint32_t lastDraw = 0;
  static uint32_t lastActive = millis();   // 最後にキー操作した時刻
  static bool     screenOn   = true;
  bool dirty = false;

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    lastActive = millis();
    // 消灯中のキーは「復帰専用」として消費し、操作としては処理しない
    if (!screenOn) {
      M5Cardputer.Display.setBrightness(DISPLAY_BRIGHTNESS);
      screenOn = true;
      redraw();
      delay(15);
      return;
    }
    auto st = M5Cardputer.Keyboard.keysState();

    // Enter: 選択局を再生
    if (st.enter) {
      g_cur = g_sel;
      player::play(STATIONS[g_cur].id);
      dirty = true;
    }
    // space: 停止
    if (st.space) {
      player::stop();
      g_cur = -1;
      dirty = true;
    }
    for (auto c : st.word) {
      if (c == ';') { if (g_sel > 0) g_sel--; dirty = true; }               // ↑ 選局
      else if (c == '.') { if (g_sel < STATION_COUNT - 1) g_sel++; dirty = true; } // ↓ 選局
      else if (c == '-' || c == '_') { g_vol = max(0, g_vol - 1);           // 音量down(0-15)
                           M5Cardputer.Speaker.setVolume(VOLUME_TO_M5(g_vol)); dirty = true; }
      else if (c == '=' || c == '+') { g_vol = min(VOLUME_MAX, g_vol + 1);  // 音量up(0-15)
                           M5Cardputer.Speaker.setVolume(VOLUME_TO_M5(g_vol)); dirty = true; }
      else if (c == 'w') { wifiSetupUI(); g_wifiOk = connectWiFi();
                           if (g_wifiOk && g_cur >= 0) player::play(STATIONS[g_cur].id);
                           dirty = true; }
    }
  }

  // 無操作が続いたらバックライト消灯（音声再生は継続）
  if (screenOn && millis() - lastActive > SCREEN_OFF_MS) {
    M5Cardputer.Display.setBrightness(0);
    screenOn = false;
  }

  // 消灯中は描画しない（無駄な処理を省く）
  if (!screenOn) { delay(20); return; }

  // キー操作時だけ全画面を再描画（チラつき防止）
  if (dirty) {
    redraw();
    lastDraw = millis();
  } else if (millis() - lastDraw > 400) {
    // 定期更新は「状態が変わったとき」だけ細い帯(ヘッダ/フッタ)を描き直す
    static String lastFoot;
    String foot = String(stateStr());
    if (g_cur >= 0) foot += " : " + String(STATIONS[g_cur].name);
    if (foot != lastFoot) { drawHeader(); drawFooter(); lastFoot = foot; }
    lastDraw = millis();
  }
  delay(15);
}
