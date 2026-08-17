// One-shot touch calibration for the YellowDeck.
// Shows 3 crosshairs in portrait; prints raw XPT2046 values for each tap:
//   CAL<n> <rawx> <rawy>
// then DONE. Used to derive the mapping baked into yellowdeck.

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSpi(HSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

struct Pt { int x, y; };
const Pt TARGETS[3] = {{30, 30}, {210, 30}, {30, 290}};
int step = 0;
bool wasTouched = false;

void drawTarget(int i) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Touch calibration", 120, 140, 2);
  char msg[24];
  snprintf(msg, sizeof(msg), "Tap crosshair %d of 3", i + 1);
  tft.drawString(msg, 120, 165, 2);
  int x = TARGETS[i].x, y = TARGETS[i].y;
  tft.drawFastHLine(x - 12, y, 25, TFT_RED);
  tft.drawFastVLine(x, y - 12, 25, TFT_RED);
  tft.drawCircle(x, y, 8, TFT_RED);
}

void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  touchSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSpi);
  ts.setRotation(1); // raw passthrough
  tft.init();
  tft.setRotation(0); // portrait
  drawTarget(0);
  Serial.println("CALIBRATION READY");
}

void loop() {
  bool touched = ts.tirqTouched() && ts.touched();
  if (touched && !wasTouched && step < 3) {
    // average a few samples
    long sx = 0, sy = 0;
    int n = 0;
    for (int i = 0; i < 8; i++) {
      if (ts.touched()) {
        TS_Point p = ts.getPoint();
        sx += p.x; sy += p.y; n++;
      }
      delay(10);
    }
    if (n > 0) {
      Serial.printf("CAL%d %ld %ld\n", step + 1, sx / n, sy / n);
      step++;
      if (step < 3) {
        drawTarget(step);
      } else {
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_GREEN);
        tft.drawString("Done! Claude will", 120, 150, 2);
        tft.drawString("reflash the deck now.", 120, 175, 2);
        Serial.println("DONE");
      }
    }
    delay(400); // debounce
  }
  wasTouched = touched;
  delay(20);
}
