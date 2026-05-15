#include <Arduino.h>
#include <EEPROM.h>

static constexpr uint8_t PIN_SW1  = 22;
static constexpr uint8_t PIN_LED3 = 21;
static constexpr uint8_t PIN_LED2 = 20;
static constexpr uint8_t PIN_LED1 = 19;
static constexpr uint8_t PIN_SW2  = 18;
static constexpr uint8_t PIN_SW3  = 17;
static constexpr uint8_t PIN_POT1 = 14;
static constexpr uint8_t PIN_POT2 = 15;
static constexpr uint8_t PIN_POT3 = 16;

static constexpr uint8_t NUM_POTS = 3;
static constexpr uint8_t NUM_SWITCHES = 3;

static constexpr uint16_t EEPROM_MAGIC = 0x4D41;
static constexpr uint16_t CONFIG_VERSION = 3;

static constexpr uint16_t ADC_MAX_VALUE = 4095;
static constexpr uint32_t DEFAULT_AUTOSAVE_DELAY_MS = 120000UL;

static constexpr uint32_t LED1_DARK_MS = 10;
static constexpr uint32_t LED2_FLASH_MS = 30;
static constexpr uint32_t LED3_FLASH_MS = 30;

static constexpr uint32_t DEBOUNCE_MS = 25;

static constexpr float POT_FILTER_ALPHA = 0.12f;
static constexpr uint16_t POT_RAW_DEADBAND = 4;
static constexpr uint8_t MIDI_DEADBAND = 1;

enum SwitchMode : uint8_t {
  SWITCH_MOMENTARY = 0,
  SWITCH_TOGGLE = 1
};

struct PotConfig {
  uint8_t midiMin;
  uint8_t midiMax;
  uint8_t sendChannel;
  uint8_t ccNumber;
  uint16_t analogMin;
  uint16_t analogMax;
};

struct SwitchConfig {
  uint8_t offValue;
  uint8_t onValue;
  uint8_t sendChannel;
  uint8_t ccNumber;
  uint8_t mode;
};

struct Config {
  uint16_t magic;
  uint16_t version;
  uint8_t receiveChannel;
  PotConfig pots[NUM_POTS];
  SwitchConfig switches[NUM_SWITCHES];
  uint32_t autosaveDelayMs;
  uint16_t crc;
};

Config cfg;

struct PotRuntime {
  uint8_t pin;
  float filtered;
  uint16_t lastRaw;
  int16_t lastMidi;
  bool calibrating;
  uint16_t calMin;
  uint16_t calMax;
};

struct SwitchRuntime {
  uint8_t pin;
  bool stablePressed;
  bool lastReading;
  uint32_t lastChangeMs;
  bool toggleState;
};

PotRuntime pots[NUM_POTS] = {
  {PIN_POT1, 0, 0, -1, false, ADC_MAX_VALUE, 0},
  {PIN_POT2, 0, 0, -1, false, ADC_MAX_VALUE, 0},
  {PIN_POT3, 0, 0, -1, false, ADC_MAX_VALUE, 0}
};

SwitchRuntime switches[NUM_SWITCHES] = {
  {PIN_SW1, false, false, 0, false},
  {PIN_SW2, false, false, 0, false},
  {PIN_SW3, false, false, 0, false}
};

bool configDirty = false;
uint32_t lastConfigChangeMs = 0;

uint32_t led1OffUntilMs = 0;
uint32_t led2OnUntilMs = 0;
uint32_t led3OnUntilMs = 0;

uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;

  while (len--) {
    crc ^= *data++;
    for (uint8_t i = 0; i < 8; i++) {
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
  }

  return crc;
}

uint16_t calcConfigCrc(Config &c) {
  uint16_t oldCrc = c.crc;
  c.crc = 0;
  uint16_t result = crc16(reinterpret_cast<const uint8_t*>(&c), sizeof(Config));
  c.crc = oldCrc;
  return result;
}

uint8_t clampMidi7(uint8_t v) {
  return v > 127 ? 127 : v;
}

uint8_t clampChannel(uint8_t v) {
  if (v < 1) return 1;
  if (v > 16) return 16;
  return v;
}

