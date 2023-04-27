#pragma once

//my BTC address
#define BTC_ADDRESS "bc..."

// Pool
#define POOL_URL "51.81.56.15" //"solo.ckpool.org" //"btc.zsolo.bid" "eu.stratum.slushpool.com"
#define POOL_PORT String("3333").toInt()  //6057 //3333
#define POOL_PWD "x"
#define POOL_WORKER "worker1"

#define POOL_ADDRESS_WORKER String(BTC_ADDRESS+"."+POOL_WORKER)



//WiFi
#define WIFI_SSID "OpenWrt"
#define WIFI_PWD ""

#define PIN_BUTTON_1 0
#define PIN_BUTTON_2 14
//#define PIN_POWER_ON 15

#define ESP_DRD_USE_SPIFFS true
#define FS_NO_GLOBALS

void checkScreenButton();

// Mining
#define THREADS 1
#define MAX_NONCE 1000000
// #define MAX_NONCE    1.215.752.192

void runMonitor(void *name);
void runWorker(void *name);


// macros from DateTime.h 
/* Useful Constants */
#define SECS_PER_MIN  (60UL)
#define SECS_PER_HOUR (3600UL)
#define SECS_PER_DAY  (SECS_PER_HOUR * 24L)
 
/* Useful Macros for getting elapsed time */
#define numberOfSeconds(_time_) (_time_ % SECS_PER_MIN)  
#define numberOfMinutes(_time_) ((_time_ / SECS_PER_MIN) % SECS_PER_MIN) 
#define numberOfHours(_time_) (( _time_% SECS_PER_DAY) / SECS_PER_HOUR)