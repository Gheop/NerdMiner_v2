#include <Arduino.h>
#include <WiFi.h>
//#include <WebServer.h>
#include <WiFiManager.h>

#include <esp_task_wdt.h>
#include <TFT_eSPI.h> // Graphics and font library

#include "mbedtls/md.h"
#include "media/images.h"
#include "media/myFonts.h"
#include "OpenFontRender.h"
#include "config.h"
//#include "mining.h"
#include "time.h"
//3 seconds WDT
#define WDT_TIMEOUT 3

OpenFontRender render;

/**********************⚡ GLOBAL Vars *******************************/
TFT_eSPI tft = TFT_eSPI();  // Invoke library, pins defined in User_Setup.h
TFT_eSprite background = TFT_eSprite(&tft);  // Invoke library sprite

static long templates = 0;
static long hashes = 0;
static int halfshares = 0; // increase if blockhash has 16 bits of zeroes
static int shares = 0; // increase if blockhash has 32 bits of zeroes
static int valids = 0; // increased if blockhash <= target

int oldStatus = 0;
unsigned long start = millis();

char timeHour[3];
//char timeMin[3];

int screenOff = HIGH;
static unsigned long lastButton2Press = 0;

#include <WiFi.h>

unsigned long previousMillis = 0;
unsigned long interval = 30000;

void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  Serial.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(500);
  }
  Serial.println(WiFi.localIP());
}

void checkScreenButton()
{
  if ((millis() - lastButton2Press) > 250)
  {
    screenOff = !digitalRead(TFT_BL);
    digitalWrite(TFT_BL, screenOff);
    previousMillis =  lastButton2Press = millis();
  }
 
}

void setup()
{
  Serial.begin(115200);
  Serial.println("------------------------\n| Hello #TeamNerdMiner |\n------------------------\n\n");
  //battery on
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);

  Serial.setTimeout(0);
 // delay(100);

  // Idle task that would reset WDT never runs, because core 0 gets fully utilized
  disableCore0WDT();
  
  /******** INIT NERDMINER ************/
  Serial.println("NerdMiner v2 starting......");

  // // Setup the button
  // pinMode(PIN_BUTTON_1, INPUT);
  // attachInterrupt(PIN_BUTTON_1, checkResetConfigButton, FALLING);

  pinMode(PIN_BUTTON_2, INPUT_PULLUP);
  attachInterrupt(PIN_BUTTON_2, checkScreenButton, FALLING);
  
  /******** INIT DISPLAY ************/
  tft.init();
  tft.setRotation(1);
  //SPIFFS.begin();
  tft.setSwapBytes(true);// Swap the colour byte order when rendering
  background.createSprite(initWidth,initHeight); //Background Sprite
  background.setSwapBytes(true);
  render.setDrawer(background); // Link drawing object to background instance (so font will be rendered on background)
  render.setLineSpaceRatio(0.9); //Espaciado entre texto

  // Load the font and check it can be read OK
  //if (render.loadFont(NotoSans_Bold, sizeof(NotoSans_Bold))) {
  if (render.loadFont(DigitalNumbers, sizeof(DigitalNumbers))){
    Serial.println("Initialise font error");
    return;
  }
  
  /******** PRINT INIT SCREEN *****/
  tft.fillScreen(TFT_BLACK);
  tft.pushImage(0, 0, initWidth, initHeight, initScreen);

  //delay(2000);
  initWiFi();
  Serial.print("RSSI: ");
  Serial.println(WiFi.RSSI());
  
  /******** CREATE TASK TO PRINT SCREEN *****/
  //tft.pushImage(0, 0, MinerWidth, MinerHeight, MinerScreen);
  // Higher prio monitor task
  Serial.println("");

  //const char* ntpServer = "pool.ntp.org";
  configTzTime("CET-1CEST-2,M3.5.0/02:00:00,M10.5.0/03:00:00", "pool.ntp.org");
  Serial.println("Initiating tasks...");
  xTaskCreate(runMonitor, "Monitor", 5000, NULL, 4, NULL);

  /******** CREATE MINER TASKS *****/
  for (size_t i = 0; i < THREADS; i++) {
    char *name = (char*) malloc(32);
    sprintf(name, "(%d)", i);

    // Start mining tasks
    BaseType_t res = xTaskCreate(runWorker, name, 30000, (void*)name, 1, NULL);
    Serial.printf("Starting %s %s!\n", name, res == pdPASS? "successful":"failed");
  }
}

int getHour()
{
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo))
  {
    return -1;
  }
  strftime(timeHour,3, "%H", &timeinfo);
  return String(timeHour).toInt();
}

void app_error_fault_handler(void *arg) {
  char *stack = (char *)arg;
  esp_log_write(ESP_LOG_ERROR, "APP_ERROR", "Pila de errores:\n%s", stack);
  esp_restart();
}

void loop()
{
  unsigned long currentMillis = millis();

  if(previousMillis + interval  < currentMillis) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.print(millis());
    Serial.println("Reconnecting to WiFi...");
    WiFi.disconnect();
    WiFi.reconnect();
    
  }
    int hour = getHour();
    Serial.println("Hour:"+String(hour));
    if (screenOff != LOW && (hour < 8 || hour >= 20))
    {
      digitalWrite(TFT_BL, LOW);
    }
    else
    {
      digitalWrite(TFT_BL, screenOff);
    }
    previousMillis = currentMillis;
  }
}