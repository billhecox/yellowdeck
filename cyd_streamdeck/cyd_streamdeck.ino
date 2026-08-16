// CYD Stream Deck — now-playing header + 10 touch buttons (2 cols x 5 rows,
// portrait) with icons.
//
// Sends "BTN:<id>\n" over USB serial on each press; a Python bridge on the PC
// (streamdeck_bridge.py) maps IDs to actions.
//
// Buttons are configurable at runtime over serial — no reflash needed:
//   CFG:<id>:<icon>:<label>:<rrggbb>\n
// e.g. CFG:0:play:Play:22CC44
// The header shows player status/track info pushed by the bridge:
//   NOW:<play|pause|stop>:<title>:<artist>\n
// The bridge pushes the layout from streamdeck_config.json on connect and
// whenever the file changes.
//
// Board: ESP32-2432S028R "Cheap Yellow Display"
// Display: ILI9341 240x320 via TFT_eSPI (see TFT_eSPI_Setup_CYD.h)
// Touch: XPT2046 on its own SPI bus

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// ---- Touch controller pins (fixed by the CYD board layout) ----
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

// ---- Touch calibration (measured on this panel with cyd_touchcal) ----
// Raw p.x maps to screen Y (direct); raw p.y maps to screen X (inverted).
#define TOUCH_RX_MIN 139   // raw p.x at screen y = 0
#define TOUCH_RX_MAX 3682  // raw p.x at screen y = 319
#define TOUCH_RY_MIN 242   // raw p.y at screen x = 239
#define TOUCH_RY_MAX 3876  // raw p.y at screen x = 0

// ---- Layout: portrait 240x320, header + 2 columns x 5 rows ----
#define SCREEN_W 240
#define SCREEN_H 320
#define HEADER_H 45
#define COLS 2
#define ROWS 5
#define CELL_W (SCREEN_W / COLS)                // 120
#define CELL_H ((SCREEN_H - HEADER_H) / ROWS)   // 55
#define PAD 3
#define NUM_BUTTONS (COLS * ROWS)
#define LABEL_MAX 12
#define NOW_MAX 26

// ---- Colors (RGB565) ----
#define COL_BG 0x0000
#define COL_BTN 0x18E3 // dark gray fill
#define COL_TEXT 0xFFFF
#define COL_DIM 0x8410  // mid gray (artist line, stopped state)
#define COL_PLAY 0x07E8 // green
#define COL_PAUSE 0xFD20 // amber

enum IconId {
  IC_PLAY, IC_NEXT, IC_PREV, IC_VOLUP, IC_VOLDN, IC_MUTE,
  IC_TERM, IC_WEB, IC_FILES, IC_CODE, IC_MUSIC, IC_MAIL,
  IC_SHOT, IC_CALC, IC_LOCK, IC_GEAR,
  IC_MIC, IC_CAM, IC_CHAT, IC_GAME, IC_REC, IC_STOP,
  IC_STAR, IC_HEART, IC_SUN, IC_MOON, IC_SEARCH, IC_BELL,
  IC_HOME, IC_POWER,
  IC_COUNT
};

const char *ICON_NAMES[IC_COUNT] = {
  "play", "next", "prev", "volup", "voldn", "mute",
  "term", "web", "files", "code", "music", "mail",
  "shot", "calc", "lock", "gear",
  "mic", "cam", "chat", "game", "rec", "stop",
  "star", "heart", "sun", "moon", "search", "bell",
  "home", "power"
};

struct Button {
  char label[LABEL_MAX + 1];
  IconId icon;
  uint16_t accent;
};

// Defaults shown until the bridge pushes its own layout.
Button buttons[NUM_BUTTONS] = {
  {"Play",  IC_PLAY,  0x07E8}, // 0
  {"Next",  IC_NEXT,  0x07E8}, // 1
  {"Prev",  IC_PREV,  0x07E8}, // 2
  {"Vol +", IC_VOLUP, 0x055F}, // 3
  {"Vol -", IC_VOLDN, 0x055F}, // 4
  {"Mute",  IC_MUTE,  0xF981}, // 5
  {"Term",  IC_TERM,  0xAFE5}, // 6
  {"Code",  IC_CODE,  0x3D9F}, // 7
  {"Music", IC_MUSIC, 0xF8B2}, // 8
  {"Lock",  IC_LOCK,  0xFFE0}, // 9
};

