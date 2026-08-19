// =============================================================================
//  radiko.cpp
// =============================================================================
#include "radiko.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_random.h>
#include "mbedtls/base64.h"

namespace radiko {

// radiko の固定認証キー（ASCII 40文字）
static const char* FULL_KEY = "bcd151073c03b352e1ef2fd66c32209da9ca0afa";

// 共通の radiko ヘッダ
static void addCommonHeaders(HTTPClient& http) {
  http.addHeader("X-Radiko-App", "pc_html5");
  http.addHeader("X-Radiko-App-Version", "0.0.1");
  http.addHeader("X-Radiko-User", "dummy_user");
  http.addHeader("X-Radiko-Device", "pc");
}

// FULL_KEY[offset..offset+length] を Base64 化して partialkey を作る
static String makePartialKey(int offset, int length) {
  int keyLen = strlen(FULL_KEY);
  if (offset < 0 || length <= 0 || offset + length > keyLen) return "";
  unsigned char out[128];
  size_t olen = 0;
  if (mbedtls_base64_encode(out, sizeof(out), &olen,
        (const unsigned char*)(FULL_KEY + offset), length) != 0) {
    return "";
  }
  return String((char*)out).substring(0, olen);
}

// 32桁のランダム16進(lsid 用)
static String randomHex32() {
  const char* h = "0123456789abcdef";
  String s;
  s.reserve(32);
  for (int i = 0; i < 32; i++) s += h[esp_random() & 0x0F];
  return s;
}

bool authenticate(String& tokenOut, String& areaOut) {
  WiFiClientSecure client;
  client.setInsecure();          // 証明書検証はスキップ(組込みの実装簡略化/メモリ節約)

  // ---- auth1 ----
  {
    HTTPClient http;
    http.setReuse(false);
    if (!http.begin(client, "https://radiko.jp/v2/api/auth1")) {
      LOG("[radiko] auth1 begin failed\n");
      return false;
    }
    addCommonHeaders(http);
    const char* collect[] = { "X-Radiko-AuthToken", "X-Radiko-KeyOffset", "X-Radiko-KeyLength" };
    http.collectHeaders(collect, 3);

    int code = http.GET();
    if (code != 200) {
      LOG("[radiko] auth1 HTTP %d\n", code);
      http.end();
      return false;
    }
    String token   = http.header("X-Radiko-AuthToken");
    int    offset  = http.header("X-Radiko-KeyOffset").toInt();
    int    length  = http.header("X-Radiko-KeyLength").toInt();
    http.end();

    if (token.isEmpty()) { LOG("[radiko] no authtoken\n"); return false; }
    String partial = makePartialKey(offset, length);
    if (partial.isEmpty()) { LOG("[radiko] partialkey failed off=%d len=%d\n", offset, length); return false; }
    tokenOut = token;

    // ---- auth2 ----
    HTTPClient http2;
    http2.setReuse(false);
    if (!http2.begin(client, "https://radiko.jp/v2/api/auth2")) {
      LOG("[radiko] auth2 begin failed\n");
      return false;
    }
    addCommonHeaders(http2);
    http2.addHeader("X-Radiko-AuthToken", token);
    http2.addHeader("X-Radiko-PartialKey", partial);

    int code2 = http2.GET();
    if (code2 != 200) {
      LOG("[radiko] auth2 HTTP %d\n", code2);
      http2.end();
      return false;
    }
    String body = http2.getString();   // 例: "JP14,神奈川県,KANAGAWA JAPAN"
    http2.end();

    int comma = body.indexOf(',');
    areaOut = (comma > 0) ? body.substring(0, comma) : body;
    areaOut.trim();
    LOG("[radiko] auth OK area=%s\n", areaOut.c_str());
    return true;
  }
}

String buildPlaylistUrl(const String& stationId) {
  String url = "https://alliance-stream-radiko.smartstream.ne.jp/so/playlist.m3u8";
  url += "?station_id=" + stationId;
  url += "&l=15";
  url += "&lsid=" + randomHex32();
  url += "&type=b";
  return url;
}

int httpGetString(const String& url, const String& token, String& body, uint32_t timeoutMs) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(timeoutMs);
  if (!http.begin(client, url)) return -1;
  if (!token.isEmpty()) http.addHeader("X-Radiko-AuthToken", token);
  int code = http.GET();
  if (code == 200) body = http.getString();
  http.end();
  return code;
}

String pickNextM3U8(const String& body) {
  // メディアプレイリスト(セグメント入り)なら空を返す
  if (body.indexOf("#EXTINF") >= 0) return "";
  // それ以外は「#で始まらない行 = URL」を最後に出てきたもの採用（=最高帯域変種）
  String last = "";
  int start = 0;
  while (start < (int)body.length()) {
    int nl = body.indexOf('\n', start);
    if (nl < 0) nl = body.length();
    String line = body.substring(start, nl);
    line.trim();
    if (line.length() > 0 && line[0] != '#') last = line;
    start = nl + 1;
  }
  return last;
}

} // namespace radiko
