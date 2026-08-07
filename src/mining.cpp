#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>
#include <nvs.h>
//#include "ShaTests/nerdSHA256.h"
#include "ShaTests/nerdSHA256plus.h"
#include "stratum.h"
#include "mining.h"
#include "utils.h"
#include "monitor.h"
#include "timeconst.h"
#include "drivers/displays/display.h"
#include "drivers/storage/storage.h"
#include <mutex>
#include <list>
#include <map>
#include "mbedtls/sha256.h"
#include "i2c_master.h"

//10 Jobs per second
#define NONCE_PER_JOB_SW 4096
#define NONCE_PER_JOB_HW 16*1024

//Optimisations du chemin SHA materiel, actives par defaut : elles n'exigent aucun
//reglage cote platformio.ini. Chacune est mesurable independamment en la passant a 0.
//  RACE_SHA_DIRECT_READ  lecture DPORT brute au lieu de la sequence protegee (classic)
//  RACE_ASM_FILL         remplissage des blocs SHA en assembleur, une seule base
//  RACE_PREFILL          bloc 2 ecrit pendant que le moteur calcule le bloc 1
//  RACE_ASM_LOOP         boucle sur les nonces entierement en assembleur (classic)
//  RACE_ASM_LOOP_S3      idem sur S3
//Les drapeaux de diagnostic (RACE_RATE, RACE_BENCH, RACE_CLASSIC_BENCH, RACE_KAT,
//RACE_ASM_NONCE, VALIDATION) restent a 0 : ils coutent du debit ou du serie.
#ifndef RACE_SHA_DIRECT_READ
#define RACE_SHA_DIRECT_READ 1
#endif
#ifndef RACE_ASM_FILL
#define RACE_ASM_FILL 1
#endif
#ifndef RACE_PREFILL
#define RACE_PREFILL 1
#endif
#ifndef RACE_ASM_LOOP
#define RACE_ASM_LOOP 1
#endif
#ifndef RACE_ASM_LOOP_S3
#define RACE_ASM_LOOP_S3 1
#endif

#if RACE_RATE
//cheap throughput probe (one printf per 256 jobs). Kept separate from
//RACE_BENCH: the per-phase ccount profiling costs ~10% of the HW path, so perf
//gates must be measured with RACE_RATE alone.
static uint32_t s_race_rate_ms = 0, s_race_rate_hw = 0, s_race_rate_sw = 0;
static uint32_t s_race_rate_jobs = 0;
#endif

#if RACE_BENCH || RACE_CLASSIC_BENCH
#include <xtensa/hal.h>
//cycle accumulators for the HW hot loop, printed every RACE_BENCH_JOBS jobs.
//RACE_CC() must be a full compiler barrier: without it GCC reorders the ccount
//reads (no declared side effects), a delta goes negative and reads as ~2^32
//unsigned — that produced 16k cyc/nonce readings against a real ~920.
#define RACE_CC() ({ uint32_t _c; __asm__ __volatile__("rsr.ccount %0" : "=r"(_c) :: "memory"); _c; })
#define RACE_BENCH_JOBS 256
//Any nonce whose total exceeds this was preempted (Monitor prio 5 > miner prio 3,
//its Serial.printf blocks for ms). Those samples are dropped, not averaged in.
#define RACE_SANE_MAX 10000
struct RaceBenchAcc { uint64_t mid, fill, w1, inter, w2, chk, tot; uint32_t nonces, jobs, dropped; };
static RaceBenchAcc s_race_acc = {};

#endif

//#define I2C_SLAVE

//#define SHA256_VALIDATE
//#define RANDOM_NONCE
#define RANDOM_NONCE_MASK 0xFFFFC000

#ifdef HARDWARE_SHA265
#include <sha/sha_dma.h>
#include <hal/sha_hal.h>
#include <hal/sha_ll.h>

#if defined(CONFIG_IDF_TARGET_ESP32)
#include <sha/sha_parallel_engine.h>
#endif

#endif

nvs_handle_t stat_handle;

uint32_t templates = 0;
uint32_t hashes = 0;
uint32_t Mhashes = 0;
uint32_t totalKHashes = 0;
uint32_t elapsedKHs = 0;
uint64_t upTime = 0;

//per-path hash counters (read by telemetry to split HW vs SW rate)
volatile uint32_t race_hashes_hw = 0;
//race: times a miner task found its queue empty and had to sleep (starvation)
volatile uint32_t race_starved_hw = 0, race_starved_sw = 0;
//latest per-path rate in kH/s, published for the web UI experiment
volatile float race_khs_hw = 0.0f, race_khs_sw = 0.0f;

#ifndef SCREEN_TIMEOUT_S
#define SCREEN_TIMEOUT_S 0          //0 disables the feature
#endif
volatile bool g_screen_on = true;
volatile uint32_t g_lastInputMs = 0;

//Called from the button callbacks. Returns with the screen on and the idle timer
//restarted. The caller checks whether the press was consumed by the wake-up.
bool screenNoteInput(void)
{
  g_lastInputMs = millis();
#if SCREEN_TIMEOUT_S && !RACE_HEADLESS
  if (!g_screen_on) {
    alternateScreenState();
    g_screen_on = true;
    return true;              //this press woke the screen, do not also act on it
  }
#endif
  return false;
}
volatile uint32_t race_hashes_sw = 0;
volatile uint32_t race_sha_mismatch = 0;
volatile int8_t race_kat_state = -1;

volatile uint32_t shares; // increase if blockhash has 32 bits of zeroes
volatile uint32_t valids; // increased if blockhash <= target

// Track best diff
double best_diff = 0.0;

// Variables to hold data from custom textboxes
//Track mining stats in non volatile memory
extern TSettings Settings;

IPAddress serverIP(1, 1, 1, 1); //Temporally save poolIPaddres

//Set by the OTA onStart hook. The hw miner holds the SHA engine lock
//(esp_sha_acquire_hardware) while hashing; suspending it mid-job would keep
//that lock and deadlock Update.end(), which verifies the image SHA-256.
//Instead the miners watch this flag and idle at a safe point, lock released.
volatile bool ota_active = false;
//Last time a pool job (mining.notify) was accepted. runStratumWorker's own
//last_job_time is local, so a frozen Stratum task can't be watched from outside;
//this global lets an independent health watchdog reboot on a real freeze.
volatile uint32_t g_lastPoolJobMs = 0;

//Global work data 
static WiFiClient client;
static miner_data mMiner; //Global miner data (Create a miner class TODO)
mining_subscribe mWorker;
mining_job mJob;
monitor_data mMonitor;
static bool volatile isMinerSuscribed = false;
unsigned long mLastTXtoPool = millis();

int saveIntervals[7] = {5 * 60, 15 * 60, 30 * 60, 1 * 3600, 3 * 3600, 6 * 3600, 12 * 3600};
int saveIntervalsSize = sizeof(saveIntervals)/sizeof(saveIntervals[0]);
int currentIntervalIndex = 0;

bool checkPoolConnection(void) {
  
  if (client.connected()) {
    return true;
  }
  
  isMinerSuscribed = false;

  Serial.println("Client not connected, trying to connect..."); 
  
  //Resolve pool DNS. WiFi.hostByName() returns 0 and writes 0.0.0.0 into serverIP
  //on failure, so check the return value instead of trusting serverIP.
  if(serverIP == IPAddress(1,1,1,1)) {
    if (WiFi.hostByName(Settings.PoolAddress.c_str(), serverIP) != 1 || serverIP == IPAddress(0,0,0,0)) {
      Serial.println("DNS resolve failed, will retry next attempt");
      serverIP = IPAddress(1,1,1,1); //keep unresolved so we retry
      return false;
    }
    Serial.printf("Resolved DNS got: %s\n", serverIP.toString());
  }

  //Try connecting pool IP
  if (!client.connect(serverIP, Settings.PoolPort)) {
    Serial.println("Imposible to connect to : " + Settings.PoolAddress);
    serverIP = IPAddress(1,1,1,1); //force a fresh DNS resolve on next attempt
    return false;
  }

  return true;
}

//Implements a socketKeepAlive function and 
//checks if pool is not sending any data to reconnect again.
//Even connection could be alive, pool could stop sending new job NOTIFY
unsigned long mStart0Hashrate = 0;
bool checkPoolInactivity(unsigned int keepAliveTime, unsigned long inactivityTime, double suggestDifficulty){ 

    unsigned long currentKHashes = (Mhashes*1000) + hashes/1000;
    unsigned long elapsedKHs = currentKHashes - totalKHashes;

    uint32_t time_now = millis();

    // If no shares sent to pool
    // send something to pool to hold socket oppened
    if (time_now < mLastTXtoPool) //32bit wrap
      mLastTXtoPool = time_now;
    if ( time_now > mLastTXtoPool + keepAliveTime)
    {
      mLastTXtoPool = time_now;
      Serial.println("  Sending  : KeepAlive suggest_difficulty");
      //Re-suggest what the pool actually settled on, not DEFAULT_DIFFICULTY. Asking
      //for 0.00015 twice a minute forever, on a pool whose minimum is higher, looks
      //like a misbehaving client from the pool side (BitMaker-hub/NerdMiner_v2#805).
      tx_suggest_difficulty(client, suggestDifficulty);
      /*if(tx_suggest_difficulty(client, DEFAULT_DIFFICULTY)){
        Serial.println("  Sending keepAlive to pool -> Detected client disconnected");
        return true;
      }*/
    }

    if(elapsedKHs == 0){
      //Check if hashrate is 0 during inactivityTIme
      if(mStart0Hashrate == 0) mStart0Hashrate  = time_now; 
      if((time_now-mStart0Hashrate) > inactivityTime) { mStart0Hashrate=0; return true;}
      return false;
    }

  mStart0Hashrate = 0;
  return false;
}

struct JobRequest
{
  uint32_t id;
  uint32_t nonce_start;
  uint32_t nonce_count;
  double difficulty;
  uint8_t sha_buffer[128];
  uint32_t midstate[8];
  uint32_t bake[16];
};

struct JobResult
{
  uint32_t id;
  uint32_t nonce;
  uint32_t nonce_count;
  double difficulty;
  uint8_t hash[32];
};

static std::mutex s_job_mutex;
std::list<std::shared_ptr<JobRequest>> s_job_request_list_sw;
#ifdef HARDWARE_SHA265
std::list<std::shared_ptr<JobRequest>> s_job_request_list_hw;
#endif
std::list<std::shared_ptr<JobResult>> s_job_result_list;
static volatile uint8_t s_working_current_job_id = 0xFF;

static void JobPush(std::list<std::shared_ptr<JobRequest>> &job_list,  uint32_t id, uint32_t nonce_start, uint32_t nonce_count, double difficulty,
                    const uint8_t* sha_buffer, const uint32_t* midstate, const uint32_t* bake)
{
  std::shared_ptr<JobRequest> job = std::make_shared<JobRequest>();
  job->id = id;
  job->nonce_start = nonce_start;
  job->nonce_count = nonce_count;
  job->difficulty = difficulty;
  memcpy(job->sha_buffer, sha_buffer, sizeof(job->sha_buffer));
  memcpy(job->midstate, midstate, sizeof(job->midstate));
  memcpy(job->bake, bake, sizeof(job->bake));
  job_list.push_back(job);
}

struct Submition
{
  double diff;
  bool is32bit;
  bool isValid;
};

static void MiningJobStop(uint32_t &job_pool, std::map<uint32_t, std::shared_ptr<Submition>> & submition_map)
{
  {
    std::lock_guard<std::mutex> lock(s_job_mutex);
    s_job_result_list.clear();
    s_job_request_list_sw.clear();
    #ifdef HARDWARE_SHA265
    s_job_request_list_hw.clear();
    #endif
  }
  s_working_current_job_id = 0xFF;
  job_pool = 0xFFFFFFFF;
  submition_map.clear();
}