// Now-playing header state, updated by NOW:<state>:<title>:<artist>.
char nowState = 's'; // 'p' playing, 'a' paused, 's' stopped/none
char nowTitle[NOW_MAX + 1] = "";
char nowArtist[NOW_MAX + 1] = "";

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSpi(HSPI); // TFT_eSPI owns VSPI; touch gets its own bus
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

bool wasTouched = false;
int heldButton = -1;
int pendingButton = -1; // press candidate awaiting confirmation read

char lineBuf[96];
size_t lineLen = 0;

void drawIcon(IconId id, int cx, int cy, uint16_t c) {
  switch (id) {
    case IC_PLAY:
      tft.fillTriangle(cx - 10, cy - 8, cx - 10, cy + 8, cx, cy, c);
      tft.fillRect(cx + 3, cy - 8, 3, 17, c);
      tft.fillRect(cx + 8, cy - 8, 3, 17, c);
      break;
    case IC_NEXT:
      tft.fillTriangle(cx - 10, cy - 8, cx - 10, cy + 8, cx + 4, cy, c);
      tft.fillRect(cx + 6, cy - 8, 3, 17, c);
      break;
    case IC_PREV:
      tft.fillRect(cx - 9, cy - 8, 3, 17, c);
      tft.fillTriangle(cx + 10, cy - 8, cx + 10, cy + 8, cx - 4, cy, c);
      break;
    case IC_VOLUP:
    case IC_VOLDN:
    case IC_MUTE:
      // speaker body + cone
      tft.fillRect(cx - 11, cy - 4, 5, 9, c);
      tft.fillTriangle(cx - 7, cy, cx, cy - 8, cx, cy + 8, c);
      if (id == IC_VOLUP) {
        tft.fillRect(cx + 3, cy - 1, 9, 3, c);
        tft.fillRect(cx + 6, cy - 4, 3, 9, c);
      } else if (id == IC_VOLDN) {
        tft.fillRect(cx + 3, cy - 1, 9, 3, c);
      } else { // X
        tft.drawLine(cx + 3, cy - 4, cx + 10, cy + 3, c);
        tft.drawLine(cx + 4, cy - 4, cx + 11, cy + 3, c);
        tft.drawLine(cx + 10, cy - 4, cx + 3, cy + 3, c);
        tft.drawLine(cx + 11, cy - 4, cx + 4, cy + 3, c);
      }
      break;
    case IC_TERM:
      tft.drawRoundRect(cx - 11, cy - 9, 23, 19, 2, c);
      tft.drawLine(cx - 7, cy - 4, cx - 3, cy, c);
      tft.drawLine(cx - 3, cy, cx - 7, cy + 4, c);
      tft.fillRect(cx, cy + 3, 7, 2, c);
      break;
    case IC_WEB:
      tft.drawCircle(cx, cy, 9, c);
      tft.drawFastHLine(cx - 9, cy, 19, c);
      tft.drawEllipse(cx, cy, 4, 9, c);
      break;
    case IC_FILES:
      tft.fillRect(cx - 10, cy - 7, 9, 4, c);   // tab
      tft.fillRect(cx - 10, cy - 4, 21, 12, c); // body
      break;
    case IC_CODE:
      tft.drawLine(cx - 4, cy - 6, cx - 10, cy, c);
      tft.drawLine(cx - 10, cy, cx - 4, cy + 6, c);
      tft.drawLine(cx + 4, cy - 6, cx + 10, cy, c);
      tft.drawLine(cx + 10, cy, cx + 4, cy + 6, c);
      tft.drawLine(cx + 2, cy - 8, cx - 2, cy + 8, c);
      break;
    case IC_MUSIC:
      tft.fillCircle(cx - 4, cy + 5, 4, c);
      tft.fillRect(cx - 1, cy - 8, 2, 14, c);
      tft.fillTriangle(cx + 1, cy - 8, cx + 8, cy - 5, cx + 1, cy - 2, c);
      break;
    case IC_MAIL:
      tft.drawRect(cx - 10, cy - 7, 21, 15, c);
      tft.drawLine(cx - 10, cy - 7, cx, cy + 1, c);
      tft.drawLine(cx + 10, cy - 7, cx, cy + 1, c);
      break;
    case IC_SHOT:
      tft.fillRect(cx - 4, cy - 8, 9, 4, c); // viewfinder hump
      tft.fillRoundRect(cx - 10, cy - 5, 21, 13, 2, c);
      tft.fillCircle(cx, cy + 1, 3, COL_BTN); // lens hole
      break;
    case IC_CALC:
      tft.drawRect(cx - 8, cy - 10, 17, 21, c);
      tft.fillRect(cx - 5, cy - 7, 11, 4, c); // display
      for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++)
          tft.fillRect(cx - 5 + col * 4, cy + r * 4, 2, 2, c);
      break;
    case IC_LOCK:
      tft.drawCircle(cx, cy - 3, 5, c); // shackle (bottom hidden by body)
      tft.drawCircle(cx, cy - 3, 4, c);
      tft.fillRoundRect(cx - 8, cy - 2, 17, 12, 2, c);
      tft.fillCircle(cx, cy + 3, 2, COL_BTN); // keyhole
      break;
    case IC_MIC:
      tft.fillRoundRect(cx - 4, cy - 10, 9, 13, 4, c); // capsule
      tft.drawFastVLine(cx - 8, cy - 3, 5, c);         // U bracket
      tft.drawFastVLine(cx + 8, cy - 3, 5, c);
      tft.drawLine(cx - 8, cy + 2, cx - 4, cy + 6, c);
      tft.drawLine(cx + 8, cy + 2, cx + 4, cy + 6, c);
      tft.drawFastHLine(cx - 4, cy + 6, 9, c);
      tft.fillRect(cx - 1, cy + 6, 3, 3, c);  // stem
      tft.fillRect(cx - 6, cy + 9, 13, 2, c); // base
      break;
    case IC_CAM:
      tft.fillRoundRect(cx - 11, cy - 6, 15, 13, 2, c); // body
      tft.fillTriangle(cx + 5, cy, cx + 11, cy - 6, cx + 11, cy + 6, c);
      break;
    case IC_CHAT:
      tft.fillRoundRect(cx - 10, cy - 9, 21, 14, 4, c); // bubble
      tft.fillTriangle(cx - 6, cy + 3, cx - 1, cy + 3, cx - 7, cy + 9, c);
      break;
    case IC_GAME:
      tft.fillRoundRect(cx - 11, cy - 6, 23, 13, 5, c);
      tft.fillRect(cx - 9, cy - 1, 7, 3, COL_BTN); // d-pad
      tft.fillRect(cx - 7, cy - 3, 3, 7, COL_BTN);
      tft.fillCircle(cx + 5, cy + 1, 1, COL_BTN); // buttons
      tft.fillCircle(cx + 8, cy - 2, 1, COL_BTN);
      break;
    case IC_REC:
      tft.drawCircle(cx, cy, 9, c);
      tft.fillCircle(cx, cy, 5, c);
      break;
    case IC_STOP:
      tft.fillRoundRect(cx - 8, cy - 8, 17, 17, 2, c);
      break;
    case IC_STAR: // 4-point sparkle
      tft.fillTriangle(cx, cy - 11, cx - 3, cy, cx + 3, cy, c);
      tft.fillTriangle(cx, cy + 11, cx - 3, cy, cx + 3, cy, c);
      tft.fillTriangle(cx - 11, cy, cx, cy - 3, cx, cy + 3, c);
      tft.fillTriangle(cx + 11, cy, cx, cy - 3, cx, cy + 3, c);
      break;
    case IC_HEART:
      tft.fillCircle(cx - 4, cy - 3, 5, c);
      tft.fillCircle(cx + 4, cy - 3, 5, c);
      tft.fillTriangle(cx - 8, cy - 1, cx + 8, cy - 1, cx, cy + 9, c);
      break;
    case IC_SUN:
      tft.fillCircle(cx, cy, 5, c);
      tft.fillRect(cx - 1, cy - 11, 3, 4, c); // N/S/E/W rays
      tft.fillRect(cx - 1, cy + 8, 3, 4, c);
      tft.fillRect(cx - 11, cy - 1, 4, 3, c);
      tft.fillRect(cx + 8, cy - 1, 4, 3, c);
      tft.drawLine(cx - 8, cy - 8, cx - 5, cy - 5, c); // diagonal rays
      tft.drawLine(cx + 8, cy - 8, cx + 5, cy - 5, c);
      tft.drawLine(cx - 8, cy + 8, cx - 5, cy + 5, c);
      tft.drawLine(cx + 8, cy + 8, cx + 5, cy + 5, c);
      break;
    case IC_MOON:
      tft.fillCircle(cx - 1, cy, 9, c);
      tft.fillCircle(cx + 4, cy - 3, 8, COL_BTN); // crescent bite
      break;
    case IC_SEARCH:
      tft.drawCircle(cx - 3, cy - 3, 6, c);
      tft.drawCircle(cx - 3, cy - 3, 5, c);
      tft.drawLine(cx + 1, cy + 1, cx + 8, cy + 8, c); // handle
      tft.drawLine(cx + 2, cy, cx + 9, cy + 7, c);
      break;
    case IC_BELL:
      tft.fillCircle(cx, cy - 3, 6, c); // dome
      tft.fillRect(cx - 6, cy - 3, 13, 6, c);
      tft.fillRect(cx - 9, cy + 3, 19, 2, c); // lip
      tft.fillCircle(cx, cy + 7, 2, c);       // clapper
      break;
    case IC_HOME:
      tft.fillTriangle(cx - 11, cy - 1, cx + 11, cy - 1, cx, cy - 10, c);
      tft.fillRect(cx - 7, cy - 1, 15, 10, c);
      tft.fillRect(cx - 2, cy + 3, 5, 6, COL_BTN); // door
      break;
    case IC_POWER:
      tft.drawCircle(cx, cy + 1, 8, c);
      tft.drawCircle(cx, cy + 1, 7, c);
      tft.fillRect(cx - 3, cy - 8, 7, 5, COL_BTN); // gap at top
      tft.fillRect(cx - 1, cy - 9, 3, 9, c);       // bar
      break;
    default: // IC_GEAR
      tft.fillRect(cx - 1, cy - 11, 3, 5, c);
      tft.fillRect(cx - 1, cy + 7, 3, 5, c);
      tft.fillRect(cx - 11, cy - 1, 5, 3, c);
      tft.fillRect(cx + 7, cy - 1, 5, 3, c);
      tft.fillCircle(cx, cy, 7, c);
      tft.fillCircle(cx, cy, 3, COL_BTN); // hub hole
      break;
  }
}

