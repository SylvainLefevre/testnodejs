// Bip toutes les secondes : ESP32-S3-DevKitC-1 v1.1 + MAX98357A (I2S)
// Nécessite le core Arduino-ESP32 3.x (bibliothèque ESP_I2S incluse).

#include <ESP_I2S.h>
#include <math.h>

// Câblage (voir README.md)
const int PIN_BCLK = 5;  // MAX98357A BCLK
const int PIN_LRC  = 4;  // MAX98357A LRC (WS)
const int PIN_DIN  = 6;  // MAX98357A DIN

// Réglages du bip
const uint32_t SAMPLE_RATE = 44100;  // Hz
const float    BEEP_FREQ   = 1000.0; // Hz
const uint32_t BEEP_MS     = 100;    // durée du bip
const uint32_t PERIOD_MS   = 1000;   // un bip par seconde
const int16_t  AMPLITUDE   = 8000;   // volume, 0 à 32767
const uint32_t FADE_MS     = 5;      // fondu d'entrée/sortie, évite les "clics"

const uint32_t PERIOD_FRAMES = SAMPLE_RATE * PERIOD_MS / 1000;
const uint32_t BEEP_FRAMES   = SAMPLE_RATE * BEEP_MS / 1000;
const uint32_t FADE_FRAMES   = SAMPLE_RATE * FADE_MS / 1000;

const size_t CHUNK_FRAMES = 256;
int16_t chunk[CHUNK_FRAMES * 2];  // stéréo : gauche, droite

I2SClass i2s;

void setup() {
  Serial.begin(115200);

  i2s.setPins(PIN_BCLK, PIN_LRC, PIN_DIN);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("Echec de l'initialisation I2S");
    while (true) delay(1000);
  }
}

// Échantillon n° `frame` de la période : sinusoïde pendant le bip, silence ensuite.
int16_t sampleAt(uint32_t frame) {
  if (frame >= BEEP_FRAMES) return 0;

  float envelope = 1.0f;
  if (frame < FADE_FRAMES) {
    envelope = (float)frame / FADE_FRAMES;
  } else if (frame >= BEEP_FRAMES - FADE_FRAMES) {
    envelope = (float)(BEEP_FRAMES - frame) / FADE_FRAMES;
  }
  return (int16_t)(AMPLITUDE * envelope * sinf(2.0f * PI * BEEP_FREQ * frame / SAMPLE_RATE));
}

// Chaque tour de loop() envoie exactement une seconde d'audio (bip + silence).
// i2s.write() est bloquant : c'est l'horloge I2S qui cadence la période.
void loop() {
  uint32_t frame = 0;
  while (frame < PERIOD_FRAMES) {
    size_t n = min((uint32_t)CHUNK_FRAMES, PERIOD_FRAMES - frame);
    for (size_t i = 0; i < n; i++) {
      int16_t s = sampleAt(frame + i);
      chunk[2 * i]     = s;
      chunk[2 * i + 1] = s;
    }
    i2s.write((const uint8_t *)chunk, n * 2 * sizeof(int16_t));
    frame += n;
  }
}