#ifdef RANDOM_NONCE
uint64_t s_random_state = 1;
static uint32_t RandomGet()
{
    s_random_state += 0x9E3779B97F4A7C15ull;
    uint64_t z = s_random_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

#endif

void runStratumWorker(void *name) {

// TEST: https://bitcoin.stackexchange.com/questions/22929/full-example-data-for-scrypt-stratum-client

  Serial.println("");
  Serial.printf("\n[WORKER] Started. Running %s on core %d\n", (char *)name, xPortGetCoreID());

  #ifdef DEBUG_MEMORY
  Serial.printf("### [Total Heap / Free heap / Min free heap]: %d / %d / %d \n", ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMinFreeHeap());
  #endif

  std::map<uint32_t, std::shared_ptr<Submition>> s_submition_map;

#ifdef I2C_SLAVE
  std::vector<uint8_t> i2c_slave_vector;

  //scan for i2c slaves
  if (i2c_master_start() == 0)
    i2c_slave_vector = i2c_master_scan(0x0, 0x80);
  Serial.printf("Found %d slave workers\n", i2c_slave_vector.size());
  if (!i2c_slave_vector.empty())
  {
    Serial.print("  Workers: ");
    for (size_t n = 0; n < i2c_slave_vector.size(); ++n)
      Serial.printf("0x%02X,", (uint32_t)i2c_slave_vector[n]);
    Serial.println("");
  }
#endif

  // connect to pool  
  double currentPoolDifficulty = DEFAULT_DIFFICULTY;
  uint32_t nonce_pool = 0;
  uint32_t job_pool = 0xFFFFFFFF;
  uint32_t last_job_time = millis();

  //Exponential backoff for pool reconnects: resume fast after a short glitch
  //(1s, 2s, 4s...) but stay gentle with the pool if it is really down (cap 15s).
  uint32_t pool_retry_delay_s = 1;

  while(true) {

    if(WiFi.status() != WL_CONNECTED){
      // WiFi is disconnected, so reconnect now
      mMonitor.NerdStatus = NM_Connecting;
      MiningJobStop(job_pool, s_submition_map);
      WiFi.reconnect();
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    }

    if(!checkPoolConnection()){
      MiningJobStop(job_pool, s_submition_map);
      Serial.printf("Pool unreachable, retrying in %us\n", pool_retry_delay_s);
      vTaskDelay((pool_retry_delay_s * 1000) / portTICK_PERIOD_MS);
      if (pool_retry_delay_s < 15)
        pool_retry_delay_s *= 2;
      continue;
    }
    pool_retry_delay_s = 1; //connected: next incident restarts from 1s

    if(!isMinerSuscribed)
    {
      //Stop miner current jobs
      mWorker = init_mining_subscribe();

      // STEP 1: Pool server connection (SUBSCRIBE)
      if(!tx_mining_subscribe(client, mWorker)) { 
        client.stop();
        MiningJobStop(job_pool, s_submition_map);
        continue; 
      }
      
      //Bounded: both destinations are fixed arrays and the sources come straight
      //from user input in the config portal.
      snprintf(mWorker.wName, sizeof(mWorker.wName), "%s", Settings.BtcWallet);
      snprintf(mWorker.wPass, sizeof(mWorker.wPass), "%s", Settings.PoolPassword);
      // STEP 2: Pool authorize work (Block Info)
      tx_mining_auth(client, mWorker.wName, mWorker.wPass); //Don't verifies authoritzation, TODO
      //tx_mining_auth2(client, mWorker.wName, mWorker.wPass); //Don't verifies authoritzation, TODO

      // STEP 3: Suggest pool difficulty
      tx_suggest_difficulty(client, currentPoolDifficulty);

      isMinerSuscribed=true;
      uint32_t time_now = millis();
      mLastTXtoPool = time_now;
      last_job_time = time_now;
      g_lastPoolJobMs = time_now;
    }

    //Check if pool is down for almost 5minutes and then restart connection with pool (1min=600000ms)
    if(checkPoolInactivity(KEEPALIVE_TIME_ms, POOLINACTIVITY_TIME_ms, currentPoolDifficulty)){
      //Restart connection
      Serial.println("  Detected more than 2 min without data form stratum server. Closing socket and reopening...");
      client.stop();
      isMinerSuscribed=false;
      MiningJobStop(job_pool, s_submition_map);
      continue; 
    }

    {
      uint32_t time_now = millis();
      if (time_now < last_job_time) //32bit wrap
        last_job_time = time_now;
      if (time_now >= last_job_time + 10*60*1000)  //10minutes without job
      {
        client.stop();
        isMinerSuscribed=false;
        MiningJobStop(job_pool, s_submition_map);
        continue;
      }
    }

    uint32_t hw_midstate[8];
    uint32_t diget_mid[8];
    uint32_t bake[16];
    #if defined(CONFIG_IDF_TARGET_ESP32)
    uint8_t sha_buffer_swap[128];
    #endif

    //Read pending messages from pool
    while(client.connected() && client.available())
    {
      String line = client.readStringUntil('\n');
      //Serial.println("  Received message from pool");      
      stratum_method result = parse_mining_method(line);
      switch (result)
      {
          case MINING_NOTIFY:         if(parse_mining_notify(line, mJob))
                                      {
                                          {
                                            std::lock_guard<std::mutex> lock(s_job_mutex);
                                            s_job_request_list_sw.clear();
                                            #ifdef HARDWARE_SHA265
                                            s_job_request_list_hw.clear();
                                            #endif
                                          }
                                          //Increse templates readed
                                          templates++;
                                          job_pool++;
                                          s_working_current_job_id = job_pool & 0xFF; //Terminate current job in thread

                                          last_job_time = millis();
                                          g_lastPoolJobMs = last_job_time;
                                          mLastTXtoPool = last_job_time;

                                          uint32_t mh = hashes/1000000;
                                          Mhashes += mh;
                                          hashes -= mh*1000000;

                                          //Prepare data for new jobs
                                          mMiner=calculateMiningData(mWorker, mJob);

                                          memset(mMiner.bytearray_blockheader+80, 0, 128-80);
                                          mMiner.bytearray_blockheader[80] = 0x80;
                                          mMiner.bytearray_blockheader[126] = 0x02;
                                          mMiner.bytearray_blockheader[127] = 0x80;

                                          nerd_mids(diget_mid, mMiner.bytearray_blockheader);
                                          nerd_sha256_bake(diget_mid, mMiner.bytearray_blockheader+64, bake);

                                          #ifdef HARDWARE_SHA265
                                          #if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)
                                            esp_sha_acquire_hardware();
                                            sha_hal_hash_block(SHA2_256,  mMiner.bytearray_blockheader, 64/4, true);
                                            sha_hal_read_digest(SHA2_256, hw_midstate);
                                            esp_sha_release_hardware();
                                          #endif
                                          #endif

                                          #if defined(CONFIG_IDF_TARGET_ESP32)
                                          for (int i = 0; i < 32; ++i)
                                            ((uint32_t*)sha_buffer_swap)[i] = __builtin_bswap32(((const uint32_t*)(mMiner.bytearray_blockheader))[i]);
                                          #endif

                                          #ifdef RANDOM_NONCE
                                          nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
                                          #else
                                            #ifdef I2C_SLAVE
                                            if (!i2c_slave_vector.empty())
                                              nonce_pool = 0x10000000;
                                            else
                                            #endif
                                              nonce_pool = 0xDA54E700;  //nonce 0x00000000 is not possible, start from some random nonce
                                          #endif
                                          

                                          {
                                            std::lock_guard<std::mutex> lock(s_job_mutex);
                                            for (int i = 0; i < 4; ++ i)
                                            {
                                              #if 1
                                              JobPush( s_job_request_list_sw, job_pool, nonce_pool, NONCE_PER_JOB_SW, currentPoolDifficulty, mMiner.bytearray_blockheader, diget_mid, bake);
                                              #ifdef RANDOM_NONCE
                                              nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
                                              #else
                                              nonce_pool += NONCE_PER_JOB_SW;
                                              #endif
                                              #endif
                                              #ifdef HARDWARE_SHA265
                                                #if defined(CONFIG_IDF_TARGET_ESP32)
                                                  JobPush( s_job_request_list_hw, job_pool, nonce_pool, NONCE_PER_JOB_HW, currentPoolDifficulty, sha_buffer_swap, hw_midstate, bake);
                                                #else
                                                  JobPush( s_job_request_list_hw, job_pool, nonce_pool, NONCE_PER_JOB_HW, currentPoolDifficulty, mMiner.bytearray_blockheader, hw_midstate, bake);
                                                #endif
                                              #ifdef RANDOM_NONCE
                                              nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
                                              #else
                                              nonce_pool += NONCE_PER_JOB_HW;
                                              #endif
                                              #endif
                                            }
                                          }
                                          #ifdef I2C_SLAVE
                                          //Nonce for nonce_pool starts from 0x10000000
                                          //For i2c slave we give nonces from 0x20000000, that is 0x10000000 nonces per slave
                                          i2c_feed_slaves(i2c_slave_vector, job_pool & 0xFF, 0x20, currentPoolDifficulty, mMiner.bytearray_blockheader);
                                          #endif
                                      } else
                                      {
                                        Serial.println("Parsing error, need restart");
                                        client.stop();
                                        isMinerSuscribed=false;
                                        MiningJobStop(job_pool, s_submition_map);
                                      }
                                      break;
          case MINING_SET_DIFFICULTY: parse_mining_set_difficulty(line, currentPoolDifficulty);
                                      break;
          case STRATUM_SUCCESS:       {
                                        unsigned long id = parse_extract_id(line);
                                        auto itt = s_submition_map.find(id);
                                        if (itt != s_submition_map.end())
                                        {
                                          if (itt->second->diff > best_diff)
                                            best_diff = itt->second->diff;
                                          if (itt->second->is32bit)
                                            shares++;
                                          if (itt->second->isValid)
                                          {
                                            Serial.println("CONGRATULATIONS! Valid block found");
                                            valids++;
                                          }
                                          s_submition_map.erase(itt);
                                        }
                                      }
                                      break;
          case STRATUM_PARSE_ERROR:   {
                                        unsigned long id = parse_extract_id(line);
                                        auto itt = s_submition_map.find(id);
                                        if (itt != s_submition_map.end())
                                        {
                                          Serial.printf("Refuse submition %d\n", id);
                                          s_submition_map.erase(itt);
                                        }
                                      }
                                      break;
          default:                    Serial.println("  Parsed JSON: unknown"); break;

      }
    }

    std::list<std::shared_ptr<JobResult>> job_result_list;
    #ifdef I2C_SLAVE
    if (i2c_slave_vector.empty() || job_pool == 0xFFFFFFFF)
    {
      vTaskDelay(50 / portTICK_PERIOD_MS); //Small delay
    } else
    {
      uint32_t time_start = millis();
      i2c_hit_slaves(i2c_slave_vector);
      vTaskDelay(5 / portTICK_PERIOD_MS);
      uint32_t nonces_done = 0;
      std::vector<uint32_t> nonce_vector = i2c_harvest_slaves(i2c_slave_vector, job_pool & 0xFF, nonces_done);
      hashes += nonces_done;
      for (size_t n = 0; n < nonce_vector.size(); ++n)
      {
        std::shared_ptr<JobResult> result = std::make_shared<JobResult>();
        ((uint32_t*)(mMiner.bytearray_blockheader+64+12))[0] = nonce_vector[n];
        if (nerd_sha256d_baked(diget_mid, mMiner.bytearray_blockheader+64, bake, result->hash))
        {
          result->id = job_pool;
          result->nonce = nonce_vector[n];
          result->nonce_count = 0;
          result->difficulty = diff_from_target(result->hash);
          job_result_list.push_back(result);
        }
      }
      uint32_t time_end = millis();
      //if (nonces_done > 16384)
        //Serial.printf("Harvest slaves in %dms hashes=%d\n", time_end - time_start, nonces_done);
      if (time_end > time_start)
      {
        uint32_t elapsed = time_end - time_start;
        if (elapsed < 50)
          vTaskDelay((50 - elapsed) / portTICK_PERIOD_MS);
      } else
        vTaskDelay(40 / portTICK_PERIOD_MS);
    }
    #else
    vTaskDelay(50 / portTICK_PERIOD_MS); //Small delay
    #endif

    
    if (job_pool != 0xFFFFFFFF)
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      job_result_list.insert(job_result_list.end(), s_job_result_list.begin(), s_job_result_list.end());
      s_job_result_list.clear();

#if 1
      while (s_job_request_list_sw.size() < 4)
      {
        JobPush( s_job_request_list_sw, job_pool, nonce_pool, NONCE_PER_JOB_SW, currentPoolDifficulty, mMiner.bytearray_blockheader, diget_mid, bake);
        #ifdef RANDOM_NONCE
        nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
        #else
        nonce_pool += NONCE_PER_JOB_SW;
        #endif
      }
#endif

      #ifdef HARDWARE_SHA265
      while (s_job_request_list_hw.size() < 4)
      {
        #if defined(CONFIG_IDF_TARGET_ESP32)
          JobPush( s_job_request_list_hw, job_pool, nonce_pool, NONCE_PER_JOB_HW, currentPoolDifficulty, sha_buffer_swap, hw_midstate, bake);
        #else
          JobPush( s_job_request_list_hw, job_pool, nonce_pool, NONCE_PER_JOB_HW, currentPoolDifficulty, mMiner.bytearray_blockheader, hw_midstate, bake);
        #endif
        #ifdef RANDOM_NONCE
        nonce_pool = RandomGet() & RANDOM_NONCE_MASK;
        #else
        nonce_pool += NONCE_PER_JOB_HW;
        #endif
      }
      #endif
    }

    while (!job_result_list.empty())
    {
      std::shared_ptr<JobResult> res = job_result_list.front();
      job_result_list.pop_front();

      hashes += res->nonce_count;
      if (res->difficulty > currentPoolDifficulty && job_pool == res->id && res->nonce != 0xFFFFFFFF)
      {
        if (!client.connected())
          break;
        unsigned long sumbit_id = 0;
        tx_mining_submit(client, mWorker, mJob, res->nonce, sumbit_id);
        Serial.print("   - Current diff share: "); Serial.println(res->difficulty,12);
        Serial.print("   - Current pool diff : "); Serial.println(currentPoolDifficulty,12);
        Serial.print("   - TX SHARE: ");
        for (size_t i = 0; i < 32; i++)
            Serial.printf("%02x", res->hash[i]);
        Serial.println("");
        mLastTXtoPool = millis();

        std::shared_ptr<Submition> submition = std::make_shared<Submition>();
        submition->diff = res->difficulty;
        submition->is32bit = (res->hash[29] == 0 && res->hash[28] == 0);
        if (submition->is32bit)
        {
          submition->isValid = checkValid(res->hash, mMiner.bytearray_target);
        } else
          submition->isValid = false;

        s_submition_map.insert(std::make_pair(sumbit_id, submition));
        if (s_submition_map.size() > 32)
          s_submition_map.erase(s_submition_map.begin());
      }
    }
  }
}

//////////////////THREAD CALLS///////////////////

void minerWorkerSw(void * task_id)
{
  unsigned int miner_id = (uint32_t)task_id;
  Serial.printf("[MINER] %d Started minerWorkerSw Task on core %d!\n", miner_id, xPortGetCoreID());

  std::shared_ptr<JobRequest> job;
  std::shared_ptr<JobResult> result;
  uint8_t hash[32];
  uint32_t wdt_counter = 0;
  while (1)
  {
    if (ota_active) { vTaskDelay(100 / portTICK_PERIOD_MS); continue; } //idle during OTA
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      if (result)
      {
        race_hashes_sw += result->nonce_count;
        if (s_job_result_list.size() < 16)
          s_job_result_list.push_back(result);
        result.reset();
      }
      if (!s_job_request_list_sw.empty())
      {
        job = s_job_request_list_sw.front();
        s_job_request_list_sw.pop_front();
      } else
        job.reset();
    }
    if (job)
    {
      result = std::make_shared<JobResult>();
      result->difficulty = job->difficulty;
      result->nonce = 0xFFFFFFFF;
      result->id = job->id;
      result->nonce_count = job->nonce_count;
      uint8_t job_in_work = job->id & 0xFF;
      for (uint32_t n = 0; n < job->nonce_count; ++n)
      {
        ((uint32_t*)(job->sha_buffer+64+12))[0] = job->nonce_start+n;
        if (nerd_sha256d_baked(job->midstate, job->sha_buffer+64, job->bake, hash))
        {
          double diff_hash = diff_from_target(hash);
          if (diff_hash > result->difficulty)
          {
            result->difficulty = diff_hash;
            result->nonce = job->nonce_start+n;
            memcpy(result->hash, hash, 32);
          }
        }

        if ( (uint16_t)(n & 0xFF) == 0 &&s_working_current_job_id != job_in_work)
        {
          result->nonce_count = n+1;
          break;
        }
      }
    } else
      vTaskDelay(2 / portTICK_PERIOD_MS);

    wdt_counter++;
    if (wdt_counter >= 8)
    {
      wdt_counter = 0;
      esp_task_wdt_reset();
    }
  }
}

