// =============================================================================
//  radiko.h - radiko 認証 & HLS(URL取得) クライアント
// -----------------------------------------------------------------------------
//  参考(現行仕様): streamlink radiko プラグイン / sho10case(2026版)
//    auth1  : https://radiko.jp/v2/api/auth1
//    auth2  : https://radiko.jp/v2/api/auth2
//    固定鍵 : bcd151073c03b352e1ef2fd66c32209da9ca0afa
//    ライブ : https://alliance-stream-radiko.smartstream.ne.jp/so/playlist.m3u8
//             ?station_id=XXX&l=15&lsid=<md5hex>&type=b   (X-Radiko-AuthToken 付与)
//  ※ radiko は概ね年1回 API 変更あり。動かなくなったらこのファイルを更新する。
// =============================================================================
#pragma once
#include <Arduino.h>

namespace radiko {

// auth1 + partialkey + auth2 を実行。成功で token / area を埋めて true。
bool authenticate(String& tokenOut, String& areaOut);

// 局IDからライブ配信の「マスター/メディア m3u8」URL を組み立てる。
String buildPlaylistUrl(const String& stationId);

// エリアの局一覧XMLのURL（認証不要）。例: JP14 → v3/station/list/JP14.xml
String stationListUrl(const String& areaId);

// 現在放送中の番組一覧XMLのURL（認証不要, 局ごとcurrent+next）。
String nowProgramUrl(const String& areaId);

// URL を GET して本文を body へ。token 非空なら X-Radiko-AuthToken を付与。
//  戻り値: HTTPステータス（成功=200）。負値は接続失敗。
int httpGetString(const String& url, const String& token, String& body,
                  uint32_t timeoutMs = 8000);

// m3u8 本文から「次に GET すべき m3u8(=チャンクリスト)URL」を1つ取り出す。
//  本文自体がメディアプレイリスト(#EXTINF入り)なら空文字を返す(= このm3u8で再生)。
String pickNextM3U8(const String& body);

} // namespace radiko
