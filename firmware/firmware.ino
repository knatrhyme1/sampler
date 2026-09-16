// Стори 0.2: пустой проект на ESP32-S3, живой в Wokwi.
// Раз в секунду выводит сообщение в лог — доказательство, что плата
// запускается и код работает.

unsigned long lastBeat = 0;
const unsigned long BEAT_INTERVAL_MS = 1000;

void setup() {
  Serial.begin(115200);
  delay(200); // дать монитору порта время открыться в Wokwi
  Serial.println("ESP32-S3 sampler-sequencer: старт");
}

void loop() {
  unsigned long now = millis();
  if (now - lastBeat >= BEAT_INTERVAL_MS) {
    lastBeat = now;
    Serial.println("heartbeat");
  }
}