#ifdef HARDWARE_SHA265

#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)

static inline void nerd_sha_ll_fill_text_block_sha256(const void *input_text, uint32_t nonce)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    REG_WRITE(&reg_addr_buf[0], data_words[0]);
    REG_WRITE(&reg_addr_buf[1], data_words[1]);
    REG_WRITE(&reg_addr_buf[2], data_words[2]);
    REG_WRITE(&reg_addr_buf[3], nonce);
    REG_WRITE(&reg_addr_buf[4], 0x00000080);
    REG_WRITE(&reg_addr_buf[5], 0x00000000);
    REG_WRITE(&reg_addr_buf[6], 0x00000000);
    REG_WRITE(&reg_addr_buf[7], 0x00000000);
    REG_WRITE(&reg_addr_buf[8], 0x00000000);
    REG_WRITE(&reg_addr_buf[9], 0x00000000);
    REG_WRITE(&reg_addr_buf[10], 0x00000000);
    REG_WRITE(&reg_addr_buf[11], 0x00000000);
    REG_WRITE(&reg_addr_buf[12], 0x00000000);
    REG_WRITE(&reg_addr_buf[13], 0x00000000);
    REG_WRITE(&reg_addr_buf[14], 0x00000000);
    REG_WRITE(&reg_addr_buf[15], 0x80020000);
}

//Same as above but skips SHA_TEXT[9..14]: after the intermediate block
//(nerd_sha_ll_fill_text_block_sha256_inter) those registers already hold 0
//and the engine does not clobber them, so rewriting them every nonce is waste.
//Requires SHA_TEXT[9..14] to have been zeroed once beforehand.
static inline void nerd_sha_ll_fill_text_block_sha256_fast(const void *input_text, uint32_t nonce)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    REG_WRITE(&reg_addr_buf[0], data_words[0]);
    REG_WRITE(&reg_addr_buf[1], data_words[1]);
    REG_WRITE(&reg_addr_buf[2], data_words[2]);
    REG_WRITE(&reg_addr_buf[3], nonce);
    REG_WRITE(&reg_addr_buf[4], 0x00000080);   //inter wrote digest word here
    REG_WRITE(&reg_addr_buf[5], 0x00000000);   //inter wrote digest word here
    REG_WRITE(&reg_addr_buf[6], 0x00000000);   //inter wrote digest word here
    REG_WRITE(&reg_addr_buf[7], 0x00000000);   //inter wrote digest word here
    REG_WRITE(&reg_addr_buf[8], 0x00000000);   //inter wrote 0x80 here
    REG_WRITE(&reg_addr_buf[15], 0x80020000);  //inter wrote 0x00010000 here
}

static inline void nerd_sha_ll_fill_text_block_sha256_inter()
{
  uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

  DPORT_INTERRUPT_DISABLE();
  REG_WRITE(&reg_addr_buf[0], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 0 * 4));
  REG_WRITE(&reg_addr_buf[1], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 1 * 4));
  REG_WRITE(&reg_addr_buf[2], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 2 * 4));
  REG_WRITE(&reg_addr_buf[3], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 3 * 4));
  REG_WRITE(&reg_addr_buf[4], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 4 * 4));
  REG_WRITE(&reg_addr_buf[5], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 5 * 4));
  REG_WRITE(&reg_addr_buf[6], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 6 * 4));
  REG_WRITE(&reg_addr_buf[7], DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 7 * 4));
  DPORT_INTERRUPT_RESTORE();

  //SHA_TEXT[9..14] are kept at zero for the whole job (zeroed once in
  //minerWorkerHw; neither fill nor the engine touches them), so only the
  //two registers that alternate between the fills need rewriting here.
  REG_WRITE(&reg_addr_buf[8], 0x00000080);
  REG_WRITE(&reg_addr_buf[15], 0x00010000);
}

static inline void nerd_sha_ll_read_digest(void* ptr)
{
  DPORT_INTERRUPT_DISABLE();
  ((uint32_t*)ptr)[0] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 0 * 4);
  ((uint32_t*)ptr)[1] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 1 * 4);
  ((uint32_t*)ptr)[2] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 2 * 4);
  ((uint32_t*)ptr)[3] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 3 * 4);
  ((uint32_t*)ptr)[4] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 4 * 4);
  ((uint32_t*)ptr)[5] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 5 * 4);
  ((uint32_t*)ptr)[6] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 6 * 4);  
  ((uint32_t*)ptr)[7] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 7 * 4);
  DPORT_INTERRUPT_RESTORE();
}


static inline bool nerd_sha_ll_read_digest_if(void* ptr)
{
  DPORT_INTERRUPT_DISABLE();
  uint32_t last = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 7 * 4);
  #if 1
  if ( (uint16_t)(last >> 16) != 0)
  {
    DPORT_INTERRUPT_RESTORE();
    return false;
  }
  #endif

  ((uint32_t*)ptr)[7] = last;
  ((uint32_t*)ptr)[0] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 0 * 4);
  ((uint32_t*)ptr)[1] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 1 * 4);
  ((uint32_t*)ptr)[2] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 2 * 4);
  ((uint32_t*)ptr)[3] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 3 * 4);
  ((uint32_t*)ptr)[4] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 4 * 4);
  ((uint32_t*)ptr)[5] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 5 * 4);
  ((uint32_t*)ptr)[6] = DPORT_SEQUENCE_REG_READ(SHA_H_BASE + 6 * 4);  
  DPORT_INTERRUPT_RESTORE();
  return true;
}

static inline void nerd_sha_ll_write_digest(void *digest_state)
{
    uint32_t *digest_state_words = (uint32_t *)digest_state;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_H_BASE);

    REG_WRITE(&reg_addr_buf[0], digest_state_words[0]);
    REG_WRITE(&reg_addr_buf[1], digest_state_words[1]);
    REG_WRITE(&reg_addr_buf[2], digest_state_words[2]);
    REG_WRITE(&reg_addr_buf[3], digest_state_words[3]);
    REG_WRITE(&reg_addr_buf[4], digest_state_words[4]);
    REG_WRITE(&reg_addr_buf[5], digest_state_words[5]);
    REG_WRITE(&reg_addr_buf[6], digest_state_words[6]);
    REG_WRITE(&reg_addr_buf[7], digest_state_words[7]);
}

static inline void nerd_sha_hal_wait_idle()
{
    while (REG_READ(SHA_BUSY_REG))
    {}
}

//#define VALIDATION
#if RACE_ASM_LOOP_S3
//Boucle sur les nonces en assembleur, version S3. Meme raison que sur classic : les
//blocs asm declarent un clobber memoire, donc repasser par du C entre eux force gcc a
//tout relire. Plus simple ici : un seul registre de base couvre le peripherique
//(MODE +0x00, START +0x10, CONTINUE +0x14, BUSY +0x18, H +0x40, TEXT +0x80) et le
//nonce s'ecrit tel quel dans TEXT[3], donc il s'incremente de 1 sans decoupage.
//SHA_TEXT[9..14] restent a zero pour tout le job, comme dans la version C.
//Retourne le nombre de nonces restants ; sur un candidat le digest est dans SHA_H.
static inline uint32_t nerd_sha_s3_run_asm(const void *in, const uint32_t *mid,
                                           uint32_t *nonce_io, uint32_t count)
{
    uint32_t remaining = count, nonce = *nonce_io;
    __asm__ __volatile__(
    "0:\n\t"
        //Midstate reinjecte : le moteur ecrase H a chaque hash.
        "l32i    a8, %[mid], 0\n\t"  "s32i    a8, %[sc], 0x40\n\t"
        "l32i    a8, %[mid], 4\n\t"  "s32i    a8, %[sc], 0x44\n\t"
        "l32i    a8, %[mid], 8\n\t"  "s32i    a8, %[sc], 0x48\n\t"
        "l32i    a8, %[mid], 12\n\t"  "s32i    a8, %[sc], 0x4C\n\t"
        "l32i    a8, %[mid], 16\n\t"  "s32i    a8, %[sc], 0x50\n\t"
        "l32i    a8, %[mid], 20\n\t"  "s32i    a8, %[sc], 0x54\n\t"
        "l32i    a8, %[mid], 24\n\t"  "s32i    a8, %[sc], 0x58\n\t"
        "l32i    a8, %[mid], 28\n\t"  "s32i    a8, %[sc], 0x5C\n\t"
        //Bloc 2 : en-tete, nonce, puis le padding que inter() avait remplace.
        "l32i    a8, %[in], 0\n\t"    "s32i    a8, %[sc], 0x80\n\t"
        "l32i    a8, %[in], 4\n\t"    "s32i    a8, %[sc], 0x84\n\t"
        "l32i    a8, %[in], 8\n\t"    "s32i    a8, %[sc], 0x88\n\t"
        "s32i    %[nonce], %[sc], 0x8C\n\t"
        "movi    a8, 0x80\n\t"        "s32i    a8, %[sc], 0x90\n\t"
        "movi.n  a9, 0\n\t"
        "s32i    a9, %[sc], 0x94\n\t" "s32i    a9, %[sc], 0x98\n\t"
        "s32i    a9, %[sc], 0x9C\n\t" "s32i    a9, %[sc], 0xA0\n\t"
        "movi    a8, 0x80020000\n\t"  "s32i    a8, %[sc], 0xBC\n\t"
        "movi.n  a8, 1\n\t"           "s32i    a8, %[sc], 0x14\n\t"
        "1: l32i a8, %[sc], 0x18\n\t" "bnez.n  a8, 1b\n\t"
        //Second sha : le digest passe de H vers TEXT, puis son padding.
        "l32i    a8, %[sc], 0x40\n\t"  "s32i    a8, %[sc], 0x80\n\t"
        "l32i    a8, %[sc], 0x44\n\t"  "s32i    a8, %[sc], 0x84\n\t"
        "l32i    a8, %[sc], 0x48\n\t"  "s32i    a8, %[sc], 0x88\n\t"
        "l32i    a8, %[sc], 0x4C\n\t"  "s32i    a8, %[sc], 0x8C\n\t"
        "l32i    a8, %[sc], 0x50\n\t"  "s32i    a8, %[sc], 0x90\n\t"
        "l32i    a8, %[sc], 0x54\n\t"  "s32i    a8, %[sc], 0x94\n\t"
        "l32i    a8, %[sc], 0x58\n\t"  "s32i    a8, %[sc], 0x98\n\t"
        "l32i    a8, %[sc], 0x5C\n\t"  "s32i    a8, %[sc], 0x9C\n\t"
        "movi    a8, 0x80\n\t"        "s32i    a8, %[sc], 0xA0\n\t"
        "movi    a8, 0x00010000\n\t"  "s32i    a8, %[sc], 0xBC\n\t"
        "movi.n  a8, 1\n\t"           "s32i    a8, %[sc], 0x10\n\t"
        "2: l32i a8, %[sc], 0x18\n\t" "bnez.n  a8, 2b\n\t"
        //Rejet precoce : candidat quand les 16 bits de poids fort de H[7] sont nuls.
        "l32i    a8, %[sc], 0x5C\n\t"
        "extui   a8, a8, 16, 16\n\t"
        "addi    %[nonce], %[nonce], 1\n\t"
        "addi    %[cnt], %[cnt], -1\n\t"
        "beqz    a8, 9f\n\t"
        "bnez    %[cnt], 0b\n\t"
    "9:\n\t"
        : [cnt] "+r" (remaining), [nonce] "+r" (nonce)
        : [sc] "r" ((uint32_t *)(SHA_TEXT_BASE - 0x80)), [in] "r" (in), [mid] "r" (mid)
        : "a8", "a9", "memory");
    *nonce_io = nonce;
    return remaining;
}
#endif

