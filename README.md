Wiring Diagram (ESP32 + ADS1115 + pH Sensor)
ADS1115 Power: Connect VDD to ESP32 3.3V, GND to GND.
I2C Communication: Connect ADS1115 SCL to ESP32 GPIO22, SDA to GPIO21.
Sensor Power: Connect pH Module VCC to ESP32 3.3V, GND to GND.
Signal: Connect pH Module Po (Analog Output) to ADS1115 A0.
Address: Leave ADDR pin disconnected (default 0x48). # phProbe



Create this path:

devices/reefDoser1/sensors/phCal

with this JSON:

{
  "enabled": true,
  "pH7_V": 1.82,
  "pH4_V": 1.44
}