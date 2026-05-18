#include <M5Dial.h>
#include <LittleFS.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

M5Canvas spr(&M5Dial.Display);

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple devices
// in one room are distinguishable in the desktop picker.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
const int W = 240, H = 240;
const int CX = W / 2;
const int CY = H / 2;

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → setBrightness 51..255
bool    btnLong     = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtn = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Approval flow: when a prompt is pending the encoder picks between
// APPROVE (0) and DENY (1); short-press sends the highlighted decision.
uint8_t  promptChoice = 0;

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

uint32_t promptArrivedMs = 0;

static void applyBrightness() {
  M5Dial.Display.setBrightness(51 + brightLevel * 51);
}

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    M5Dial.Display.wakeup();
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5Dial.Speaker.tone(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  spr.fillSprite(0x0000);
  characterInvalidate();
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "sound", "bluetooth", "wifi", "transcript", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 8;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      s.bt = !s.bt;
      break;
    case 3: s.wifi = !s.wifi; break;   // stored only — no WiFi stack linked
    case 4: s.hud = !s.hud; break;
    case 5: nextPet(); return;
    case 6: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 7: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS wipe + filesystem format + BLE bonds.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* rotLbl = "rotate", const char* pressLbl = "press") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  spr.setCursor(mx + 8, hy); spr.print(rotLbl);
  spr.setCursor(mx + mw / 2 + 4, hy); spr.print(pressLbl);
}

// Centered panel for menus/settings on the round face. Avoids the ~26px
// corner wedges by capping width at 200 and centering.
static void drawPanel(const Palette& p, int mh, uint16_t border) {
  int mw = 200;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 6, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 6, border);
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = 200, mh = 16 + SETTINGS_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 6, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 6, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.hud };
  for (int i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 10, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - 50, my + 8 + i * 14);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 4) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 5) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "scroll", "select");
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 200, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 6, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 6, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 10, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "scroll", "select");
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: M5Dial.Display.sleep(); ESP.deepSleep(0); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 200, mh = 16 + MENU_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 6, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 6, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 10, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "scroll", "select");
}

// Cache the RTC at 1Hz so reads in the loop don't pound the I2C bus.
static m5::rtc_datetime_t _clk;
uint32_t                  _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool               _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = M5Dial.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
  _clk = M5Dial.Rtc.getDateTime();
}

// Clock face: shown when charging on USB with nothing else going on.
// 240×240 round face: HH:MM big in the center, seconds + date below.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clk.date.weekDay % 7; }
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clk.time.hours, _clk.time.minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clk.time.seconds);
  uint8_t mi = (_clk.date.month >= 1 && _clk.date.month <= 12) ? _clk.date.month - 1 : 0;
  char dl[12]; snprintf(dl, sizeof(dl), "%s %s %02u",
                       DOW[clockDow()], MON[mi], _clk.date.date);

  // Clear lower half of the face — pet sleeps in the upper half via peek mode
  spr.fillRect(0, 130, W, H - 130, p.bg);
  spr.setTextDatum(middle_center);
  spr.setTextSize(4); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, 165);
  spr.setTextSize(2); spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, 195);
  spr.setTextSize(1);                                     spr.drawString(dl, CX, 215);
  spr.setTextDatum(top_left);
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

