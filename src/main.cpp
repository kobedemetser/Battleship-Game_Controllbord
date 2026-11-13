#include <Arduino.h>
#include <Wire.h>

// I²C test: TCA9548A (0x70)
//   kanaal 0 -> MPU6050  (0x68)
//   kanaal 1 -> MCP23017 (0x20)
// ESP32: SDA=GPIO23, SCL=GPIO22
const uint8_t MPU_CH = 0;
const uint8_t MCP_CH = 1;
const uint8_t TCA_ADDR = 0x70;
const uint8_t MCP_ADDR = 0x20;
const uint8_t MPU_ADDR = 0x68;
unsigned long lastI2cRead = 0;

bool tcaSelect(uint8_t channel)
{
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << channel);
  return Wire.endTransmission() == 0;
}

bool writeReg(uint8_t addr, uint8_t reg, uint8_t value)
{
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readRegs(uint8_t addr, uint8_t reg, uint8_t *data, uint8_t len)
{
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, len) != len) return false;
  for (uint8_t i = 0; i < len; ++i) data[i] = Wire.read();
  return true;
}

// Toont welke I²C-adressen antwoorden op het geselecteerde kanaal.
void scanChannel(uint8_t channel)
{
  Serial.printf("Kanaal %u:", channel);
  tcaSelect(channel);
  bool found = false;
  for (uint8_t addr = 1; addr < 127; ++addr) {
    if (addr == TCA_ADDR) continue;
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", addr);
      found = true;
    }
  }
  Serial.println(found ? "" : " niets gevonden");
}

void scanI2c()
{
  Serial.println("I2C test (TCA9548A):");
  Serial.println(tcaSelect(0) ? "TCA9548A: OK" : "TCA9548A: NIET GEVONDEN");
  scanChannel(0); // verwacht 0x68
  scanChannel(1); // verwacht 0x20
}

void initI2cTest()
{
  Wire.begin(23, 22);
  scanI2c();

  // MCP23017: alle pinnen ingang met pull-up.
  tcaSelect(MCP_CH);
  const bool mcp = writeReg(MCP_ADDR, 0x00, 0xFF) && writeReg(MCP_ADDR, 0x01, 0xFF) &&
                   writeReg(MCP_ADDR, 0x0C, 0xFF) && writeReg(MCP_ADDR, 0x0D, 0xFF);
  Serial.println(mcp ? "MCP23017: OK" : "MCP23017: NIET GEVONDEN");

  // MPU6050: uit slaapstand halen.
  tcaSelect(MPU_CH);
  Serial.println(writeReg(MPU_ADDR, 0x6B, 0x00) ? "MPU6050: OK" : "MPU6050: NIET GEVONDEN");
}

void updateI2cTest()
{
  if (millis() - lastI2cRead < 500) return;
  lastI2cRead = millis();

  uint8_t gpio[2];
  tcaSelect(MCP_CH);
  if (readRegs(MCP_ADDR, 0x12, gpio, 2)) {
    Serial.printf("MCP23017 A=0x%02X B=0x%02X | ", gpio[0], gpio[1]);
  } else {
    Serial.print("MCP23017 leesfout | ");
  }

  uint8_t raw[14];
  tcaSelect(MPU_CH);
  if (readRegs(MPU_ADDR, 0x3B, raw, sizeof(raw))) {
    auto v = [&](uint8_t i) { return (int16_t)((raw[i] << 8) | raw[i + 1]); };
    Serial.printf("MPU6050 accel[g] %.2f %.2f %.2f | gyro[deg/s] %.1f %.1f %.1f\n",
                  v(0) / 16384.0f, v(2) / 16384.0f, v(4) / 16384.0f,
                  v(8) / 131.0f, v(10) / 131.0f, v(12) / 131.0f);
  } else {
    Serial.println("MPU6050 leesfout");
  }
}

// DFPlayer Mini: ESP32 GPIO17 (TX2) -> RX, GPIO16 (RX2) <- TX
HardwareSerial dfPlayer(2);
const uint8_t volumePotPin = 34;
const uint8_t nextButtonPin = 25;
const uint16_t configuredTrackCount = 12;
const uint8_t startVolume = 15;
int lastVolume = -1;
int lastPotValue = -1000;
uint16_t trackCount = 0;
uint16_t currentTrack = 0;
unsigned long lastButtonPress = 0;
bool buttonWasPressed = false;

void sendCommand(uint8_t command, uint16_t parameter = 0)
{
  const uint8_t frame[] = {
      0x7E, 0xFF, 0x06, command, 0x00,
      static_cast<uint8_t>(parameter >> 8),
      static_cast<uint8_t>(parameter & 0xFF),
      0x00, 0x00, 0xEF};

  uint16_t checksum = 0;
  for (uint8_t i = 1; i <= 6; ++i) {
    checksum -= frame[i];
  }

  uint8_t packet[10];
  memcpy(packet, frame, sizeof(packet));
  packet[7] = static_cast<uint8_t>(checksum >> 8);
  packet[8] = static_cast<uint8_t>(checksum & 0xFF);
  dfPlayer.write(packet, sizeof(packet));
}

bool readResponse(uint8_t &responseCommand, uint16_t &responseParameter)
{
  uint8_t response[10];
  uint8_t count = 0;
  const unsigned long start = millis();

  while (millis() - start < 500) {
    if (!dfPlayer.available()) {
      delay(1);
      continue;
    }

    const uint8_t byte = dfPlayer.read();
    if (count == 0 && byte != 0x7E) {
      continue;
    }

    response[count++] = byte;
    if (count == sizeof(response)) {
      uint16_t checksum = 0;
      for (uint8_t i = 1; i <= 6; ++i) {
        checksum -= response[i];
      }
      const uint16_t received =
          (static_cast<uint16_t>(response[7]) << 8) | response[8];
      responseCommand = response[3];
      responseParameter =
          (static_cast<uint16_t>(response[5]) << 8) | response[6];
      return response[9] == 0xEF && checksum == received;
    }
  }

  return false;
}

