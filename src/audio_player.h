// =============================================================================
//  audio_player.h - radiko ライブ再生（別タスクで動作）
// -----------------------------------------------------------------------------
//  役割: 認証 → チャンクリスト取得 → AACセグメント巡回DL → Helixでデコード →
//        M5Cardputer.Speaker へ供給（バックプレッシャ制御）。
//  UIタスク(loop)からは play()/stop() を呼ぶだけ。実処理は専用FreeRTOSタスク。
// =============================================================================
#pragma once
#include <Arduino.h>

namespace player {

enum State { STOPPED, AUTHENTICATING, BUFFERING, PLAYING, ERROR };

void   reserveSbrEarly();              // ★最優先で呼ぶ: SBR用50KBをheapが綺麗なうちに予約
void   begin();                        // 再生タスク生成（起動時に1回）
void   play(const String& stationId);  // 指定局の再生を開始/切替
void   stop();                         // 停止
void   releaseNetwork();               // メディアTLSを解放（局リスト取得前にheapを空ける）
State  state();                        // 現在の状態
String area();                         // 直近認証で判明したエリア(例 JP14)
String message();                      // UI表示用の短いステータス/エラー文
String program();                      // 現在放送中の番組名(取得できていれば)
void   stats(uint32_t& spkUnder, uint32_t& fifoStarve);  // 診断カウンタ(ロードテスト用)

} // namespace player