// Persistent screen-level title row matching the PET header.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(24, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 50, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(24, y); spr.print(section);
  y += 12;
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextDatum(middle_center);
  spr.setTextColor(p.textDim, p.bg);
  spr.drawString("BLUETOOTH PAIRING", CX, 60);
  spr.drawString("enter on desktop:", CX, H - 60);
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.drawString(b, CX, CY);
  spr.setTextDatum(top_left);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 90;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[40]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(24, y); spr.print(b); y += 9;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += 4;
    ln("Sleep when idle,");
    ln("wake when working,");
    ln("alert on approvals.");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("Press to approve");
    ln("from this device.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "CONTROLS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("rotate");
    spr.setTextColor(p.textDim, p.bg); ln(" scroll / select");
    ln(" approve <-> deny");
    y += 4;
    spr.setTextColor(p.text, p.bg);    ln("press");
    spr.setTextColor(p.textDim, p.bg); ln(" next screen");
    ln(" confirm / send");
    y += 4;
    spr.setTextColor(p.text, p.bg);    ln("hold");
    spr.setTextColor(p.textDim, p.bg); ln(" open menu");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int pct = M5Dial.Power.getBatteryLevel();
    int vBat = M5Dial.Power.getBatteryVoltage();
    bool usb = M5Dial.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
    bool noBat = pct < 0;
    if (pct < 0) pct = 0;
    if (vBat < 0) vBat = 0;

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(24, y);
    if (noBat) spr.print(usb ? "USB" : "---");
    else spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(usb ? GREEN : p.textDim, p.bg);
    spr.setCursor(80, y + 4);
    spr.print(noBat ? "" : (usb ? "charging" : "battery"));
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    if (!noBat) ln("  battery  %d.%02dV", vBat/1000, (vBat%1000)/10);
    y += 6;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner   %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime  %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap    %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright  %u/4", brightLevel);
    ln("  bt      %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setTextSize(2);
    spr.setCursor(24, y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr.setTextSize(1);
    y += 20;

    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 6;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Open Claude desktop");
      ln(" > Developer");
      ln(" > Hardware Buddy");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    spr.setTextColor(p.text, p.bg);
    ln(" Felix Rieseberg");
    y += 8;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    spr.setTextColor(p.text, p.bg);
    ln(" github.com/anthropics");
    ln(" /claude-desktop-buddy");
    y += 8;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    ln(" M5 Dial");
    ln(" ESP32-S3");
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
static uint8_t wrapInto(const char* in, char out[][32], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

// Full-screen radial approval overlay: tool name + hint at top, APPROVE
// and DENY pills at the bottom, encoder picks between them.
static void drawApproval() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);

  spr.setTextSize(1);
  spr.setTextDatum(middle_center);

  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  spr.setTextColor(waited >= 10 ? HOT : p.textDim, p.bg);
  char waitBuf[24];
  snprintf(waitBuf, sizeof(waitBuf), "approve? %lus", (unsigned long)waited);
  spr.drawString(waitBuf, CX, 50);

  // Tool name large in upper third
  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 12 ? 2 : 1);
  spr.drawString(tama.promptTool, CX, 80);
  spr.setTextSize(1);

  // Hint wraps to two lines centered
  spr.setTextColor(p.textDim, p.bg);
  char h1[24] = {0}, h2[24] = {0};
  int hlen = strlen(tama.promptHint);
  int take1 = hlen > 22 ? 22 : hlen;
  strncpy(h1, tama.promptHint, take1); h1[take1] = 0;
  if (hlen > 22) { int take2 = hlen - 22 > 22 ? 22 : hlen - 22; strncpy(h2, tama.promptHint + 22, take2); h2[take2] = 0; }
  spr.drawString(h1, CX, 110);
  if (h2[0]) spr.drawString(h2, CX, 122);

  spr.setTextDatum(top_left);

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextDatum(middle_center);
    spr.drawString("sent...", CX, 180);
    spr.setTextDatum(top_left);
    return;
  }

  // Two pills, left=APPROVE, right=DENY. Selected one filled, other outlined.
  int pillW = 88, pillH = 36;
  int leftX  = CX - pillW - 6;
  int rightX = CX + 6;
  int pillY  = 170;

  bool approveSel = (promptChoice == 0);
  uint16_t okCol  = GREEN;
  uint16_t noCol  = HOT;

  if (approveSel) {
    spr.fillRoundRect(leftX, pillY, pillW, pillH, 8, okCol);
    spr.drawRoundRect(rightX, pillY, pillW, pillH, 8, noCol);
  } else {
    spr.drawRoundRect(leftX, pillY, pillW, pillH, 8, okCol);
    spr.fillRoundRect(rightX, pillY, pillW, pillH, 8, noCol);
  }

  spr.setTextDatum(middle_center);
  spr.setTextSize(2);
  spr.setTextColor(approveSel ? (uint16_t)0x0000 : okCol, p.bg);
  spr.drawString("OK", leftX + pillW / 2, pillY + pillH / 2);
  spr.setTextColor(approveSel ? noCol : (uint16_t)0x0000, p.bg);
  spr.drawString("NO", rightX + pillW / 2, pillY + pillH / 2);
  spr.setTextSize(1);

  spr.setTextColor(p.textDim, p.bg);
  spr.drawString("rotate \xb7 press", CX, 220);
  spr.setTextDatum(top_left);
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = 90;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 16;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(28, y - 2); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? (uint16_t)0xF800 : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(82 + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  spr.setCursor(28, y - 2); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 70 + i * 9;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  spr.setCursor(28, y - 2); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? (uint16_t)0x07FF : (en >= 2) ? (uint16_t)0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 82 + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 24;
  spr.fillRoundRect(28, y - 2, 48, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(34, y + 1); spr.printf("Lv %u", stats().level);

  y += 20;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(28, y);
  spr.printf("approved %u", stats().approvals);
  spr.setCursor(28, y + 10);
  spr.printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(28, y + 20);
  spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(28, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + 30);
  tokFmt("today    ", tama.tokensToday, y + 40);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 90;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 14;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(28, y); spr.print(s); y += 9;
  };
  auto gap = [&]() { y += 4; };

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up + confetti"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " refills over time"); gap();

  ln(p.textDim, "idle 30s = off");
  ln(p.textDim, "any press = wake"); gap();

  ln(p.textDim, "press: next  hold: menu");
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 88;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(24, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 50, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  const int SHOW = 3, LH = 9, WIDTH = 28;
  const int AREA = SHOW * LH + 6;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.setTextSize(1);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setTextDatum(middle_center);
    spr.drawString(tama.msg, CX, H - 12);
    spr.setTextDatum(top_left);
    return;
  }

  static char disp[32][32];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(20, H - AREA + 2 + i * LH);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - 28, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
}

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, /*encoder=*/true, /*rfid=*/false);
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] claude desktop buddy");

  startBt();
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();

  spr.createSprite(W, H);
  characterInit(nullptr);
  gifAvailable = characterLoaded();
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(middle_center);
    spr.setTextSize(2);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, CX, CY - 14);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), CX, CY + 14);
    } else {
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", CX, CY - 14);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", CX, CY + 14);
    }
    spr.setTextDatum(top_left); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  M5Dial.update();
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking, hold sleep for 12s so users see the wake-up animation.
  // Urgent states override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // Prompt arrival: beep, reset response flag, default to APPROVE highlighted
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      promptChoice = 0;   // default APPROVE
      wake();
      beep(1200, 80);
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Encoder: rotation since last loop. M5Dial encoder reports 4 counts per
  // detent on most units. Divide by 4 to get logical clicks; rounding
  // small jitter to 0 keeps the selection from flickering between detents.
  static long encPrev = 0;
  long encNow = M5Dial.Encoder.read();
  int encDelta = (int)((encNow - encPrev) / 4);
  if (encDelta != 0) {
    encPrev += (long)encDelta * 4;
    wake();
  }

  // Single physical button — the encoder press.
  bool btnPressed = M5Dial.BtnA.isPressed();
  if (btnPressed) {
    if (screenOff) swallowBtn = true;
    wake();
  }

  // Long-press detection (600ms). Fires once per hold.
  if (M5Dial.BtnA.pressedFor(600) && !btnLong && !swallowBtn) {
    btnLong = true;
    beep(800, 60);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }

  // Short-press release: act on the current context.
  if (M5Dial.BtnA.wasReleased()) {
    if (!btnLong && !swallowBtn) {
      if (inPrompt) {
        char cmd[96];
        const char* dec = (promptChoice == 0) ? "once" : "deny";
        snprintf(cmd, sizeof(cmd),
                 "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}",
                 tama.promptId, dec);
        sendCmd(cmd);
        responseSent = true;
        if (promptChoice == 0) {
          uint32_t tookS = (millis() - promptArrivedMs) / 1000;
          statsOnApproval(tookS);
          beep(2400, 60);
          if (tookS < 5) triggerOneShot(P_HEART, 2000);
        } else {
          statsOnDenial();
          beep(600, 60);
        }
      } else if (resetOpen) {
        beep(2400, 30);
        applyReset(resetSel);
      } else if (settingsOpen) {
        beep(2400, 30);
        applySetting(settingsSel);
      } else if (menuOpen) {
        beep(2400, 30);
        menuConfirm();
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnLong = false;
    swallowBtn = false;
  }

  // Encoder rotation: route to whatever's on screen.
  if (encDelta != 0) {
    if (inPrompt) {
      // toggle between APPROVE (0) and DENY (1)
      int next = (int)promptChoice + (encDelta > 0 ? 1 : -1);
      if (next < 0) next = 1;
      if (next > 1) next = 0;
      promptChoice = (uint8_t)next;
      beep(1800, 20);
    } else if (resetOpen) {
      int next = ((int)resetSel + encDelta) % (int)RESET_N;
      if (next < 0) next += RESET_N;
      resetSel = (uint8_t)next;
      resetConfirmIdx = 0xFF;
      beep(1800, 20);
    } else if (settingsOpen) {
      int next = ((int)settingsSel + encDelta) % (int)SETTINGS_N;
      if (next < 0) next += SETTINGS_N;
      settingsSel = (uint8_t)next;
      beep(1800, 20);
    } else if (menuOpen) {
      int next = ((int)menuSel + encDelta) % (int)MENU_N;
      if (next < 0) next += MENU_N;
      menuSel = (uint8_t)next;
      beep(1800, 20);
    } else if (displayMode == DISP_PET) {
      petPage = (petPage + (encDelta > 0 ? 1 : PET_PAGES - 1)) % PET_PAGES;
      applyDisplayMode();
      beep(1800, 20);
    } else if (displayMode == DISP_INFO) {
      infoPage = (infoPage + (encDelta > 0 ? 1 : INFO_PAGES - 1)) % INFO_PAGES;
      beep(1800, 20);
    } else {
      // normal home: scroll transcript
      int next = (int)msgScroll + (encDelta > 0 ? 1 : -1);
      if (next < 0) next = 0;
      if (next > 30) next = 30;
      msgScroll = (uint8_t)next;
    }
  }

  // Charging clock: takes over the home screen when on USB power, nothing
  // urgent, and the RTC has been set by the bridge.
  clockRefreshRtc();
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;

  static bool wasClocking = false;
  if (clocking != wasClocking) {
    if (clocking) characterSetPeek(true);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clk.time.hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  if (screenOff) {
    // skip render
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setTextDatum(middle_center);
      spr.drawString("installing", CX, CY - 14);
      char prog[24];
      snprintf(prog, sizeof(prog), "%luK / %luK", done/1024, total/1024);
      spr.drawString(prog, CX, CY);
      spr.setTextDatum(top_left);
      int barW = 180;
      int barX = (W - barW) / 2;
      spr.drawRect(barX, CY + 16, barW, 10, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(barX + 1, CY + 17, fill - 1, 8, p.body);
      }
    } else {
      spr.setTextDatum(middle_center);
      spr.drawString("no character loaded", CX, CY);
      spr.setTextDatum(top_left);
    }
  }

  if (!screenOff) {
    if (blePasskey()) drawPasskey();
    else if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (settings().hud) drawHUD();
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
    spr.pushSprite(0, 0);
  }

  // Auto-sleep disabled: device is mains-powered, keep the display on.

  delay(screenOff ? 100 : 16);
}
