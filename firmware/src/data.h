#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    char account[64];        // account email from daemon (oauthAccount); "" if unknown
    long clock_epoch;        // local wall-clock epoch (s) from daemon; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from daemon); defaults to 24
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

// Multi-account pacing screen (fed by the daemon's {"accts":[...]} message).
#define MAX_ACCOUNTS 6
struct Account {
    char email[40];
    int  used_pct;       // 7-day utilization, 0-100
    int  reset_mins;     // minutes until the 7-day window resets
};
struct AccountsData {
    Account accounts[MAX_ACCOUNTS];
    int  count;
    bool valid;          // false until first accounts message arrives
};