void configDump()
{
  Serial.println();
  Serial.println("========== CONFIG DUMP ==========");

  Serial.print("magic             : 0x");
  Serial.println(cfg.magic, HEX);

  Serial.print("version           : ");
  Serial.println(cfg.version);

  Serial.print("receiveChannel    : ");
  Serial.println(cfg.receiveChannel);

  Serial.print("autosaveDelayMs   : ");
  Serial.println(cfg.autosaveDelayMs);

  Serial.print("crc               : 0x");
  Serial.println(cfg.crc, HEX);

  Serial.println();

  // -------------------------------------------------
  // POTS
  // -------------------------------------------------

  Serial.println("--------------- POTS ---------------");

  for (uint8_t i = 0; i < NUM_POTS; i++)
  {
    const PotConfig& p = cfg.pots[i];

    Serial.print("Pot ");
    Serial.println(i);

    Serial.print("  midiMin      : ");
    Serial.println(p.midiMin);

    Serial.print("  midiMax      : ");
    Serial.println(p.midiMax);

    Serial.print("  sendChannel  : ");
    Serial.println(p.sendChannel);

    Serial.print("  ccNumber     : ");
    Serial.println(p.ccNumber);

    Serial.print("  analogMin    : ");
    Serial.println(p.analogMin);

    Serial.print("  analogMax    : ");
    Serial.println(p.analogMax);

    Serial.println();
  }

  // -------------------------------------------------
  // SWITCHES
  // -------------------------------------------------

  Serial.println("------------- SWITCHES -------------");

  for (uint8_t i = 0; i < NUM_SWITCHES; i++)
  {
    const SwitchConfig& s = cfg.switches[i];

    Serial.print("Switch ");
    Serial.println(i);

    Serial.print("  offValue     : ");
    Serial.println(s.offValue);

    Serial.print("  onValue      : ");
    Serial.println(s.onValue);

    Serial.print("  sendChannel  : ");
    Serial.println(s.sendChannel);

    Serial.print("  ccNumber     : ");
    Serial.println(s.ccNumber);

    Serial.print("  mode         : ");
    Serial.println(s.mode);

    Serial.println();
  }

  Serial.println("====================================");
  Serial.println();
}

void setDefaults() {
  memset(&cfg, 0, sizeof(cfg));

  cfg.magic = EEPROM_MAGIC;
  cfg.version = CONFIG_VERSION;
  cfg.receiveChannel = 1;
  cfg.autosaveDelayMs = DEFAULT_AUTOSAVE_DELAY_MS;

  for (uint8_t i = 0; i < NUM_POTS; i++) {
    cfg.pots[i].sendChannel = 1;
    cfg.pots[i].ccNumber = 20 + i;
    cfg.pots[i].midiMin = 0;
    cfg.pots[i].midiMax = 127;
    cfg.pots[i].analogMin = 0;
    cfg.pots[i].analogMax = ADC_MAX_VALUE;
  }

  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    cfg.switches[i].sendChannel = 1;
    cfg.switches[i].ccNumber = 23 + i;
    cfg.switches[i].offValue = 0;
    cfg.switches[i].onValue = 127;
    cfg.switches[i].mode = SWITCH_MOMENTARY;
  }
}


void saveConfig() {
  Serial.println("debug: saveConfig()");
  cfg.crc = calcConfigCrc(cfg);
  EEPROM.put(0, cfg);
  configDirty = false;
}

bool loadConfig() {
  Serial.println("debug: loadConfig()");

  EEPROM.get(0, cfg);

  if (cfg.magic != EEPROM_MAGIC) return false;
  if (cfg.version != CONFIG_VERSION) return false;
  if (calcConfigCrc(cfg) != cfg.crc) return false;
  if (cfg.receiveChannel < 1 || cfg.receiveChannel > 16) return false;

  return true;
}

void markConfigDirty() {
  configDirty = true;
  lastConfigChangeMs = millis();
}

void pulseMidiOutLed() {
  led1OffUntilMs = millis() + LED1_DARK_MS;
}

void pulseMidiInLed() {
  led2OnUntilMs = millis() + LED2_FLASH_MS;
}

void pulseMidiAdjustLed() {
  led3OnUntilMs = millis() + LED3_FLASH_MS;
}

void updateLeds() {
  digitalWrite(PIN_LED1, millis() < led1OffUntilMs ? LOW : HIGH);
  digitalWrite(PIN_LED2, millis() < led2OnUntilMs ? HIGH : LOW);
  //digitalWrite(PIN_LED3, millis() < led3OnUntilMs ? HIGH : LOW);
}

void sendCC(uint8_t cc, uint8_t value, uint8_t channel) {
  channel = clampChannel(channel);
  usbMIDI.sendControlChange(cc, value, channel);
  pulseMidiOutLed();
}