//RACE_KAT: le test a reponse connue coute environ 1,2 % sur S3, non par son
//execution (une fois) mais par la disposition du code generee autour. Sur ce chemin
//il fait double emploi : la validation logicielle tourne deja sur chaque candidat,
//environ 5 fois par seconde, et la comptabilite de la pool sert de troisieme filet.
//On le garde compilable pour s'en servir des qu'on touche a la boucle chaude, mais
//il n'est pas actif en production S3. Sur classic il est actif : c'est le seul
//controle disponible au demarrage.
#if defined(VALIDATION) && RACE_KAT
//Test a reponse connue joue une fois au demarrage, sur la sequence S3 reellement
//compilee : bloc 125552, en-tete et nonce publics. Le resultat materiel est compare
//a la reference logicielle, pas a une constante recopiee du materiel, sinon le test
//ne validerait que lui-meme. Verdict remonte dans la telemetrie (race_kat_state) :
//les six S3 sont au rack, sans port serie.
static void __attribute__((noinline)) nerd_s3_kat(void)
{
  static const uint8_t kat[80] = {
    0x01,0x00,0x00,0x00,0x81,0xcd,0x02,0xab,0x7e,0x56,0x9e,0x8b,0xcd,0x93,0x17,0xe2,
    0xfe,0x99,0xf2,0xde,0x44,0xd4,0x9a,0xb2,0xb8,0x85,0x1b,0xa4,0xa3,0x08,0x00,0x00,
    0x00,0x00,0x00,0x00,0xe3,0x20,0xb6,0xc2,0xff,0xfc,0x8d,0x75,0x04,0x23,0xdb,0x8b,
    0x1e,0xb9,0x42,0xae,0x71,0x0e,0x95,0x1e,0xd7,0x97,0xf7,0xaf,0xfc,0x88,0x92,0xb0,
    0xf1,0xfc,0x12,0x2b,0xc7,0xf5,0xd7,0x4d,0xf2,0xb9,0x44,0x1a,0x42,0xa1,0x46,0x95 };
  uint8_t hdr[80] __attribute__((aligned(4)));
  memcpy(hdr, kat, sizeof(hdr));   //depuis la RAM, comme en production
  const uint32_t nonce = 0x9546a142;

  uint32_t hw_mid[8];
  sha_hal_hash_block(SHA2_256, hdr, 64/4, true);
  sha_hal_read_digest(SHA2_256, hw_mid);
  REG_WRITE(SHA_MODE_REG, SHA2_256);
  //Le remplissage rapide saute les mots 9 a 14 en supposant qu'ils sont nuls : la
  //production les met a zero une fois par job, il faut faire pareil ici. Sans ca le
  //calcul du midstate laisse des mots d'en-tete a leur place et le test echoue alors
  //que la production est correcte. Meme classe de defaut que le padding du 7 aout.
  {
    uint32_t *tb = (uint32_t *)(SHA_TEXT_BASE);
    for (int i = 9; i <= 14; ++i) REG_WRITE(&tb[i], 0x00000000);
  }

  uint8_t hash[32];
#if RACE_ASM_LOOP_S3
  { uint32_t nn = nonce; nerd_sha_s3_run_asm(hdr+64, hw_mid, &nn, 1); }
#else
  nerd_sha_ll_write_digest(hw_mid);
  nerd_sha_ll_fill_text_block_sha256_fast(hdr+64, nonce);
  REG_WRITE(SHA_CONTINUE_REG, 1);
  nerd_sha_hal_wait_idle();
  nerd_sha_ll_fill_text_block_sha256_inter();
  REG_WRITE(SHA_START_REG, 1);
  nerd_sha_hal_wait_idle();
#endif

  //Le point d'echec est encode dans la valeur remontee : les S3 sont au rack, sans
  //port serie, et un verdict binaire ne dit pas ou chercher.
  //1 succes, 2 le materiel n'a pas rendu de digest filtre, 3 la reference logicielle
  //a refuse, 4 les deux ne concordent pas.
  int8_t st;
  if (!nerd_sha_ll_read_digest_if(hash)) {
    st = 2;
  } else {
    uint32_t mids[8], bake[16];
    uint8_t sw[32];
    nerd_mids(mids, hdr);
    nerd_sha256_bake(mids, hdr+64, bake);
    if (!nerd_sha256d_baked(mids, hdr+64, bake, sw)) {
      st = 3;
    } else {
      st = 1;
      for (int i = 0; i < 32; ++i)
        if (hash[i] != sw[i]) { st = 4; break; }
    }
  }
  race_kat_state = st;
  Serial.printf("KAT: etat %d (bloc 125552)\n", (int)st);
}
#endif

void minerWorkerHw(void * task_id)
{
  unsigned int miner_id = (uint32_t)task_id;
  Serial.printf("[MINER] %d Started minerWorkerHw Task on core %d!\n", miner_id, xPortGetCoreID());

  std::shared_ptr<JobRequest> job;
  std::shared_ptr<JobResult> result;
  uint8_t interResult[64];
  uint8_t hash[32];
  uint8_t digest_mid[32];
  uint8_t sha_buffer[64];
  uint32_t wdt_counter = 0;

#ifdef VALIDATION
  uint8_t doubleHash[32];
  uint32_t diget_mid[8];
  uint32_t bake[16];
#endif

  while (1)
  {
    //Idle during OTA at a safe point: never suspended while holding the SHA
    //engine lock, so Update.end()'s image SHA-256 verification can proceed.
    if (ota_active) { vTaskDelay(100 / portTICK_PERIOD_MS); continue; }
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      if (result)
      {
        race_hashes_hw += result->nonce_count;
        if (s_job_result_list.size() < 16)
          s_job_result_list.push_back(result);
        result.reset();
      }
      if (!s_job_request_list_hw.empty())
      {
        job = s_job_request_list_hw.front();
        s_job_request_list_hw.pop_front();
      } else
        job.reset();
    }
    if (job)
    {
      result = std::make_shared<JobResult>();
      result->id = job->id;
      result->nonce = 0xFFFFFFFF;
      result->nonce_count = job->nonce_count;
      result->difficulty = job->difficulty;
      uint8_t job_in_work = job->id & 0xFF;
      memcpy(digest_mid, job->midstate, sizeof(digest_mid));
      memcpy(sha_buffer, job->sha_buffer+64, sizeof(sha_buffer));
#ifdef VALIDATION
      nerd_mids(diget_mid, job->sha_buffer);
      nerd_sha256_bake(diget_mid, job->sha_buffer+64, bake);
#endif

      esp_sha_acquire_hardware();
#if defined(VALIDATION) && RACE_KAT
      { static bool kat_done = false; if (!kat_done) { kat_done = true; nerd_s3_kat(); } }
#endif
      REG_WRITE(SHA_MODE_REG, SHA2_256);
      {
        //Zero SHA_TEXT[9..14] once per job so the per-nonce fast fill can skip them
        //(they stay 0: the inter block writes 0 there and the engine doesn't clobber them)
        uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);
        for (int i = 9; i <= 14; ++i)
          REG_WRITE(&reg_addr_buf[i], 0x00000000);
      }
#if RACE_ASM_LOOP_S3
      //Meme decoupage qu'ailleurs : la boucle vit dans l'assembleur, le C ne revient
      //que sur un candidat ou toutes les 256 iterations pour voir si le job a change.
      {
        uint32_t nonce = job->nonce_start;
        uint32_t done = 0;
        while (done < job->nonce_count)
        {
          uint32_t chunk = job->nonce_count - done;
          if (chunk > 256) chunk = 256;
          done += chunk - nerd_sha_s3_run_asm(sha_buffer, (const uint32_t*)digest_mid, &nonce, chunk);
          if (nerd_sha_ll_read_digest_if(hash))
          {
            const uint32_t nonce_hit = job->nonce_start + done - 1;
#ifdef VALIDATION
            ((uint32_t*)(job->sha_buffer+64+12))[0] = nonce_hit;
            nerd_sha256d_baked(diget_mid, job->sha_buffer+64, bake, doubleHash);
            for (int i = 0; i < 32; ++i)
              if (hash[i] != doubleHash[i]) { race_sha_mismatch++; break; }
#endif
            double diff_hash = diff_from_target(hash);
            if (diff_hash > result->difficulty && isSha256Valid(hash))
            {
              result->difficulty = diff_hash;
              result->nonce = nonce_hit;
              memcpy(result->hash, hash, sizeof(hash));
            }
          }
          if (s_working_current_job_id != job_in_work) break;
        }
        result->nonce_count = done;
      }
#else
      uint32_t nend = job->nonce_start + job->nonce_count;
      for (uint32_t n = job->nonce_start; n < nend; ++n)
      {
#if RACE_BENCH
        uint32_t c0 = RACE_CC();
#endif
        //nerd_sha_hal_wait_idle();
        nerd_sha_ll_write_digest(digest_mid);
#if RACE_BENCH
        uint32_t c1 = RACE_CC();
#endif
        //nerd_sha_hal_wait_idle();
        nerd_sha_ll_fill_text_block_sha256_fast(sha_buffer, n);
        //sha_ll_load() is an EMPTY inline on S3 (hal/esp32s3/sha_ll.h)
        //— the calls that used to sit here were pure noise. SHA_MODE_REG is set
        //once per job, so writing the CONTINUE/START trigger is the whole op.
        REG_WRITE(SHA_CONTINUE_REG, 1);
#if RACE_BENCH
        uint32_t c2 = RACE_CC();
#endif
        nerd_sha_hal_wait_idle();
#if RACE_BENCH
        uint32_t c3 = RACE_CC();
#endif
        nerd_sha_ll_fill_text_block_sha256_inter();
        REG_WRITE(SHA_START_REG, 1);
#if RACE_BENCH
        uint32_t c4 = RACE_CC();
#endif
        nerd_sha_hal_wait_idle();
#if RACE_BENCH
        uint32_t c5 = RACE_CC();
#endif
        if (__builtin_expect(nerd_sha_ll_read_digest_if(hash), 0))
        {
          //Serial.printf("Hw 16bit Share, nonce=0x%X\n", n);
#ifdef VALIDATION
          //Validation
          ((uint32_t*)(job->sha_buffer+64+12))[0] = n;
          nerd_sha256d_baked(diget_mid, job->sha_buffer+64, bake, doubleHash);
          for (int i = 0; i < 32; ++i)
          {
            if (hash[i] != doubleHash[i])
            {
              Serial.println("***HW sha256 esp32s3 bug detected***");
              race_sha_mismatch++;
              break;
            }
          }
#endif
          //~5 per second
          double diff_hash = diff_from_target(hash);
          if (diff_hash > result->difficulty)
          {
            if (isSha256Valid(hash))
            {
              result->difficulty = diff_hash;
              result->nonce = n;
              memcpy(result->hash, hash, sizeof(hash));
            }
          }
        }
#if RACE_BENCH
        uint32_t c6 = RACE_CC();
        uint32_t tot_n = c6 - c0;
        if (tot_n < RACE_SANE_MAX) {   //drop preempted samples
          s_race_acc.mid   += c1 - c0;
          s_race_acc.fill  += c2 - c1;
          s_race_acc.w1    += c3 - c2;
          s_race_acc.inter += c4 - c3;
          s_race_acc.w2    += c5 - c4;
          s_race_acc.chk   += c6 - c5;
          s_race_acc.tot   += tot_n;
          s_race_acc.nonces++;
        } else s_race_acc.dropped++;
#endif
        if (
             (uint8_t)(n & 0xFF) == 0 &&
             s_working_current_job_id != job_in_work)
        {
          result->nonce_count = n-job->nonce_start+1;
          break;
        }
      }
#endif
#if RACE_BENCH
      if (++s_race_acc.jobs >= RACE_BENCH_JOBS) {
        uint32_t nn = s_race_acc.nonces ? s_race_acc.nonces : 1;
        Serial.printf("Bench: cyc/nonce tot=%u mid=%u fill=%u w1=%u inter=%u w2=%u chk=%u (n=%u)\n",
          (unsigned)(s_race_acc.tot/nn), (unsigned)(s_race_acc.mid/nn),
          (unsigned)(s_race_acc.fill/nn), (unsigned)(s_race_acc.w1/nn),
          (unsigned)(s_race_acc.inter/nn), (unsigned)(s_race_acc.w2/nn),
          (unsigned)(s_race_acc.chk/nn), (unsigned)nn);
        Serial.printf("Bench: dropped=%u (preempted)\n", (unsigned)s_race_acc.dropped);
        s_race_acc = {};
      }
#endif
#if RACE_RATE
      if (++s_race_rate_jobs >= 256) {
        s_race_rate_jobs = 0;
        uint32_t now_ms = millis();
        uint32_t hw_now = race_hashes_hw, sw_now = race_hashes_sw;
        if (s_race_rate_ms) {
          uint32_t dt = now_ms - s_race_rate_ms;
          if (dt > 0)
          {
            //Compare our per-path counters with the counter the telemetry uses
            //(Mhashes/hashes, fed by runMiner when it harvests results): a gap
            //means work is done but never harvested.
            static uint64_t s_prev_official = 0;
            uint64_t official = (uint64_t)Mhashes * 1000000ULL + hashes;
            double khs_off = s_prev_official ? (double)(official - s_prev_official) / dt : 0.0;
            s_prev_official = official;
                  race_khs_hw = (float)((double)(hw_now - s_race_rate_hw) / dt);
      race_khs_sw = (float)((double)(sw_now - s_race_rate_sw) / dt);
      Serial.printf("Rate: khs_hw=%.1f khs_sw=%.1f total=%.1f official=%.1f mismatch=%u starvedHw=%u\n",
              (double)(hw_now - s_race_rate_hw) / dt, (double)(sw_now - s_race_rate_sw) / dt,
              (double)((hw_now - s_race_rate_hw) + (sw_now - s_race_rate_sw)) / dt,
              khs_off, (unsigned)race_sha_mismatch, (unsigned)race_starved_hw);
          }
        }
        s_race_rate_ms = now_ms; s_race_rate_hw = hw_now; s_race_rate_sw = sw_now;
      }
#endif
      esp_sha_release_hardware();
    } else
      vTaskDelay(2 / portTICK_PERIOD_MS);

    wdt_counter++;
    if (wdt_counter >= 8)
    {
      wdt_counter = 0;
      esp_task_wdt_reset();
    }
  }
}

#endif  //#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3)

#if defined(CONFIG_IDF_TARGET_ESP32)

//RACE_SHA_DIRECT_READ: lire SHA_TEXT sans la sequence DPORT.
//La sequence protege d'un bug de concurrence entre les deux coeurs sur le bus
//DPORT, et chaque DPORT_SEQUENCE_REG_READ est un appel de fonction (call8) : 8 par
//nonce rien que pour lire le digest. Ici le second coeur fait du SHA logiciel et ne
//touche jamais ces registres, et les interruptions sont deja masquees, donc la
//lecture directe est sure. VALIDATION verifie chaque hash pendant les essais.
#if RACE_SHA_DIRECT_READ
#define SHA_READ(addr) _DPORT_REG_READ(addr)
#else
#define SHA_READ(addr) DPORT_SEQUENCE_REG_READ(addr)
#endif