void drawHeader() {
  int w = SCREEN_W - 2 * PAD;
  int h = HEADER_H - 2 * PAD;
  tft.fillRoundRect(PAD, PAD, w, h, 6, COL_BTN);
  bool active = nowState == 'p' || nowState == 'a';
  uint16_t c = nowState == 'p' ? COL_PLAY : (nowState == 'a' ? COL_PAUSE : COL_DIM);
  int cx = PAD + 18, cy = HEADER_H / 2;
  if (nowState == 'p') {
    tft.fillTriangle(cx - 6, cy - 8, cx - 6, cy + 8, cx + 8, cy, c);
  } else if (nowState == 'a') {
    tft.fillRect(cx - 6, cy - 8, 5, 17, c);
    tft.fillRect(cx + 2, cy - 8, 5, 17, c);
  } else {
    tft.fillRoundRect(cx - 7, cy - 7, 15, 15, 2, c);
  }
  int tx = PAD + 34;
  tft.setTextDatum(ML_DATUM);
  if (active && nowTitle[0]) {
    tft.setTextColor(COL_TEXT, COL_BTN);
    tft.drawString(nowTitle, tx, PAD + 11, 2);
    tft.setTextColor(COL_DIM, COL_BTN);
    tft.drawString(nowArtist, tx, PAD + 28, 2);
  } else {
    tft.setTextColor(COL_DIM, COL_BTN);
    tft.drawString(active ? "(no track info)" : "No player", tx, cy, 2);
  }
}

