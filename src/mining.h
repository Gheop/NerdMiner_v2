
#ifndef MINING_API_H
#define MINING_API_H

// Mining
#define MAX_NONCE_STEP  5000000U
#define MAX_NONCE       25000000U
#define TARGET_NONCE    471136297U
#define DEFAULT_DIFFICULTY  0.00015
// Fallback pool IP used when DNS resolution fails (WiFi.hostByName returns 0/0.0.0.0).
// Comment out to disable the fallback (upstream/generic behaviour).
#define POOL_FALLBACK_IP        "38.51.144.232"   // public-pool.io
#define KEEPALIVE_TIME_ms       30000
#define POOLINACTIVITY_TIME_ms  60000

//#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
#define HARDWARE_SHA265
//#endif

#define TARGET_BUFFER_SIZE 64

void runMonitor(void *name);

void runStratumWorker(void *name);

//Set true by the OTA onStart hook so the miners idle and release the SHA engine.
extern volatile bool ota_active;
extern volatile uint32_t g_lastPoolJobMs;

//race: screen auto-off. The screen costs 1.07% of hashrate on a healthy panel
//(measured 301.48 -> 304.70 kH/s on worker1), so it sleeps when nobody is looking.
//Any button press wakes it and the first press is consumed by the wake-up.
extern volatile bool g_screen_on;
extern volatile uint32_t g_lastInputMs;
bool screenNoteInput(void);      //true if the press only woke the screen

//race/gheop8: per-path hash counters + HW/SW mismatch count (telemetry split)
extern volatile uint32_t race_hashes_hw;
extern volatile float race_khs_hw, race_khs_sw;
extern volatile uint32_t race_hashes_sw;
extern volatile uint32_t race_sha_mismatch;
void runMiner(void *name);

void minerWorkerSw(void * task_id);
void minerWorkerHw(void * task_id);

String printLocalTime(void);

void resetStat();

typedef struct{
  uint8_t bytearray_target[32];
  uint8_t bytearray_pooltarget[32];
  uint8_t merkle_result[32];
  uint8_t bytearray_blockheader[128];
} miner_data;


#endif // UTILS_API_H