//Le masquage d'interruptions n'existe que pour la sequence DPORT : elle lit deux
//registres et exige que rien ne s'intercale. En lecture brute il ne protege plus rien
//et coute deux acces registre par nonce, sur le chemin le plus chaud du firmware.
#if RACE_SHA_DIRECT_READ
#define SHA_RD_ENTER() do {} while (0)
#define SHA_RD_LEAVE() do {} while (0)
#else
#define SHA_RD_ENTER() DPORT_INTERRUPT_DISABLE()
#define SHA_RD_LEAVE() DPORT_INTERRUPT_RESTORE()
#endif

static inline bool nerd_sha_ll_read_digest_swap_if(void* ptr)
{
  SHA_RD_ENTER();
  uint32_t fin = SHA_READ(SHA_TEXT_BASE + 7 * 4);
  if ( (uint32_t)(fin & 0xFFFF) != 0)
  {
    SHA_RD_LEAVE();
    return false;
  }
  ((uint32_t*)ptr)[7] = __builtin_bswap32(fin);
  ((uint32_t*)ptr)[0] = __builtin_bswap32(SHA_READ(SHA_TEXT_BASE + 0 * 4));
  ((uint32_t*)ptr)[1] = __builtin_bswap32(SHA_READ(SHA_TEXT_BASE + 1 * 4));
  ((uint32_t*)ptr)[2] = __builtin_bswap32(SHA_READ(SHA_TEXT_BASE + 2 * 4));
  ((uint32_t*)ptr)[3] = __builtin_bswap32(SHA_READ(SHA_TEXT_BASE + 3 * 4));
  ((uint32_t*)ptr)[4] = __builtin_bswap32(SHA_READ(SHA_TEXT_BASE + 4 * 4));
  ((uint32_t*)ptr)[5] = __builtin_bswap32(SHA_READ(SHA_TEXT_BASE + 5 * 4));
  ((uint32_t*)ptr)[6] = __builtin_bswap32(SHA_READ(SHA_TEXT_BASE + 6 * 4));
  SHA_RD_LEAVE();
  return true;
}

static inline void nerd_sha_ll_read_digest(void* ptr)
{
  SHA_RD_ENTER();
  ((uint32_t*)ptr)[0] = SHA_READ(SHA_TEXT_BASE + 0 * 4);
  ((uint32_t*)ptr)[1] = SHA_READ(SHA_TEXT_BASE + 1 * 4);
  ((uint32_t*)ptr)[2] = SHA_READ(SHA_TEXT_BASE + 2 * 4);
  ((uint32_t*)ptr)[3] = SHA_READ(SHA_TEXT_BASE + 3 * 4);
  ((uint32_t*)ptr)[4] = SHA_READ(SHA_TEXT_BASE + 4 * 4);
  ((uint32_t*)ptr)[5] = SHA_READ(SHA_TEXT_BASE + 5 * 4);
  ((uint32_t*)ptr)[6] = SHA_READ(SHA_TEXT_BASE + 6 * 4);
  ((uint32_t*)ptr)[7] = SHA_READ(SHA_TEXT_BASE + 7 * 4);
  SHA_RD_LEAVE();
}

//DPORT_REG_READ se resout en appel de fonction (esp_dport_access_sequence_reg_read)
//quand le contournement DPORT est actif. Dans une boucle de polling, cela coute un
//call8 avec rotation de fenetre a chaque tour, pour lire un simple registre d'etat.
//RACE_SHA_DIRECT_READ passe en lecture brute. Le second coeur fait du SHA logiciel
//et ne touche jamais ces registres ; VALIDATION verifie chaque hash pendant les essais.
#if RACE_CLASSIC_BENCH
static struct { uint32_t total, wait, n; } s_cbench;
#endif

//Chaque tour de polling lit SHA_BUSY sur le bus APB, ce qui coute bien plus qu'un
//cycle. Le moteur prend environ 91 cycles par bloc (mesure RACE_CLASSIC_BENCH), donc
//attendre d'abord en nop puis poller supprime plusieurs lectures inutiles. C'est ce
//que fait le firmware NMMiner sur cette puce. RACE_NOP_WAIT est le nombre de nop.
//Sous-estimer est sans danger : le poll qui suit rattrape.
#if RACE_NOP_WAIT
//Nops DEROULES, sans boucle : une boucle for coute environ 4 cycles par nop
//(increment, test, saut) et masque completement ce qu'on cherche a mesurer.
#define NOP8   "nop;nop;nop;nop;nop;nop;nop;nop\n\t"
static inline void nerd_sha_nop_delay(void)
{
#if   RACE_NOP_WAIT == 16
    __asm__ __volatile__(NOP8 NOP8);
#elif RACE_NOP_WAIT == 32
    __asm__ __volatile__(NOP8 NOP8 NOP8 NOP8);
#elif RACE_NOP_WAIT == 48
    __asm__ __volatile__(NOP8 NOP8 NOP8 NOP8 NOP8 NOP8);
#else
    __asm__ __volatile__(NOP8 NOP8 NOP8 NOP8 NOP8 NOP8 NOP8 NOP8);
#endif
}
#endif

static inline void nerd_sha_hal_wait_idle()
{
#if RACE_NOP_WAIT
    nerd_sha_nop_delay();
#endif
#if RACE_ASM_FILL
    //BUSY est a SHA_TEXT_BASE+0x9C. La boucle garde l'adresse dans un registre au
    //lieu de la rematerialiser, et le poll tient en deux instructions.
    __asm__ __volatile__(
        "1: l32i  a8, %0, 0x9C\n\t"
        "   bnez  a8, 1b\n\t"
        : : "r"((uint32_t *)(SHA_TEXT_BASE)) : "a8", "memory");
#elif RACE_SHA_DIRECT_READ
    while (_DPORT_REG_READ(SHA_256_BUSY_REG))
    {}
#else
    while (DPORT_REG_READ(SHA_256_BUSY_REG))
    {}
#endif
}

#if RACE_ASM_FILL
//Note d'ordonnancement : aucune barriere memw ici. Une ecriture de commande est
//postee, donc en theorie la lecture de BUSY qui suit peut la doubler et voir le
//moteur encore au repos. En pratique les instructions de raccord entre blocs asm
//suffisent, ce que verifie le test a reponse connue joue au demarrage. Mettre un
//memw apres chaque commande rend l'ordre explicite mais coute 4 % (643 -> 618 kH/s).
//Si le KAT echoue apres un changement de compilateur, c'est la premiere piste.
//gcc materialise l'adresse absolue de chaque mot avec un l32r, soit trois
//instructions par ecriture. Ici la base est chargee une fois et les seize mots
//partent en offsets immediats, comme le fait SparkMiner.
static inline void nerd_sha_ll_fill_text_block_sha256(const void *input_text)
{
    __asm__ __volatile__(
        "l32i.n  a8,  %0, 0\n\t"   "s32i.n  a8,  %1, 0\n\t"
        "l32i.n  a9,  %0, 4\n\t"   "s32i.n  a9,  %1, 4\n\t"
        "l32i.n  a10, %0, 8\n\t"   "s32i.n  a10, %1, 8\n\t"
        "l32i.n  a11, %0, 12\n\t"  "s32i.n  a11, %1, 12\n\t"
        "l32i.n  a8,  %0, 16\n\t"  "s32i.n  a8,  %1, 16\n\t"
        "l32i.n  a9,  %0, 20\n\t"  "s32i.n  a9,  %1, 20\n\t"
        "l32i.n  a10, %0, 24\n\t"  "s32i.n  a10, %1, 24\n\t"
        "l32i.n  a11, %0, 28\n\t"  "s32i.n  a11, %1, 28\n\t"
        "l32i.n  a8,  %0, 32\n\t"  "s32i.n  a8,  %1, 32\n\t"
        "l32i.n  a9,  %0, 36\n\t"  "s32i.n  a9,  %1, 36\n\t"
        "l32i.n  a10, %0, 40\n\t"  "s32i.n  a10, %1, 40\n\t"
        "l32i.n  a11, %0, 44\n\t"  "s32i.n  a11, %1, 44\n\t"
        "l32i.n  a8,  %0, 48\n\t"  "s32i.n  a8,  %1, 48\n\t"
        "l32i.n  a9,  %0, 52\n\t"  "s32i.n  a9,  %1, 52\n\t"
        "l32i.n  a10, %0, 56\n\t"  "s32i.n  a10, %1, 56\n\t"
        "l32i.n  a11, %0, 60\n\t"  "s32i.n  a11, %1, 60\n\t"
        //START est a SHA_TEXT_BASE+0x90, donc atteignable depuis la base deja en
        //registre : cela evite le l32r que sha_ll_start_block refait a chaque appel.
        "movi.n  a8, 1\n\t"        "s32i    a8,  %1, 0x90\n\t"
        :
        : "r"(input_text), "r"((uint32_t *)(SHA_TEXT_BASE))
        : "a8", "a9", "a10", "a11", "memory");
}
#else
static inline void nerd_sha_ll_fill_text_block_sha256(const void *input_text)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    reg_addr_buf[0]  = data_words[0];
    reg_addr_buf[1]  = data_words[1];
    reg_addr_buf[2]  = data_words[2];
    reg_addr_buf[3]  = data_words[3];
    reg_addr_buf[4]  = data_words[4];
    reg_addr_buf[5]  = data_words[5];
    reg_addr_buf[6]  = data_words[6];
    reg_addr_buf[7]  = data_words[7];
    reg_addr_buf[8]  = data_words[8];
    reg_addr_buf[9]  = data_words[9];
    reg_addr_buf[10] = data_words[10];
    reg_addr_buf[11] = data_words[11];
    reg_addr_buf[12] = data_words[12];
    reg_addr_buf[13] = data_words[13];
    reg_addr_buf[14] = data_words[14];
    reg_addr_buf[15] = data_words[15];
}
#endif

static inline void nerd_sha_ll_fill_text_block_sha256_upper(const void *input_text, uint32_t nonce)
{
    uint32_t *data_words = (uint32_t *)input_text;
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

    reg_addr_buf[0]  = data_words[0];
    reg_addr_buf[1]  = data_words[1];
    reg_addr_buf[2]  = data_words[2];
    reg_addr_buf[3]  = __builtin_bswap32(nonce);
#if 1
    reg_addr_buf[4]  = 0x80000000;
    //5..7 are overwritten by the digest on the LOAD that follows, so they must be
    //rewritten here. 9..14 stay zero for the whole job: both this padding and the
    //one in _double() put zeros there, and the engine never touches them. They are
    //zeroed once in minerWorkerHw instead, saving 6 APB writes per nonce here and
    //6 more in _double(). Same trick the S3 path already uses.
    reg_addr_buf[5]  = 0x00000000;
    reg_addr_buf[6]  = 0x00000000;
    reg_addr_buf[7]  = 0x00000000;
    reg_addr_buf[8]  = 0x00000000;
    //9..14 DOIVENT etre reecrits ici : le remplissage du bloc 1 ecrit les seize mots,
    //donc il y laisse des mots d'en-tete. Les mettre a zero une fois par job ne suffit
    //que pour le bloc 3 (le LOAD n'ecrit que 0..7), pas pour le bloc 2.
    reg_addr_buf[9]  = 0x00000000;
    reg_addr_buf[10] = 0x00000000;
    reg_addr_buf[11] = 0x00000000;
    reg_addr_buf[12] = 0x00000000;
    reg_addr_buf[13] = 0x00000000;
    reg_addr_buf[14] = 0x00000000;
    reg_addr_buf[15] = 0x00000280;
#else
    reg_addr_buf[4]  = data_words[4];
    reg_addr_buf[5]  = data_words[5];
    reg_addr_buf[6]  = data_words[6];
    reg_addr_buf[7]  = data_words[7];
    reg_addr_buf[8]  = data_words[8];
    reg_addr_buf[9]  = data_words[9];
    reg_addr_buf[10] = data_words[10];
    reg_addr_buf[11] = data_words[11];
    reg_addr_buf[12] = data_words[12];
    reg_addr_buf[13] = data_words[13];
    reg_addr_buf[14] = data_words[14];
    reg_addr_buf[15] = data_words[15];
#endif
}

#if RACE_PREFILL
//Le contenu du bloc 2 ne depend pas du resultat du bloc 1 : c'est l'en-tete plus le
//nonce, connus avant meme le START. On l'ecrit donc PENDANT que le moteur calcule le
//bloc 1, pour cacher dix ecritures APB (~30 cycles) dans les ~91 cycles d'attente.
//Cela n'est correct que si le moteur a deja consomme le message au START. Si ce n'est
//pas le cas il hache un message ecrase et VALIDATION le dit immediatement.
//Le CONTINUE reste apres l'attente, lui : il ne doit partir que moteur au repos.
static inline void nerd_sha_ll_fill_text_block_sha256_upper_nocont(const void *input_text, uint32_t nonce)
{
    const uint32_t be_nonce = __builtin_bswap32(nonce);
    __asm__ __volatile__(
        "l32i.n  a8,  %0, 0\n\t"      "s32i.n  a8,  %1, 0\n\t"
        "l32i.n  a9,  %0, 4\n\t"      "s32i.n  a9,  %1, 4\n\t"
        "l32i.n  a10, %0, 8\n\t"      "s32i.n  a10, %1, 8\n\t"
        "s32i.n  %2,  %1, 12\n\t"
        "movi    a11, 0x80000000\n\t" "s32i.n  a11, %1, 16\n\t"
        "movi.n  a8,  0\n\t"
        "s32i.n  a8,  %1, 20\n\t"     "s32i.n  a8,  %1, 24\n\t"
        "s32i.n  a8,  %1, 28\n\t"     "s32i.n  a8,  %1, 32\n\t"
        "s32i.n  a8,  %1, 36\n\t"     "s32i.n  a8,  %1, 40\n\t"
        "s32i.n  a8,  %1, 44\n\t"     "s32i.n  a8,  %1, 48\n\t"
        "s32i.n  a8,  %1, 52\n\t"     "s32i.n  a8,  %1, 56\n\t"
        "movi    a9,  0x280\n\t"      "s32i.n  a9,  %1, 60\n\t"
        :
        : "r"(input_text), "r"((uint32_t *)(SHA_TEXT_BASE)), "r"(be_nonce)
        : "a8", "a9", "a10", "a11", "memory");
}

