
// Mining
#define THREADS 1
#define MAX_NONCE 1000000
#define PIN_POWER_ON 15
// #define MAX_NONCE    1.215.752.192

// Pool
//#define POOL_URL "solo.ckpool.org" //"btc.zsolo.bid" "eu.stratum.slushpool.com"
//#define POOL_PORT 3333  //6057 //3333


extern int screenOff;


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
//#define elapsedDays(_time_) ( _time_ / SECS_PER_DAY)  
