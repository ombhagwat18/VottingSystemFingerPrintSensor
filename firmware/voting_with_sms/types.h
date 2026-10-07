/*****************************************************
 * types.h - types used in function signatures.
 * Kept in a header because the Arduino build generates function
 * prototypes ahead of the sketch body, and they must see these types.
 *****************************************************/
#pragma once
#include <Arduino.h>

enum SmsStatus : uint8_t { SMS_FREE = 0, SMS_QUEUED, SMS_RETRY, SMS_SENT, SMS_FAILED, SMS_SKIPPED };
struct SmsJob {
  uint32_t id, epoch, ms, nextMs;
  char     phone[13], tpl[4], kind[7], var1[40], var2[40];
  uint8_t  status, attempts;
  int16_t  http;
};
enum EnrStep : uint8_t { EN_IDLE = 0, EN_WAIT1, EN_REMOVE, EN_WAIT2, EN_WAIT3, EN_OK, EN_FAIL };