static inline void nerd_sha_continue_asm(void)
{
    __asm__ __volatile__(
        "movi.n  a8, 1\n\t"  "s32i  a8, %0, 0x94\n\t"
        : : "r"((uint32_t *)(SHA_TEXT_BASE)) : "a8", "memory");
}

//Padding du second sha, sans le START. TEXT[8] et TEXT[15] ne sont touches ni par le
//LOAD (qui n'ecrit que TEXT[0..7]) ni par le moteur, donc ces deux mots peuvent partir
//en avance eux aussi.
static inline void nerd_sha_ll_fill_double_nostart(void)
{
    __asm__ __volatile__(
        "movi    a8, 0x80000000\n\t"  "s32i.n  a8, %0, 32\n\t"
        "movi    a9, 0x100\n\t"       "s32i.n  a9, %0, 60\n\t"
        : : "r"((uint32_t *)(SHA_TEXT_BASE)) : "a8", "a9", "memory");
}

#if RACE_CLASSIC_BENCH
#define RACE_WAITB() { uint32_t a_=RACE_CC(); nerd_sha_hal_wait_idle(); s_cbench.wait += RACE_CC()-a_; }
#else
#define RACE_WAITB() nerd_sha_hal_wait_idle()
#endif

static inline void nerd_sha_start_asm(void)
{
    __asm__ __volatile__(
        "movi.n  a8, 1\n\t"  "s32i  a8, %0, 0x90\n\t"
        : : "r"((uint32_t *)(SHA_TEXT_BASE)) : "a8", "memory");
}
#endif

#if RACE_ASM_FILL
//Bloc 2, meme principe que le bloc 1 : une seule base, offsets immediats.
static inline void nerd_sha_ll_fill_text_block_sha256_upper_asm(const void *input_text, uint32_t nonce)
{
    const uint32_t be_nonce = __builtin_bswap32(nonce);
    __asm__ __volatile__(
        "l32i.n  a8,  %0, 0\n\t"      "s32i.n  a8,  %1, 0\n\t"
        "l32i.n  a9,  %0, 4\n\t"      "s32i.n  a9,  %1, 4\n\t"
        "l32i.n  a10, %0, 8\n\t"      "s32i.n  a10, %1, 8\n\t"
        "s32i.n  %2,  %1, 12\n\t"
        "movi    a11, 0x80000000\n\t" "s32i.n  a11, %1, 16\n\t"
        "movi.n  a8,  0\n\t"
        "s32i.n  a8,  %1, 20\n\t"     "s32i.n  a8,  %1, 24\n\t"
        "s32i.n  a8,  %1, 28\n\t"     "s32i.n  a8,  %1, 32\n\t"
        "s32i.n  a8,  %1, 36\n\t"     "s32i.n  a8,  %1, 40\n\t"
        "s32i.n  a8,  %1, 44\n\t"     "s32i.n  a8,  %1, 48\n\t"
        "s32i.n  a8,  %1, 52\n\t"     "s32i.n  a8,  %1, 56\n\t"
        "movi    a9,  0x280\n\t"      "s32i.n  a9,  %1, 60\n\t"
        "movi.n  a8, 1\n\t"          "s32i    a8,  %1, 0x94\n\t"   //CONTINUE
        :
        : "r"(input_text), "r"((uint32_t *)(SHA_TEXT_BASE)), "r"(be_nonce)
        : "a8", "a9", "a10", "a11", "memory");
}
#endif

#if RACE_ASM_FILL
//LOAD depuis la base : sha_ll_load() recharge l'adresse a chaque appel, et il y en a
//deux par nonce.
static inline void nerd_sha_ll_load_asm(void)
{
    __asm__ __volatile__(
        "movi.n  a8, 1\n\t"  "s32i  a8, %0, 0x98\n\t"
        : : "r"((uint32_t *)(SHA_TEXT_BASE)) : "a8", "memory");
}
#endif

#if RACE_ASM_FILL
//Padding du second sha256 puis START, depuis la meme base.
static inline void nerd_sha_ll_fill_text_block_sha256_double_asm(void)
{
    __asm__ __volatile__(
        "movi    a8, 0x80000000\n\t"  "s32i.n  a8, %0, 32\n\t"
        "movi    a9, 0x100\n\t"       "s32i.n  a9, %0, 60\n\t"
        "movi.n  a8, 1\n\t"           "s32i    a8, %0, 0x90\n\t"   //START
        :
        : "r"((uint32_t *)(SHA_TEXT_BASE))
        : "a8", "a9", "memory");
}
#endif

static inline void nerd_sha_ll_fill_text_block_sha256_double()
{
    uint32_t *reg_addr_buf = (uint32_t *)(SHA_TEXT_BASE);

#if 0
    //No change
    reg_addr_buf[0]  = data_words[0];
    reg_addr_buf[1]  = data_words[1];
    reg_addr_buf[2]  = data_words[2];
    reg_addr_buf[3]  = data_words[3];
    reg_addr_buf[4]  = data_words[4];
    reg_addr_buf[5]  = data_words[5];
    reg_addr_buf[6]  = data_words[6];
    reg_addr_buf[7]  = data_words[7];
#endif
    reg_addr_buf[8]  = 0x80000000;
    //9..14 kept at zero for the whole job, see _upper()
    reg_addr_buf[15] = 0x00000100;
}

#if RACE_ASM_NONCE
//Tout le corps d'un nonce en un seul bloc assembleur. Decoupe en cinq blocs separes,
//gcc rematerialise la base SHA_TEXT a chaque bloc et repasse par du C entre eux ;
//ici la base est chargee une fois, les registres de commande sont atteints par une
//seconde base (base+0x90) qui autorise la forme etroite de s32i, et chaque attente
//tient en deux instructions. C'est la structure de SparkMiner.
//Retourne SHA_TEXT[7] brut : le C ne relit le digest complet que sur un candidat.
static inline uint32_t nerd_sha_nonce_asm(const void *in, uint32_t be_nonce)
{
    uint32_t fin;
    __asm__ __volatile__(
        "l32i.n  a8,  %[in], 0\n\t"  "s32i.n  a8,  %[sb], 0\n\t"
        "l32i.n  a8,  %[in], 4\n\t"  "s32i.n  a8,  %[sb], 4\n\t"
        "l32i.n  a8,  %[in], 8\n\t"  "s32i.n  a8,  %[sb], 8\n\t"
        "l32i.n  a8,  %[in], 12\n\t"  "s32i.n  a8,  %[sb], 12\n\t"
        "l32i.n  a8,  %[in], 16\n\t"  "s32i.n  a8,  %[sb], 16\n\t"
        "l32i.n  a8,  %[in], 20\n\t"  "s32i.n  a8,  %[sb], 20\n\t"
        "l32i.n  a8,  %[in], 24\n\t"  "s32i.n  a8,  %[sb], 24\n\t"
        "l32i.n  a8,  %[in], 28\n\t"  "s32i.n  a8,  %[sb], 28\n\t"
        "l32i.n  a8,  %[in], 32\n\t"  "s32i.n  a8,  %[sb], 32\n\t"
        "l32i.n  a8,  %[in], 36\n\t"  "s32i.n  a8,  %[sb], 36\n\t"
        "l32i.n  a8,  %[in], 40\n\t"  "s32i.n  a8,  %[sb], 40\n\t"
        "l32i.n  a8,  %[in], 44\n\t"  "s32i.n  a8,  %[sb], 44\n\t"
        "l32i.n  a8,  %[in], 48\n\t"  "s32i.n  a8,  %[sb], 48\n\t"
        "l32i.n  a8,  %[in], 52\n\t"  "s32i.n  a8,  %[sb], 52\n\t"
        "l32i.n  a8,  %[in], 56\n\t"  "s32i.n  a8,  %[sb], 56\n\t"
        "l32i.n  a8,  %[in], 60\n\t"  "s32i.n  a8,  %[sb], 60\n\t"
        "movi.n  a8, 1\n\t"           "s32i    a8, %[sb], 0x90\n\t"  "memw\n\t"   //START bloc 1
        //Bloc 2 ecrit pendant que le moteur calcule le bloc 1.
        "l32i.n  a8,  %[in], 64\n\t"  "s32i.n  a8,  %[sb], 0\n\t"
        "l32i.n  a8,  %[in], 68\n\t"  "s32i.n  a8,  %[sb], 4\n\t"
        "l32i.n  a8,  %[in], 72\n\t"  "s32i.n  a8,  %[sb], 8\n\t"
        "s32i.n  %[nonce], %[sb], 12\n\t"
        "movi    a10, 0x80000000\n\t" "s32i.n  a10, %[sb], 16\n\t"
        "movi.n  a9, 0\n\t"
        "s32i.n  a9,  %[sb], 20\n\t"
        "s32i.n  a9,  %[sb], 24\n\t"
        "s32i.n  a9,  %[sb], 28\n\t"
        "s32i.n  a9,  %[sb], 32\n\t"
        "s32i.n  a9,  %[sb], 36\n\t"
        "s32i.n  a9,  %[sb], 40\n\t"
        "s32i.n  a9,  %[sb], 44\n\t"
        "s32i.n  a9,  %[sb], 48\n\t"
        "s32i.n  a9,  %[sb], 52\n\t"
        "s32i.n  a9,  %[sb], 56\n\t"
        "movi    a10, 0x280\n\t"      "s32i.n  a10, %[sb], 60\n\t"
        "1: l32i    a8, %[sb], 0x9C\n\t"  "bnez.n  a8, 1b\n\t"
        "movi.n  a8, 1\n\t"           "s32i    a8, %[sb], 0x94\n\t"  "memw\n\t"   //CONTINUE bloc 2
        "2: l32i    a8, %[sb], 0x9C\n\t"  "bnez.n  a8, 2b\n\t"
        "movi.n  a8, 1\n\t"           "s32i    a8, %[sb], 0x98\n\t"  "memw\n\t"   //LOAD digest
        "3: l32i    a8, %[sb], 0x9C\n\t"  "bnez.n  a8, 3b\n\t"
#if RACE_ASM_DBG1
        "l32i.n  %[fin], %[sb], 0\n\t"
        "j       9f\n\t"
#endif
        //Apres un SHA_LOAD, BUSY retombe AVANT que les huit mots du digest soient
        //reellement dans SHA_TEXT. Sans cette temporisation le second sha part sur un
        //digest incomplet : hash faux, de facon deterministe. La version en blocs
        //separes passait par hasard, les instructions de raccord suffisaient.
        //Second sha : le digest est deja dans TEXT[0..7], il reste le padding.
        "movi    a10, 0x80000000\n\t" "s32i.n  a10, %[sb], 32\n\t"
        "movi    a10, 0x100\n\t"      "s32i.n  a10, %[sb], 60\n\t"
        "movi.n  a8, 1\n\t"           "s32i    a8, %[sb], 0x90\n\t"  "memw\n\t"   //START bloc 3
        "4: l32i    a8, %[sb], 0x9C\n\t"  "bnez.n  a8, 4b\n\t"
        "movi.n  a8, 1\n\t"           "s32i    a8, %[sb], 0x98\n\t"  "memw\n\t"   //LOAD
        "5: l32i    a8, %[sb], 0x9C\n\t"  "bnez.n  a8, 5b\n\t"
        "l32i.n  %[fin], %[sb], 28\n\t"
        "9:\n\t"
        : [fin] "=&r" (fin)
        : [sb] "r" ((uint32_t *)(SHA_TEXT_BASE)), [in] "r" (in), [nonce] "r" (be_nonce)
        : "a8", "a9", "a10", "memory");
    return fin;
}
#endif