void buttonRect(int id, int &x, int &y, int &w, int &h) {
  int col = id % COLS;
  int row = id / COLS;
  x = col * CELL_W + PAD;
  y = HEADER_H + row * CELL_H + PAD;
  w = CELL_W - 2 * PAD;
  h = CELL_H - 2 * PAD;
}

void drawButton(int id, bool pressed) {
  int x, y, w, h;
  buttonRect(id, x, y, w, h);
  const Button &b = buttons[id];
  tft.fillRoundRect(x, y, w, h, 6, pressed ? b.accent : COL_BTN);
  tft.drawRoundRect(x, y, w, h, 6, b.accent);
  drawIcon(b.icon, x + 20, y + h / 2, pressed ? COL_BG : b.accent);
  tft.setTextColor(pressed ? COL_BG : COL_TEXT);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(b.label, x + 40, y + h / 2, 2);
}

int hitTest(int x, int y) {
  if (y < HEADER_H) return -1; // header is not a button
  int col = x / CELL_W;
  int row = (y - HEADER_H) / CELL_H;
  if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return -1;
  return row * COLS + col;
}

// Read touch and convert to portrait rotation-0 screen coords.
bool readTouch(int &sx, int &sy) {
  if (!ts.tirqTouched() || !ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.z < 300) return false;
  // Reject rail readings from SPI glitches / boot noise
  if (p.x < 80 || p.x > 4015 || p.y < 80 || p.y > 4015) return false;
  long x = map(p.y, TOUCH_RY_MAX, TOUCH_RY_MIN, 0, 239);
  long y = map(p.x, TOUCH_RX_MIN, TOUCH_RX_MAX, 0, 319);
  sx = constrain(x, 0, 239);
  sy = constrain(y, 0, 319);
  return true;
}

