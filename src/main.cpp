#include <Arduino.h>

// DFPlayer Mini: ESP32 GPIO17 (TX2) -> RX, GPIO16 (RX2) <- TX
HardwareSerial dfPlayer(2);
const uint8_t volumePotPin = 34;
const uint8_t nextButtonPin = 25;
const uint16_t configuredTrackCount = 5;
int lastVolume = -1;
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
  sendCommand(0x03, currentTrack);
}

void updateVolumeFromPot()
{
  const int potValue = analogRead(volumePotPin);
  const int volume = map(potValue, 0, 4095, 0, 30);

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
  Serial.println("Commando-test:");
  testSdCard();
  testCommand("Status", 0x42);
  testCommand("Equalizer normaal", 0x07, 0);
  Serial.println("Test klaar. Draai aan de potmeter of druk op de knop.");
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

  static unsigned long lastStatusCheck = 0;
  if (millis() - lastStatusCheck >= 3000) {
    lastStatusCheck = millis();
    Serial.println("Status opvragen...");
    sendCommand(0x42); // DFPlayer: vraag huidige status op

    uint8_t responseCommand = 0;
    uint16_t responseParameter = 0;
    if (readResponse(responseCommand, responseParameter)) {
      Serial.println("VERBONDEN: geldige DFPlayer-reactie ontvangen.");
    } else {
      Serial.println("GEEN REACTIE: controleer 5V, GND en TX/RX.");
    }
  }

  delay(250);
}
