#ifndef VERSION_H
#define VERSION_H

//Surchargeable depuis platformio.ini : un build de test remonte son propre nom sur le
//dashboard, seule preuve qu'un OTA a vraiment pris (espota rend 0 meme quand la
//carte redemarre sur l'ancienne image).
#ifndef CURRENT_VERSION
#define CURRENT_VERSION "V2.1.0"
#endif

#endif // VERSION_H
