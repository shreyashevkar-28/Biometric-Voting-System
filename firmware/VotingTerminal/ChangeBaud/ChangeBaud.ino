#include <AltSoftSerial.h>
AltSoftSerial mySerial;

void setup() {
  Serial.begin(9600);
  mySerial.begin(57600);
  while(!Serial);
  Serial.println("Reconfiguring R307S baud rate to 9600...");

  // System Parameter Command: Header(6) | PID(1) | Len(2) | Cmd(1) | Param(1) | Value(1) | Checksum(2)
  // Param 4 = Baud Rate. Value 1 = 9600 baud (1 * 9600).
  uint8_t packet[] = {0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x05, 0x0E, 0x04, 0x01, 0x00, 0x1D};

  mySerial.write(packet, sizeof(packet));
  delay(500);

  Serial.println("Command dispatched. You MUST physically unplug the Arduino to power cycle the sensor.");
}
void loop() {}