void testCommand(const char *name, uint8_t command, uint16_t parameter = 0)
{
  Serial.print(name);
  Serial.print(": ");
  sendCommand(command, parameter);

  uint8_t responseCommand = 0;
  uint16_t responseParameter = 0;
  if (!readResponse(responseCommand, responseParameter)) {
    Serial.println("GEEN GELDIGE REACTIE");
    return;
  }

  Serial.print("REACTIE OK (commando 0x");
  if (responseCommand < 0x10) {
    Serial.print('0');
  }
  Serial.print(responseCommand, HEX);
  Serial.print(", waarde ");
  Serial.print(responseParameter);
  Serial.println(')');
}

void testSdCard()
{
  Serial.println("SD-kaart initialiseren...");
  sendCommand(0x3F);

  uint8_t responseCommand = 0;
  uint16_t responseParameter = 0;
  if (!readResponse(responseCommand, responseParameter)) {
    Serial.println("SD-kaart: GEEN REACTIE");
    return;
  }

  Serial.print("SD-kaart: REACTIE OK (broncode ");
  Serial.print(responseParameter);
  Serial.println(')');

  Serial.print("Bestanden op SD: ");
  sendCommand(0x48);

  if (readResponse(responseCommand, responseParameter)) {
    trackCount = configuredTrackCount;
    Serial.print("REACTIE OK, ");
    Serial.print(responseParameter);
    Serial.print(" gemeld, ");
    Serial.print(trackCount);
    Serial.println(" ingesteld voor selectie");
  } else {
    Serial.println("GEEN GELDIGE REACTIE");
  }
}

void selectNextTrack()
{
  if (trackCount == 0) {
    Serial.println("Geen nummers gevonden op de SD-kaart.");
    return;
  }

  currentTrack = currentTrack % trackCount + 1;
  Serial.print("Speel nummer ");
  Serial.println(currentTrack);
  sendCommand(0x08, currentTrack); // herhaal dit nummer continu
}

// Leest ongevraagde berichten van de DFPlayer zonder te blokkeren, zodat
// het afspelen niet verstoord wordt door extra commando's.
void logDfPlayerMessages()
{
  static uint8_t frame[10];
  static uint8_t count = 0;

  while (dfPlayer.available()) {
    const uint8_t byte = dfPlayer.read();
    if (count == 0 && byte != 0x7E) {
      continue;
    }

    frame[count++] = byte;
    if (count < sizeof(frame)) {
      continue;
    }
    count = 0;

    const uint8_t command = frame[3];
    const uint16_t parameter =
        (static_cast<uint16_t>(frame[5]) << 8) | frame[6];
    switch (command) {
    case 0x3D:
      Serial.print("Nummer ");
      Serial.print(parameter);
      Serial.println(" is afgelopen.");
      break;
    case 0x40:
      Serial.print("DFPlayer FOUT, code ");
      Serial.println(parameter);
      break;
    case 0x3A:
      Serial.println("SD-kaart verwijderd/ingestoken.");
      break;
    default:
      break;
    }
  }
}

void updateVolumeFromPot()
{
  const int potValue = analogRead(volumePotPin);
  // ADC-ruis negeren, anders worden er continu volume-commando's gestuurd.
  if (abs(potValue - lastPotValue) < 60) {
    return;
  }

  const int volume = map(potValue, 0, 4095, 0, 30);
  lastPotValue = potValue;

  if (volume == lastVolume) {
    return;
  }

  lastVolume = volume;
  sendCommand(0x06, volume);
  Serial.print("Volume ingesteld op ");
  Serial.println(volume);
}

void setup()
{
  Serial.begin(115200);
  dfPlayer.begin(9600, SERIAL_8N1, 16, 17);
  analogReadResolution(12);
  delay(1500);

  Serial.println();
  Serial.println("DFPlayer UART test gestart");
  Serial.println("ESP32 RX2=GPIO16, TX2=GPIO17");
  Serial.println("Volume-potmeter: 3.3V/GND en loper naar GPIO34.");
  Serial.println("Volgende-knop: GPIO25 naar GND.");
  pinMode(nextButtonPin, INPUT_PULLUP);
  Serial.println();
  initI2cTest();
  Serial.println();
  Serial.println("Commando-test:");
  testSdCard();
  testCommand("Status", 0x42);
  testCommand("Equalizer normaal", 0x07, 0);
  Serial.println("Test klaar. Draai aan de potmeter of druk op de knop.");

  // Starten op vast volume; de potmeter neemt pas over zodra hij bewogen wordt.
  lastPotValue = analogRead(volumePotPin);
  lastVolume = startVolume;
  sendCommand(0x06, startVolume);
  Serial.print("Volume ingesteld op ");
  Serial.println(startVolume);
  delay(100);
  selectNextTrack();
}

void loop()
{
  updateVolumeFromPot();

  const bool buttonIsPressed = digitalRead(nextButtonPin) == LOW;
  if (buttonIsPressed && !buttonWasPressed &&
      millis() - lastButtonPress > 250) {
    lastButtonPress = millis();
    selectNextTrack();
  }
  buttonWasPressed = buttonIsPressed;

  logDfPlayerMessages();
  updateI2cTest();

  // 'v' in de seriële monitor: I²C opnieuw scannen.
  while (Serial.available()) {
    if (Serial.read() == 'v') {
      scanI2c();
    }
  }

  delay(50);
}