int mapPotToMidi(uint16_t raw, const PotConfig &pc) {
  uint16_t amin = pc.analogMin;
  uint16_t amax = pc.analogMax;

  if (amax <= amin + 2) {
    amin = 0;
    amax = ADC_MAX_VALUE;
  }

  raw = constrain(raw, amin, amax);

  long value = map(raw, amin, amax, pc.midiMin, pc.midiMax);
  return constrain(value, 0, 127);
}

void handlePots() {
  for (uint8_t i = 0; i < NUM_POTS; i++) {
    uint16_t raw = analogRead(pots[i].pin);

    if (pots[i].filtered <= 0.01f) {
      pots[i].filtered = raw;
    } else {
      pots[i].filtered += POT_FILTER_ALPHA * ((float)raw - pots[i].filtered);
    }

    uint16_t fraw = (uint16_t)(pots[i].filtered + 0.5f);

    if (pots[i].calibrating) {
      if (fraw < pots[i].calMin) pots[i].calMin = fraw;
      if (fraw > pots[i].calMax) pots[i].calMax = fraw;
    }

    if (abs((int)fraw - (int)pots[i].lastRaw) < POT_RAW_DEADBAND) {
      continue;
    }

    pots[i].lastRaw = fraw;

    int midiValue = mapPotToMidi(fraw, cfg.pots[i]);

    if (pots[i].lastMidi < 0 || abs(midiValue - pots[i].lastMidi) >= MIDI_DEADBAND) {
      pots[i].lastMidi = midiValue;
      Serial.printf("debug: sending pot %i CC %i value %i channel %i\n", i, cfg.pots[i].ccNumber, midiValue, cfg.pots[i].sendChannel);
      sendCC(cfg.pots[i].ccNumber, midiValue, cfg.pots[i].sendChannel);
    }
  }
}

void handleSwitches() {
  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    bool readingPressed = digitalRead(switches[i].pin) == LOW;

    if (readingPressed != switches[i].lastReading) {
      switches[i].lastReading = readingPressed;
      switches[i].lastChangeMs = millis();
    }

    if (millis() - switches[i].lastChangeMs < DEBOUNCE_MS) {
      continue;
    }

    if (readingPressed == switches[i].stablePressed) {
      continue;
    }

    switches[i].stablePressed = readingPressed;

    SwitchConfig &sc = cfg.switches[i];

    if (sc.mode == SWITCH_TOGGLE) {
      if (readingPressed) {
        switches[i].toggleState = !switches[i].toggleState;
        sendCC(sc.ccNumber, switches[i].toggleState ? sc.onValue : sc.offValue, sc.sendChannel);
      }
    } else {
      sendCC(sc.ccNumber, readingPressed ? sc.onValue : sc.offValue, sc.sendChannel);
    }
  }
}

void startCalibration(uint8_t potIndex) {
  if (potIndex >= NUM_POTS) return;

  Serial.println("debug: startCalibration");
  digitalWrite(PIN_LED3, HIGH);

  pots[potIndex].calibrating = true;
  pots[potIndex].calMin = ADC_MAX_VALUE;
  pots[potIndex].calMax = 0;
}

void stopCalibration(uint8_t potIndex) {
  if (potIndex >= NUM_POTS) return;

  Serial.println("debug: stopCalibration");

  digitalWrite(PIN_LED3, LOW);

  pots[potIndex].calibrating = false;

  if (pots[potIndex].calMax > pots[potIndex].calMin + 20) {
    cfg.pots[potIndex].analogMin = pots[potIndex].calMin;
    cfg.pots[potIndex].analogMax = pots[potIndex].calMax;
    markConfigDirty();
  }
}

void resetConfigAndSave() {
  Serial.println("debug: resetConfigAndSave()");
  setDefaults();
  saveConfig();
}