#if RACE_ASM_LOOP
//Boucle complete sur les nonces en assembleur : le C ne revient qu'a la sortie, sur
//un candidat ou en fin de tranche. Chaque bloc asm declare un clobber memoire, donc
//sortir vers le C a chaque nonce forcait gcc a tout relire ; ici rien ne sort.
//
//Le nonce est tenu sous sa forme grand-boutiste, celle qui part directement dans
//SHA_TEXT[3]. Deux nonces consecutifs ne different que par leur octet de poids
//faible, qui devient l'octet de poids fort une fois inverse : incrementer revient a
//ajouter 0x01000000, valable sur 256 tours. Le C rappelle la fonction tous les 256
//nonces avec une nouvelle base, ce qui evite tout echange d'octets dans la boucle
//(le Xtensa LX6 n'a pas d'instruction pour ca).
//
//Le compteur est decroissant et passe en entree-sortie : une variante avec un index
//et un drapeau de sortie separes epuisait l'allocateur de registres. Retourne le
//nombre de nonces restants ; sur un candidat le digest est encore dans SHA_TEXT et
//le C le reconnait en relisant le mot 7, une lecture par tranche.
static inline uint32_t nerd_sha_nonce_run_asm(const void *in, uint32_t be_nonce0, uint32_t count)
{
    uint32_t remaining = count;
    __asm__ __volatile__(
        "mov     a13, %[n0]\n\t"
        "movi    a12, 0x01000000\n\t"
    "0:\n\t"
        "l32i.n  a8,  %[in], 0\n\t"  "s32i.n  a8,  %[sb], 0\n\t"
        "l32i.n  a8,  %[in], 4\n\t"  "s32i.n  a8,  %[sb], 4\n\t"
        "l32i.n  a8,  %[in], 8\n\t"  "s32i.n  a8,  %[sb], 8\n\t"
        "l32i.n  a8,  %[in], 12\n\t"  "s32i.n  a8,  %[sb], 12\n\t"
        "l32i.n  a8,  %[in], 16\n\t"  "s32i.n  a8,  %[sb], 16\n\t"
        "l32i.n  a8,  %[in], 20\n\t"  "s32i.n  a8,  %[sb], 20\n\t"
        "l32i.n  a8,  %[in], 24\n\t"  "s32i.n  a8,  %[sb], 24\n\t"
        "l32i.n  a8,  %[in], 28\n\t"  "s32i.n  a8,  %[sb], 28\n\t"
        "l32i.n  a8,  %[in], 32\n\t"  "s32i.n  a8,  %[sb], 32\n\t"
        "l32i.n  a8,  %[in], 36\n\t"  "s32i.n  a8,  %[sb], 36\n\t"
        "l32i.n  a8,  %[in], 40\n\t"  "s32i.n  a8,  %[sb], 40\n\t"
        "l32i.n  a8,  %[in], 44\n\t"  "s32i.n  a8,  %[sb], 44\n\t"
        "l32i.n  a8,  %[in], 48\n\t"  "s32i.n  a8,  %[sb], 48\n\t"
        "l32i.n  a8,  %[in], 52\n\t"  "s32i.n  a8,  %[sb], 52\n\t"
        "l32i.n  a8,  %[in], 56\n\t"  "s32i.n  a8,  %[sb], 56\n\t"
        "l32i.n  a8,  %[in], 60\n\t"  "s32i.n  a8,  %[sb], 60\n\t"
        "movi.n  a8, 1\n\t"           "s32i    a8, %[sb], 0x90\n\t"  "memw\n\t"
        //Bloc 2 pendant que le moteur calcule le bloc 1.
        "l32i.n  a8,  %[in], 64\n\t"  "s32i.n  a8,  %[sb], 0\n\t"
        "l32i.n  a8,  %[in], 68\n\t"  "s32i.n  a8,  %[sb], 4\n\t"
        "l32i.n  a8,  %[in], 72\n\t"  "s32i.n  a8,  %[sb], 8\n\t"
        "s32i.n  a13, %[sb], 12\n\t"
        "movi    a10, 0x80000000\n\t" "s32i.n  a10, %[sb], 16\n\t"
        "movi.n  a9, 0\n\t"
        "s32i.n  a9,  %[sb], 20\n\t"
        "s32i.n  a9,  %[sb], 24\n\t"
        "s32i.n  a9,  %[sb], 28\n\t"
        "s32i.n  a9,  %[sb], 32\n\t"
        "s32i.n  a9,  %[sb], 36\n\t"
        "s32i.n  a9,  %[sb], 40\n\t"
        "s32i.n  a9,  %[sb], 44\n\t"
        "s32i.n  a9,  %[sb], 48\n\t"
        "s32i.n  a9,  %[sb], 52\n\t"
        "s32i.n  a9,  %[sb], 56\n\t"
        "movi    a11, 0x280\n\t"      "s32i.n  a11, %[sb], 60\n\t"
        "1: l32i    a8, %[sb], 0x9C\n\t"  "bnez.n  a8, 1b\n\t"
        "movi.n  a8, 1\n\t"           "s32i    a8, %[sb], 0x94\n\t"  "memw\n\t"
        "2: l32i    a8, %[sb], 0x9C\n\t"  "bnez.n  a8, 2b\n\t"
        "movi.n  a8, 1\n\t"           "s32i    a8, %[sb], 0x98\n\t"  "memw\n\t"
        //Travail utile pendant le LOAD : preparer le padding du second sha.
        "movi    a11, 0x100\n\t"
        "3: l32i    a8, %[sb], 0x9C\n\t"  "bnez.n  a8, 3b\n\t"
        "s32i.n  a10, %[sb], 32\n\t"  "s32i.n  a11, %[sb], 60\n\t"
        "movi.n  a8, 1\n\t"           "s32i    a8, %[sb], 0x90\n\t"  "memw\n\t"
        //Pendant le second sha : avancer le nonce et decrementer le compteur.
        "add     a13, a13, a12\n\t"
        "addi    %[cnt], %[cnt], -1\n\t"
        "4: l32i    a8, %[sb], 0x9C\n\t"  "bnez.n  a8, 4b\n\t"
        "movi.n  a8, 1\n\t"           "s32i    a8, %[sb], 0x98\n\t"  "memw\n\t"
        "5: l32i    a8, %[sb], 0x9C\n\t"  "bnez.n  a8, 5b\n\t"
        //Rejet precoce : seuls les hashs dont les 16 bits bas sont nuls sortent.
        "l16ui   a8, %[sb], 28\n\t"
        "beqz.n  a8, 9f\n\t"
        "bnez    %[cnt], 0b\n\t"
    "9:\n\t"
        : [cnt] "+r" (remaining)
        : [sb] "r" ((uint32_t *)(SHA_TEXT_BASE)), [in] "r" (in), [n0] "r" (be_nonce0)
        : "a8", "a9", "a10", "a11", "a12", "a13", "memory");
    return remaining;
}
#endif

#ifdef VALIDATION
//Test a reponse connue, joue une fois au demarrage sur la sequence reellement
//compilee : bloc 125552, en-tete et nonce publics, digest connu. Verdict immediat en
//une ligne de log, la ou attendre le compteur de desaccords demande des minutes de
//statistiques. Le padding corrompu du 7 aout aurait ete vu en vingt secondes.
static void __attribute__((noinline)) nerd_classic_kat(void)
{
  static const uint8_t kat[80] = {
    0x00,0x00,0x00,0x01,0xab,0x02,0xcd,0x81,0x8b,0x9e,0x56,0x7e,0xe2,0x17,0x93,0xcd,
    0xde,0xf2,0x99,0xfe,0xb2,0x9a,0xd4,0x44,0xa4,0x1b,0x85,0xb8,0x00,0x00,0x08,0xa3,
    0x00,0x00,0x00,0x00,0xc2,0xb6,0x20,0xe3,0x75,0x8d,0xfc,0xff,0x8b,0xdb,0x23,0x04,
    0xae,0x42,0xb9,0x1e,0x1e,0x95,0x0e,0x71,0xaf,0xf7,0x97,0xd7,0xb0,0x92,0x88,0xfc,
    0x2b,0x12,0xfc,0xf1,0x4d,0xd7,0xf5,0xc7,0x1a,0x44,0xb9,0xf2,0x95,0x46,0xa1,0x42 };
  static const uint32_t want[8] = {
    0x1dbd981f,0xe6985776,0xb644b173,0xa4d0385d,0xdc1aa2a8,0x29688d1e,0x00000000,0x00000000 };
  //L'en-tete doit etre lu depuis la RAM comme en production : depuis la flash les
  //chargements passent par le cache et sont plus lents, ce qui donne au moteur un
  //repit que la boucle reelle n'a pas. Un KAT plus lent que la production valide des
  //sequences qui echouent ensuite, c'est arrive le 7 aout.
  uint8_t hdr[80] __attribute__((aligned(4)));
  memcpy(hdr, kat, sizeof(hdr));
  uint32_t *tb = (uint32_t *)(SHA_TEXT_BASE);
  for (int i = 9; i <= 14; ++i) tb[i] = 0;
#if RACE_ASM_LOOP
  //67 tours qui finissent sur le nonce connu : verifie l'enchainement, pas seulement
  //une iteration isolee. 0x9546a100 + 0x42 = 0x9546a142 sans retenue sur l'octet de
  //poids faible, donc l'increment en forme grand-boutiste reste valable.
  nerd_sha_nonce_run_asm(hdr, __builtin_bswap32(0x9546a100), 0x43);
#elif RACE_ASM_NONCE
  nerd_sha_nonce_asm(hdr, __builtin_bswap32(0x9546a142));
#else
  nerd_sha_ll_fill_text_block_sha256(hdr);
#if !RACE_ASM_FILL
  sha_ll_start_block(SHA2_256);
#endif
  nerd_sha_hal_wait_idle();
  nerd_sha_ll_fill_text_block_sha256_upper(hdr+64, 0x9546a142);
  sha_ll_continue_block(SHA2_256);
  nerd_sha_hal_wait_idle();
  sha_ll_load(SHA2_256);
  nerd_sha_hal_wait_idle();
  nerd_sha_ll_fill_text_block_sha256_double();
  sha_ll_start_block(SHA2_256);
  nerd_sha_hal_wait_idle();
  sha_ll_load(SHA2_256);
  nerd_sha_hal_wait_idle();
#endif
  bool ok = true;
  for (int i = 0; i < 8; ++i)
    if (_DPORT_REG_READ(SHA_TEXT_BASE + i*4) != want[i]) ok = false;
  race_kat_state = ok ? 1 : 0;
  if (ok) {
    Serial.println("KAT: ok (bloc 125552)");
  } else {
    //Une seule ecriture serie : les autres taches loggent en parallele et
    //decoupaient la ligne en morceaux illisibles.
    char line[160];
    int k = snprintf(line, sizeof(line), "KAT: ECHEC got=");
    for (int i = 0; i < 8; ++i)
      k += snprintf(line+k, sizeof(line)-k, "%08x", (unsigned)_DPORT_REG_READ(SHA_TEXT_BASE + i*4));
    snprintf(line+k, sizeof(line)-k, " attendu=1dbd981fe6985776...");
    Serial.println(line);
  }
  for (int i = 9; i <= 14; ++i) tb[i] = 0;
}
#endif

