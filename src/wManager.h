//Botón configuración
#define PIN_BUTTON_1               0
#define PIN_BUTTON_2               14

void init_WifiManager();
void wifiManagerProcess();
void checkResetConfigButton();
void checkRemoveConfiguration();
//void checkScreenButton();

extern int stopScreen;
extern int startScreen;
extern int portNumber;