void handleConfigCC(uint8_t cc, uint8_t value) {
  bool handled = true;

  Serial.printf("debug: incoming CC %i value %i\n", cc, value);
  switch (cc) {
    case 100: cfg.pots[0].sendChannel = clampChannel(value); markConfigDirty(); break;
    case 101: cfg.pots[0].ccNumber = clampMidi7(value); markConfigDirty(); break;
    case 102: cfg.pots[0].midiMin = clampMidi7(value); markConfigDirty(); break;
    case 103: cfg.pots[0].midiMax = clampMidi7(value); markConfigDirty(); break;
    case 104: value == 127 ? startCalibration(0) : stopCalibration(0); break;

    case 105: cfg.pots[1].sendChannel = clampChannel(value); markConfigDirty(); break;
    case 106: cfg.pots[1].ccNumber = clampMidi7(value); markConfigDirty(); break;
    case 107: cfg.pots[1].midiMin = clampMidi7(value); markConfigDirty(); break;
    case 108: cfg.pots[1].midiMax = clampMidi7(value); markConfigDirty(); break;
    case 109: value == 127 ? startCalibration(1) : stopCalibration(1); break;

    case 110: cfg.pots[2].sendChannel = clampChannel(value); markConfigDirty(); break;
    case 111: cfg.pots[2].ccNumber = clampMidi7(value); markConfigDirty(); break;
    case 112: cfg.pots[2].midiMin = clampMidi7(value); markConfigDirty(); break;
    case 113: cfg.pots[2].midiMax = clampMidi7(value); markConfigDirty(); break;
    case 114: value == 127 ? startCalibration(2) : stopCalibration(2); break;

    case 115: cfg.switches[0].sendChannel = clampChannel(value); markConfigDirty(); break;
    case 116: cfg.switches[0].ccNumber = clampMidi7(value); markConfigDirty(); break;
    case 117: cfg.switches[0].mode = value >= 64 ? SWITCH_TOGGLE : SWITCH_MOMENTARY; markConfigDirty(); break;

    case 118: cfg.switches[1].sendChannel = clampChannel(value); markConfigDirty(); break;
    case 119: cfg.switches[1].ccNumber = clampMidi7(value); markConfigDirty(); break;
    case 120: cfg.switches[1].mode = value >= 64 ? SWITCH_TOGGLE : SWITCH_MOMENTARY; markConfigDirty(); break;

    case 121: cfg.switches[2].sendChannel = clampChannel(value); markConfigDirty(); break;
    case 122: cfg.switches[2].ccNumber = clampMidi7(value); markConfigDirty(); break;
    case 123: cfg.switches[2].mode = value >= 64 ? SWITCH_TOGGLE : SWITCH_MOMENTARY; markConfigDirty(); break;

    case 124:
      cfg.receiveChannel = clampChannel(value);
      markConfigDirty();
      break;

    case 125:
      if (value == 0) resetConfigAndSave();
      else if (value == 127) saveConfig();
      break;

    case 126:
      cfg.autosaveDelayMs = max<uint32_t>(1000UL, (uint32_t)value * 1000UL);
      markConfigDirty();
      break;

    case 127:
      configDump();
    break;

    default:
      handled = false;
      break;
  }

  if (handled) {
    pulseMidiInLed();
  }
}

void handleIncomingMidi() {
  while (usbMIDI.read()) {
    if (usbMIDI.getType() == usbMIDI.ControlChange &&
        usbMIDI.getChannel() == cfg.receiveChannel) {
      handleConfigCC(usbMIDI.getData1(), usbMIDI.getData2());
      sendCC(usbMIDI.getData1(), usbMIDI.getData2(), usbMIDI.getChannel()); // Midi through
    }
  }
}

void handleAutosave() {
  if (!configDirty) return;

  if (millis() - lastConfigChangeMs >= cfg.autosaveDelayMs) {
    saveConfig();
  }
}

void setup() {
  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);
  pinMode(PIN_LED3, OUTPUT);

  digitalWrite(PIN_LED1, HIGH);
  digitalWrite(PIN_LED2, HIGH);
  digitalWrite(PIN_LED3, HIGH);

  pinMode(PIN_SW1, INPUT_PULLUP);
  pinMode(PIN_SW2, INPUT_PULLUP);
  pinMode(PIN_SW3, INPUT_PULLUP);

  analogReadResolution(12);
  analogReadAveraging(8);

  if (!loadConfig()) {
    setDefaults();
    saveConfig();
  }

  for (uint8_t i = 0; i < NUM_POTS; i++) {
    pots[i].filtered = analogRead(pots[i].pin);
    pots[i].lastRaw = (uint16_t)pots[i].filtered;
    pots[i].lastMidi = -1;
  }

  for (uint8_t i = 0; i < NUM_SWITCHES; i++) {
    bool pressed = digitalRead(switches[i].pin) == LOW;
    switches[i].stablePressed = pressed;
    switches[i].lastReading = pressed;
    switches[i].lastChangeMs = millis();
  }

  Serial.begin(115200);
  Serial.println("Welcome to MAT's world!");

  delay(200);
  digitalWrite(PIN_LED2, LOW);
  digitalWrite(PIN_LED3, LOW);
}

void loop() {
  handleIncomingMidi();
  handlePots();
  handleSwitches();
  handleAutosave();
  updateLeds();
}