void minerWorkerHw(void * task_id)
{
  unsigned int miner_id = (uint32_t)task_id;
  Serial.printf("[MINER] %d Started minerWorkerHwEsp32D Task!\n", miner_id);

  std::shared_ptr<JobRequest> job;
  std::shared_ptr<JobResult> result;
  uint8_t hash[32];
  uint8_t sha_buffer[128];

  //Le chemin classic n'avait aucun controle de hash : le compteur de mismatch etait
  //cablé au chemin S3 seulement, donc un "mismatch=0" sur une carte classic ne
  //prouvait rien. Meme controle que sur S3 : chaque hash qui passe le filtre des 16
  //bits de poids faible est recalcule en logiciel et compare.
#ifdef VALIDATION
  uint8_t doubleHash[32];
  uint32_t digest_mid_v[8];
  uint32_t bake[16];
  //Le job materiel du classic porte l'en-tete deja inverse mot par mot
  //(sha_buffer_swap, cote stratum). La reference logicielle veut l'en-tete d'origine,
  //on le reconstruit ici plutot que de faire porter deux buffers au job.
  uint8_t hdr[80];
#endif

  while (1)
  {
    //Idle during an OTA so the SHA engine lock is released: esp_image_verify()
    //needs the engine to check the uploaded image, and without this the upload
    //completes then fails at verification. The S3 path already had this guard.
    if (ota_active) { vTaskDelay(100 / portTICK_PERIOD_MS); continue; }
    {
      std::lock_guard<std::mutex> lock(s_job_mutex);
      if (result)
      {
        race_hashes_hw += result->nonce_count;
        if (s_job_result_list.size() < 16)
          s_job_result_list.push_back(result);
        result.reset();
      }
      if (!s_job_request_list_hw.empty())
      {
        job = s_job_request_list_hw.front();
        s_job_request_list_hw.pop_front();
      } else
        job.reset();
    }
    if (job)
    {
      result = std::make_shared<JobResult>();
      result->id = job->id;
      result->nonce = 0xFFFFFFFF;
      result->nonce_count = job->nonce_count;
      result->difficulty = job->difficulty;
      uint8_t job_in_work = job->id & 0xFF;
      memcpy(sha_buffer, job->sha_buffer, 80);
#ifdef VALIDATION
      for (int i = 0; i < 20; ++i)
        ((uint32_t*)hdr)[i] = __builtin_bswap32(((const uint32_t*)job->sha_buffer)[i]);
      nerd_mids(digest_mid_v, hdr);
      nerd_sha256_bake(digest_mid_v, hdr+64, bake);
#endif

      esp_sha_lock_engine(SHA2_256);
#ifdef VALIDATION
      { static bool kat_done = false; if (!kat_done) { kat_done = true; nerd_classic_kat(); } }
#endif
      //SHA_TEXT[9..14] are zero in both paddings used below and the engine never
      //writes them, so they are set once here instead of twice per nonce.
      {
        uint32_t *tb = (uint32_t *)(SHA_TEXT_BASE);
        for (int i = 9; i <= 14; ++i) tb[i] = 0x00000000;
      }
      const uint32_t nonce_start = job->nonce_start;
      const uint32_t nonce_count = job->nonce_count;
#if RACE_ASM_LOOP
      //La boucle vit dans l'assembleur, par tranches de 256 nonces : au-dela, la forme
      //grand-boutiste du nonce ne s'incremente plus par un simple ajout constant.
      uint32_t n = 0;
      while (n < nonce_count)
      {
        //La tranche ne doit jamais franchir une frontiere de 256 : au-dela, l'octet
        //de poids faible du nonce deborde et l'increment de 0x01000000 sur la forme
        //grand-boutiste ne correspond plus. Apres un candidat, n avance d'une valeur
        //quelconque, donc la base n'est plus alignee : c'est le cas qui fait tout
        //derailler si on decoupe naivement par 256.
        const uint32_t base = nonce_start + n;
        uint32_t chunk = 256 - (base & 0xFF);
        if (chunk > nonce_count - n) chunk = nonce_count - n;
        n += chunk - nerd_sha_nonce_run_asm(sha_buffer, __builtin_bswap32(base), chunk);
        if (nerd_sha_ll_read_digest_swap_if(hash))
        {
          const uint32_t nonce_hit = nonce_start + n - 1;
#ifdef VALIDATION
          ((uint32_t*)(hdr+64+12))[0] = nonce_hit;
          bool bad = !nerd_sha256d_baked(digest_mid_v, hdr+64, bake, doubleHash);
          if (!bad)
            for (int i = 0; i < 32; ++i)
              if (hash[i] != doubleHash[i]) { bad = true; break; }
          if (bad) {
            //Les trois premiers desaccords sont detailles : on cherche a savoir si
            //l'evenement unique observe est lie au demarrage (premier candidat, premier
            //job) ou reparti au hasard dans la plage de nonces.
            if (race_sha_mismatch < 3)
              Serial.printf("MISM nonce=%08lx base=%08lx idx=%lu n=%lu up=%lus\n",
                            (unsigned long)nonce_hit, (unsigned long)base,
                            (unsigned long)(nonce_hit - base), (unsigned long)n,
                            (unsigned long)(millis()/1000));
            race_sha_mismatch++;
          }
#endif
          double diff_hash = diff_from_target(hash);
          if (diff_hash > result->difficulty && isSha256Valid(hash))
          {
            result->difficulty = diff_hash;
            result->nonce = nonce_hit;
            memcpy(result->hash, hash, sizeof(hash));
          }
        }
        if (s_working_current_job_id != job_in_work) break;
      }
      result->nonce_count = n;
#else
      for (uint32_t n = 0; n < job->nonce_count; ++n)
      {

#if RACE_CLASSIC_BENCH
        //Profil : cycles passes a attendre le moteur contre cycles CPU. Determine
        //s'il reste quelque chose a gagner ou si le silicium fixe le plafond.
        uint32_t cb0 = RACE_CC();
#endif
#if RACE_ASM_NONCE
        const uint32_t fin_w7 = nerd_sha_nonce_asm(sha_buffer, __builtin_bswap32(job->nonce_start+n));
#elif RACE_PREFILL
        //Variante recouvrement : les ecritures qui ne dependent pas du resultat en
        //cours partent pendant que le moteur calcule, au lieu d'attendre leur tour.
        nerd_sha_ll_fill_text_block_sha256(sha_buffer);          //bloc 1 + START
        nerd_sha_ll_fill_text_block_sha256_upper_nocont(sha_buffer+64, job->nonce_start+n);
        RACE_WAITB();
        nerd_sha_continue_asm();
#if RACE_PREFILL >= 2
        nerd_sha_ll_fill_double_nostart();                       //TEXT[8] et TEXT[15]
#endif
        RACE_WAITB();
        nerd_sha_ll_load_asm();
        RACE_WAITB();
#if RACE_PREFILL >= 2
        nerd_sha_start_asm();
#else
        nerd_sha_ll_fill_text_block_sha256_double_asm();
#endif
        RACE_WAITB();
        nerd_sha_ll_load_asm();
#else
        nerd_sha_ll_fill_text_block_sha256(sha_buffer);
#if !RACE_ASM_FILL
        sha_ll_start_block(SHA2_256);
#endif

        //sha_hal_hash_block(SHA2_256, s_test_buffer+64, 64/4, false);
#if RACE_CLASSIC_BENCH
        { uint32_t a=RACE_CC(); nerd_sha_hal_wait_idle(); s_cbench.wait += RACE_CC()-a; }
#else
        nerd_sha_hal_wait_idle();
#endif
#if RACE_ASM_FILL
        nerd_sha_ll_fill_text_block_sha256_upper_asm(sha_buffer+64, job->nonce_start+n);
#else
        nerd_sha_ll_fill_text_block_sha256_upper(sha_buffer+64, job->nonce_start+n);
        sha_ll_continue_block(SHA2_256);
#endif

#if RACE_CLASSIC_BENCH
        { uint32_t a=RACE_CC(); nerd_sha_hal_wait_idle(); s_cbench.wait += RACE_CC()-a; }
#else
        nerd_sha_hal_wait_idle();
#endif
#if RACE_ASM_FILL
        nerd_sha_ll_load_asm();
#else
        sha_ll_load(SHA2_256);
#endif

#if RACE_CLASSIC_BENCH
        { uint32_t a=RACE_CC(); nerd_sha_hal_wait_idle(); s_cbench.wait += RACE_CC()-a; }
#else
        nerd_sha_hal_wait_idle();
#endif
#if RACE_ASM_FILL
        nerd_sha_ll_fill_text_block_sha256_double_asm();
#else
        nerd_sha_ll_fill_text_block_sha256_double();
        sha_ll_start_block(SHA2_256);
#endif

#if RACE_CLASSIC_BENCH
        { uint32_t a=RACE_CC(); nerd_sha_hal_wait_idle(); s_cbench.wait += RACE_CC()-a; }
#else
        nerd_sha_hal_wait_idle();
#endif
#if RACE_ASM_FILL
        nerd_sha_ll_load_asm();
#else
        sha_ll_load(SHA2_256);
#endif
#endif  //RACE_ASM_NONCE / RACE_PREFILL
#if !RACE_ASM_NONCE
        //Le LOAD copie le digest dans SHA_TEXT et n'est pas instantane. La lecture
        //DPORT brute est assez rapide pour passer devant : sans cette attente on lit
        //le contenu precedent des registres, ce que la validation voit tout de suite
        //(1969 desaccords en 4 minutes contre 1). La sequence DPORT d'origine etait
        //assez lente pour masquer la course.
        nerd_sha_hal_wait_idle();
#endif
#if RACE_CLASSIC_BENCH
        s_cbench.total += RACE_CC()-cb0; s_cbench.n++;
        if (s_cbench.n >= 200000) {
          Serial.printf("ClassicBench: %u cyc/nonce dont %u en attente moteur (%.0f%%)\n",
            s_cbench.total/s_cbench.n, s_cbench.wait/s_cbench.n,
            100.0*s_cbench.wait/s_cbench.total);
          s_cbench.total=0; s_cbench.wait=0; s_cbench.n=0;
        }
#endif
#if RACE_ASM_NONCE
        //Le bloc assembleur a deja lu le mot 7 : on ne relit le digest complet que
        //quand les 16 bits de poids faible sont nuls, soit environ 9 fois par seconde.
        if (__builtin_expect((fin_w7 & 0xFFFF) == 0, 0) && nerd_sha_ll_read_digest_swap_if(hash))
#else
        if (nerd_sha_ll_read_digest_swap_if(hash))
#endif
        {
#ifdef VALIDATION
          //nerd_sha256d_baked a son propre filtre 16 bits et ne remplit doubleHash que
          //s'il passe : un retour faux alors que le materiel a passe le filtre est
          //deja un desaccord, on le compte comme tel.
          ((uint32_t*)(hdr+64+12))[0] = job->nonce_start+n;
          if (!nerd_sha256d_baked(digest_mid_v, hdr+64, bake, doubleHash))
          {
            race_sha_mismatch++;
          }
          else
          for (int i = 0; i < 32; ++i)
          {
            if (hash[i] != doubleHash[i])
            {
              Serial.println("***HW sha256 esp32 mismatch***");
              race_sha_mismatch++;
              break;
            }
          }
#endif
          //~5 per second
          double diff_hash = diff_from_target(hash);
          if (diff_hash > result->difficulty)
          {
            if (isSha256Valid(hash))
            {
              result->difficulty = diff_hash;
              result->nonce = job->nonce_start+n;
              memcpy(result->hash, hash, sizeof(hash));
            }
          }
        }
        if (
             (uint8_t)(n & 0xFF) == 0 &&
             s_working_current_job_id != job_in_work)
        {
          result->nonce_count = n+1;
          break;
        }
      }
#endif
      esp_sha_unlock_engine(SHA2_256);
    } else {
      race_starved_hw++;
      vTaskDelay(2 / portTICK_PERIOD_MS);
    }

    esp_task_wdt_reset();
  }
}

#endif  //CONFIG_IDF_TARGET_ESP32

#endif  //HARDWARE_SHA265


//screen refresh period. 100 ms = 10 fps (upstream). The redraw costs
//CPU *and* SPI bandwidth shared with the miners — see RACE_HEADLESS (+4% with no
//screen at all). Overridable at build time to A/B the refresh rate.
#ifndef RACE_SCREEN_MS
#define RACE_SCREEN_MS 100
#endif
#define DELAY RACE_SCREEN_MS
//Full-screen redraw period, in seconds. Upstream redraws every second; the redraw
//is the expensive part (CPU + SPI shared with the miners), not the animation.
#ifndef RACE_DRAW_EVERY_S
#define RACE_DRAW_EVERY_S 1
#endif
#define REDRAW_EVERY 10

void restoreStat() {
  if(!Settings.saveStats) return;
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.printf("[MONITOR] NVS partition is full or has invalid version, erasing...\n");
    nvs_flash_init();
  }

  ret = nvs_open("state", NVS_READWRITE, &stat_handle);

  size_t required_size = sizeof(double);
  nvs_get_blob(stat_handle, "best_diff", &best_diff, &required_size);
  nvs_get_u32(stat_handle, "Mhashes", &Mhashes);
  uint32_t nv_shares, nv_valids;
  nvs_get_u32(stat_handle, "shares", &nv_shares);
  nvs_get_u32(stat_handle, "valids", &nv_valids);
  shares = nv_shares;
  valids = nv_valids;
  nvs_get_u32(stat_handle, "templates", &templates);
  nvs_get_u64(stat_handle, "upTime", &upTime);

  uint32_t crc = crc32_reset();
  crc = crc32_add(crc, &best_diff, sizeof(best_diff));
  crc = crc32_add(crc, &Mhashes, sizeof(Mhashes));
  crc = crc32_add(crc, &nv_shares, sizeof(nv_shares));
  crc = crc32_add(crc, &nv_valids, sizeof(nv_valids));
  crc = crc32_add(crc, &templates, sizeof(templates));
  crc = crc32_add(crc, &upTime, sizeof(upTime));
  crc = crc32_finish(crc);

  uint32_t nv_crc;
  nvs_get_u32(stat_handle, "crc32", &nv_crc);
  if (nv_crc != crc)
  {
    best_diff = 0.0;
    Mhashes = 0;
    shares = 0;
    valids = 0;
    templates = 0;
    upTime = 0;
  }
}

void saveStat() {
  if(!Settings.saveStats) return;
  Serial.printf("[MONITOR] Saving stats\n");
  nvs_set_blob(stat_handle, "best_diff", &best_diff, sizeof(best_diff));
  nvs_set_u32(stat_handle, "Mhashes", Mhashes);
  nvs_set_u32(stat_handle, "shares", shares);
  nvs_set_u32(stat_handle, "valids", valids);
  nvs_set_u32(stat_handle, "templates", templates);
  nvs_set_u64(stat_handle, "upTime", upTime);

  uint32_t crc = crc32_reset();
  crc = crc32_add(crc, &best_diff, sizeof(best_diff));
  crc = crc32_add(crc, &Mhashes, sizeof(Mhashes));
  uint32_t nv_shares = shares;
  uint32_t nv_valids = valids;
  crc = crc32_add(crc, &nv_shares, sizeof(nv_shares));
  crc = crc32_add(crc, &nv_valids, sizeof(nv_valids));
  crc = crc32_add(crc, &templates, sizeof(templates));
  crc = crc32_add(crc, &upTime, sizeof(upTime));
  crc = crc32_finish(crc);
  nvs_set_u32(stat_handle, "crc32", crc);
}

void resetStat() {
    Serial.printf("[MONITOR] Resetting NVS stats\n");
    templates = hashes = Mhashes = totalKHashes = elapsedKHs = upTime = shares = valids = 0;
    best_diff = 0.0;
    saveStat();
}

void runMonitor(void *name)
{

  Serial.println("[MONITOR] started");
  restoreStat();

  unsigned long mLastCheck = 0;

  resetToFirstScreen();

  unsigned long frame = 0;

  uint32_t seconds_elapsed = 0;

  totalKHashes = (Mhashes * 1000) + hashes / 1000;
  uint32_t last_update_millis = millis();
  uint32_t uptime_frac = 0;

  while (1)
  {
    uint32_t now_millis = millis();
    if (now_millis < last_update_millis)
      now_millis = last_update_millis;
    
    uint32_t mElapsed = now_millis - mLastCheck;
    if (mElapsed >= 1000)
    { 
      mLastCheck = now_millis;
      last_update_millis = now_millis;
      unsigned long currentKHashes = (Mhashes * 1000) + hashes / 1000;
      elapsedKHs = currentKHashes - totalKHashes;
      totalKHashes = currentKHashes;

      uptime_frac += mElapsed;
      while (uptime_frac >= 1000)
      {
        uptime_frac -= 1000;
        upTime ++;
      }

#if !RACE_HEADLESS
      {
#if SCREEN_TIMEOUT_S
        //Blank after SCREEN_TIMEOUT_S without a button press. Cutting the backlight
        //alone would save nothing: the cost is the redraw and its SPI traffic, so
        //stop drawing too.
        if (g_screen_on && (uint32_t)(millis() - g_lastInputMs) > (SCREEN_TIMEOUT_S * 1000UL)) {
          alternateScreenState();
          g_screen_on = false;
          Serial.printf("Screen off after %ds idle\n", SCREEN_TIMEOUT_S);
        }
#endif
        if (g_screen_on) {
          static uint32_t s_draw_skip = 0;
          if (++s_draw_skip >= RACE_DRAW_EVERY_S) {
            s_draw_skip = 0;
            drawCurrentScreen(mElapsed);
          }
        }
      }
#endif

      // Monitor state when hashrate is 0.0
      if (elapsedKHs == 0)
      {
        Serial.printf(">>> [i] Miner: newJob>%s / inRun>%s) - Client: connected>%s / subscribed>%s / wificonnected>%s\n",
            "true",//(1) ? "true" : "false",
            isMinerSuscribed ? "true" : "false",
            client.connected() ? "true" : "false", isMinerSuscribed ? "true" : "false", WiFi.status() == WL_CONNECTED ? "true" : "false");
      }

      #ifdef DEBUG_MEMORY
      Serial.printf("### [Total Heap / Free heap / Min free heap]: %d / %d / %d \n", ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMinFreeHeap());
      Serial.printf("### Max stack usage: %d\n", uxTaskGetStackHighWaterMark(NULL));
      #endif

      seconds_elapsed++;

      if(seconds_elapsed % (saveIntervals[currentIntervalIndex]) == 0){
        saveStat();
        seconds_elapsed = 0;
        if(currentIntervalIndex < saveIntervalsSize - 1)
          currentIntervalIndex++;
      }    
    }
#if !RACE_HEADLESS
    //a board whose panel is physically dead — skip every TFT/SPI redraw
    //and give those cycles (and the SPI bus) back to the SW mining path.
    animateCurrentScreen(frame);
    doLedStuff(frame);
#endif

    vTaskDelay(DELAY / portTICK_PERIOD_MS);
    frame++;
  }
}