IconId iconByName(const char *name) {
  for (int i = 0; i < IC_COUNT; i++)
    if (strcasecmp(name, ICON_NAMES[i]) == 0) return (IconId)i;
  return IC_GEAR;
}

uint16_t rgb565FromHex(const char *hex) {
  long v = strtol(hex, nullptr, 16);
  uint8_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// CFG:<id>:<icon>:<label>:<rrggbb>
void handleCfg(char *args) {
  char *idStr = strtok(args, ":");
  char *icon = strtok(nullptr, ":");
  char *label = strtok(nullptr, ":");
  char *color = strtok(nullptr, ":");
  if (!idStr || !icon || !label || !color) {
    Serial.println("ERR:CFG format is CFG:<id>:<icon>:<label>:<rrggbb>");
    return;
  }
  int id = atoi(idStr);
  if (id < 0 || id >= NUM_BUTTONS) {
    Serial.printf("ERR:button id %d out of range 0-%d\n", id, NUM_BUTTONS - 1);
    return;
  }
  Button &b = buttons[id];
  b.icon = iconByName(icon);
  strncpy(b.label, label, LABEL_MAX);
  b.label[LABEL_MAX] = 0;
  b.accent = rgb565FromHex(color);
  drawButton(id, id == heldButton);
  Serial.printf("OK:CFG %d\n", id);
}

// NOW:<state>:<title>:<artist> — state is play/pause/stop; title/artist may
// be empty, so parse with strchr (strtok would collapse empty fields).
void handleNow(char *args) {
  char *t = strchr(args, ':');
  if (t) *t++ = 0;
  char *a = t ? strchr(t, ':') : nullptr;
  if (a) *a++ = 0;
  nowState = (strcmp(args, "play") == 0) ? 'p'
           : (strcmp(args, "pause") == 0) ? 'a' : 's';
  strncpy(nowTitle, t ? t : "", NOW_MAX);
  nowTitle[NOW_MAX] = 0;
  strncpy(nowArtist, a ? a : "", NOW_MAX);
  nowArtist[NOW_MAX] = 0;
  drawHeader();
  Serial.println("OK:NOW");
}

void handleLine(char *line) {
  if (strncmp(line, "CFG:", 4) == 0) handleCfg(line + 4);
  else if (strncmp(line, "NOW:", 4) == 0) handleNow(line + 4);
  else if (strcmp(line, "PING") == 0) Serial.println("STREAMDECK READY 10");
}

void pollSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (lineLen) {
        lineBuf[lineLen] = 0;
        handleLine(lineBuf);
        lineLen = 0;
      }
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = ch;
    } else {
      lineLen = 0; // overlong line, drop it
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  touchSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSpi);
  ts.setRotation(1); // raw passthrough; mapping handled in readTouch()

  tft.init();
  tft.setRotation(0); // portrait
  tft.fillScreen(COL_BG);

  drawHeader();
  for (int i = 0; i < NUM_BUTTONS; i++) drawButton(i, false);

  Serial.println("STREAMDECK READY 10");
}

void loop() {
  pollSerial();

  int x, y;
  bool touched = readTouch(x, y);

  if (touched) {
    int id = hitTest(x, y);
    if (!wasTouched && heldButton < 0) {
      pendingButton = id; // first sighting; confirm next pass
    } else if (pendingButton >= 0 && heldButton < 0) {
      if (id == pendingButton) { // confirmed press
        heldButton = id;
        drawButton(id, true);
        Serial.print("BTN:");
        Serial.println(id);
      }
      pendingButton = -1;
    }
  } else {
    if (wasTouched && heldButton >= 0) {
      drawButton(heldButton, false);
      heldButton = -1;
    }
    pendingButton = -1;
  }

  wasTouched = touched;
  delay(20);
}
