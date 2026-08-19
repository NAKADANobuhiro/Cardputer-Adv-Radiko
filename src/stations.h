// =============================================================================
//  stations.h - プリセット選局リスト（神奈川県 / JP14 で聴取可能な局）
// -----------------------------------------------------------------------------
//  id    : radiko の局ID（ストリームURL・XMLで使う正式コード）
//  name  : 画面表示名
//  ※ TBS を先頭に置いてある（DEFAULT_STATION_INDEX=0）。
//  ※ 局IDが正しいかは https://radiko.jp/v3/station/list/JP14.xml で確認可能。
// =============================================================================
#pragma once

struct Station {
  const char* id;
  const char* name;
};

// 神奈川(JP14)で受信できる代表的な局。必要に応じて増減してください。
//  ※ 放送大学(HOUSOU-DAIGAKU)は当エリアの無料配信対象外(401/403)のため除外。
static const Station STATIONS[] = {
  { "TBS",      "TBSラジオ" },        // ← 初期選局
  { "QRR",      "文化放送" },
  { "LFR",      "ニッポン放送" },
  { "JOAK",     "NHKラジオ第1" },     // NHK東京(首都圏)。radikoで配信あり
  { "JOAK-FM",  "NHK FM" },
  // ※ NHKラジオ第2(JOAB)は当エリア無料配信対象外(403)のため除外
  { "RN1",      "ラジオNIKKEI第1" },
  { "INT",      "InterFM897" },
  { "FMT",      "TOKYO FM" },
  { "FMJ",      "J-WAVE" },
  { "JORF",     "ラジオ日本" },       // アール・エフ・ラジオ日本（神奈川の局）
  { "YFM",      "FMヨコハマ" },        // Fm yokohama 84.7（神奈川の局）
  { "BAYFM78",  "bayfm78" },
  { "NACK5",    "NACK5" },
};

static const int STATION_COUNT = sizeof(STATIONS) / sizeof(STATIONS[0]);
