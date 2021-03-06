/*
 Helium Balloon Tracker build for LilyGo TTGO T-Beam v1.1 boards.
 Copyright (C) 2022 by goshawk22

Forked from:
TTGO T-Beam Mapper for Helium
Copyright (C) 2021 by @Max_Plastix

 This code comes from a number of developers and earlier efforts,
  including:  Fizzy, longfi-arduino, Kyle T. Gabriel, and Xose Pérez

 GPL makes this all possible -- continue to modify, extend, and share!
 */

/*
  Main module

  # Modified by Kyle T. Gabriel to fix issue with incorrect GPS data for
  TTNMapper

  Copyright (C) 2018 by Xose Pérez <xose dot perez at gmail dot com>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <Arduino.h>
#include <lmic.h>

#include <SPI.h>

#include "configuration.h"
#include "gps.h"
#include "ttn.h"
#include "power.h"
#include "utils.h"
#include "sensors.h"

// Just so we can disable it to save power
#include <BluetoothSerial.h>
#include <WiFi.h>
#include <esp_bt.h>

#define FPORT_PING 1
#define FPORT_GPS 2  // FPort for Uplink messages -- must match Helium Console Decoder script!
#define FPORT_STATUS 5
#define FPORT_GPSLOST 6

// Defined in ttn.cpp
void ttn_register(void (*callback)(uint8_t message));

unsigned long int last_send_ms = 0;     // Time of last uplink
bool uplink_failed = false;             // Did the last attempted uplink fail?
double last_send_lat = 0;               // Last known location
double last_send_lon = 0;               //
uint32_t last_fix_time = 0;
bool transmitted = false;               // Have we transmitted a sensor uplink yet?
bool ping = false;                      // Are we waiting for a ping packet to complete?
bool ping_requested = false;            // Has a ping packet been requested by a downlink?
unsigned long int last_status_ms = 0;   // Time of last status uplink
signed long int status_uplinks = 0;     // Number of status uplinks
bool ack_rec = false;                   // Have we recived an ack yet? Confirms if our packets are being heard.

unsigned int tx_interval_s = TX_INTERVAL;  // TX_INTERVAL

// Return status from mapper uplink, since we care about the flavor of the failure
enum mapper_uplink_result { 
  MAPPER_UPLINK_SUCCESS,
  MAPPER_UPLINK_BADFIX,
  MAPPER_UPLINK_NOLORA,
  MAPPER_UPLINK_NOTYET
};

bool isJoined = false;

// Buffer for Payload frame
static uint8_t txBuffer[24];

// Buffer for Serial output
char msgBuffer[40];

signed long int ack_req = 0;  // Number of acks requested
signed long int ack_rx = 0;   // Number of acks received

static boolean booted = false;

bool ready() {
  // Don't attempt to send or update until we join Helium
  if (!isJoined)
    return false;

  // LoRa is not ready for a new packet, maybe still sending the last one.
  if (!LMIC_queryTxReady())
    return false;
  
  // Check if there is not a current TX/RX job running
  if (LMIC.opmode & OP_TXRXPEND)
    return false;

  return true;
}

enum mapper_uplink_result send_uplink(uint8_t *txBuffer, uint8_t length, uint8_t fport, boolean confirmed, boolean ping) {
  unsigned long int now = millis();

  if (!ready()) {
    return MAPPER_UPLINK_NOLORA;
  }

  if (confirmed) {
    Serial.println("ACK requested");
    ack_req++;
  }

  // send it!
  if (!ttn_send(txBuffer, length, fport, confirmed)) {
    Serial.println("Surprise send failure!");
    return MAPPER_UPLINK_NOLORA;
  }
  if (!ping) {
    last_send_ms = now;
  }
  return MAPPER_UPLINK_SUCCESS;
}

// Store Lat & Long in six bytes of payload
void pack_lat_lon(double lat, double lon) {
  uint32_t LatitudeBinary;
  uint32_t LongitudeBinary;

  LatitudeBinary = ((lat + 90) / 180.0) * 16777215;
  LongitudeBinary = ((lon + 180) / 360.0) * 16777215;

  txBuffer[0] = (LatitudeBinary >> 16) & 0xFF;
  txBuffer[1] = (LatitudeBinary >> 8) & 0xFF;
  txBuffer[2] = LatitudeBinary & 0xFF;
  txBuffer[3] = (LongitudeBinary >> 16) & 0xFF;
  txBuffer[4] = (LongitudeBinary >> 8) & 0xFF;
  txBuffer[5] = LongitudeBinary & 0xFF;
}

void pack_bme280() {
  if (bme280_alive) {
    unsigned long int bmeTemp = bme.readTemperature() * 100;
    unsigned long int bmePressure = bme.readPressure();
    unsigned long int bmeHumidity = bme.readHumidity() * 100;

    txBuffer[15] = (bmeTemp >> 8) & 0xFF;
    txBuffer[16] = bmeTemp & 0xFF;
    txBuffer[17] = (bmePressure >> 16) & 0xFF;
    txBuffer[18] = (bmePressure >> 8) & 0xFF;
    txBuffer[19] = bmePressure & 0xFF;
    txBuffer[20] = (bmeHumidity >> 8) & 0xFF;
    txBuffer[21] = bmeHumidity & 0xFF;

  } else {
    // Obviously bad values to show something went wrong. Temp may equal zero but only take as real when pressure and humidity are non-zero.
    txBuffer[15] = 0;
    txBuffer[16] = 0;
    txBuffer[17] = 0;
    txBuffer[18] = 0;
    txBuffer[19] = 0;
    txBuffer[20] = 0;
    txBuffer[21] = 0;

  }
}

void pack_ltr390() {
  if (ltr390_alive) {
    unsigned long int uv = ltr.readUVS();

    txBuffer[22] = (uv >> 8) & 0xFF;
    txBuffer[23] = uv & 0xFF;
  } else {
    // Obviously bad values to show something went wrong
    txBuffer[22] = 0;
    txBuffer[23] = 0;
  }
}

uint8_t battery_byte(void) {
  uint16_t batteryVoltage = ((float_t)((float_t)(axp.getBattVoltage()) / 10.0) + .5);
  return (uint8_t)((batteryVoltage - 200) & 0xFF);
}

// Prepare a packet with GPS and sensor data
void build_full_packet() {
  double lat;
  double lon;
  uint16_t altitudeGps;
  uint8_t sats;
  uint16_t speed;
  uint16_t minutes_lost = (millis() - last_fix_time) / 1000 / 60;
  unsigned long int uptime = millis() / 1000 / 60;

  lat = tGPS.location.lat();
  lon = tGPS.location.lng();
  pack_lat_lon(lat, lon);
  altitudeGps = (uint16_t)tGPS.altitude.meters();
  speed = (uint16_t)tGPS.speed.kmph();  // convert from double
  if (speed > 255)
    speed = 255;  // don't wrap around.
  sats = tGPS.satellites.value();

  sprintf(msgBuffer, "Lat: %f, ", lat);
  Serial.println(msgBuffer);
  sprintf(msgBuffer, "Long: %f, ", lon);
  Serial.println(msgBuffer);
  sprintf(msgBuffer, "Alt: %f, ", tGPS.altitude.meters());
  Serial.println(msgBuffer);
  sprintf(msgBuffer, "Sats: %d", sats);
  Serial.println(msgBuffer);

  txBuffer[6] = (altitudeGps >> 8) & 0xFF;
  txBuffer[7] = altitudeGps & 0xFF;

  txBuffer[8] = speed & 0xFF;
  txBuffer[9] = battery_byte();

  txBuffer[10] = sats & 0xFF;
  txBuffer[11] = (uptime >> 8) & 0xFF;
  txBuffer[12] = uptime & 0xFF;

  txBuffer[13] = (minutes_lost >> 8) & 0xFF;
  txBuffer[14] = minutes_lost & 0xFF;

  pack_bme280();
  pack_ltr390();
}

bool status_uplink(void) {
  pack_lat_lon(last_send_lat, last_send_lon);

  unsigned long int uptime = millis() / 1000 / 60;
  txBuffer[6] = battery_byte();
  txBuffer[7] = uptime & 0xFF; // Time since booted
  Serial.printf("Tx: STATUS %lu \n", uptime);
  status_uplinks++;
  last_status_ms = millis();
  return send_uplink(txBuffer, 8, FPORT_STATUS, 1, 0);
}

enum mapper_uplink_result gpslost_uplink(void) {
  uint16_t minutes_lost;
  unsigned long int uptime;

  // Want an ACK on this one?
  bool confirmed = (LORAWAN_CONFIRMED_EVERY > 0) && (ttn_get_count() % LORAWAN_CONFIRMED_EVERY == 0);

  uptime = millis() / 1000 / 60;
  minutes_lost = (millis() - last_fix_time) / 1000 / 60;
  pack_lat_lon(last_send_lat, last_send_lon);
  txBuffer[6] = 0; // Obviously wrong value as we couldn't get a fix. Placeholder to make structure same as full packet
  txBuffer[7] = 0; //Another obviously wrong value
  txBuffer[8] = 0; //Another obviously wrong value
  txBuffer[9] = battery_byte();
  txBuffer[10] = tGPS.satellites.value() & 0xFF;
  txBuffer[11] = (uptime >> 8) & 0xFF;
  txBuffer[12] = uptime & 0xFF;
  txBuffer[13] = (minutes_lost >> 8) & 0xFF;
  txBuffer[14] = minutes_lost & 0xFF;

  pack_bme280();
  pack_ltr390();
  Serial.printf("Tx: GPSLOST %d\n", minutes_lost);
  return send_uplink(txBuffer, 24, FPORT_GPSLOST, confirmed, 0);
}

// Send a packet, if one is warranted
enum mapper_uplink_result gps_uplink(void) {
  unsigned long int uptime = millis() / 1000 / 60;
  double now_lat = tGPS.location.lat();
  double now_lon = tGPS.location.lng();

  // Here we try to filter out bogus GPS readings.
  if (!(tGPS.location.isValid() && tGPS.time.isValid() && tGPS.satellites.isValid() && tGPS.hdop.isValid() &&
        tGPS.altitude.isValid() && tGPS.speed.isValid()))
    return MAPPER_UPLINK_BADFIX;

  // Filter out any reports while we have low satellite count.  The receiver can old a fix on 3, but it's poor.
  if (tGPS.satellites.value() < 4)
    return MAPPER_UPLINK_BADFIX;

  // With the exception of a few places, a perfectly zero lat or long probably means we got a bad reading
  if (now_lat == 0.0 || now_lon == 0.0)
    return MAPPER_UPLINK_BADFIX;

  // prepare the LoRa frame
  build_full_packet();

  // Want an ACK on this one?
  bool confirmed = (LORAWAN_CONFIRMED_EVERY > 0) && (ttn_get_count() % LORAWAN_CONFIRMED_EVERY == 0);

  last_send_lat = now_lat;
  last_send_lon = now_lon;
  
  // Send it!
  Serial.printf("Tx: GPS %lu \n", uptime);
  return send_uplink(txBuffer, 24, FPORT_GPS, confirmed, 0);  // We did it!
}

enum mapper_uplink_result uplink() {
  enum mapper_uplink_result result = gps_uplink();
  if (result == MAPPER_UPLINK_BADFIX) {
    return gpslost_uplink();
  } else {
    return result;
  }
}

void ping_uplink() {
  double lat;
  double lon;
  uint16_t altitudeGps;
  if (tGPS.location.isValid() && tGPS.time.isValid() && tGPS.satellites.isValid() && tGPS.hdop.isValid() && 
        tGPS.altitude.isValid() && tGPS.speed.isValid() && tGPS.satellites.value() >= 4) {
          
    lat = tGPS.location.lat();
    lon = tGPS.location.lng();
    pack_lat_lon(lat, lon);
    altitudeGps = (uint16_t)tGPS.altitude.meters();
    txBuffer[6] = (altitudeGps >> 8) & 0xFF;
    txBuffer[7] = altitudeGps & 0xFF;

  } else {
    // Bad GPS fix
    pack_lat_lon(last_send_lat, last_send_lon);
    txBuffer[6] = 0;
    txBuffer[7] = 0;
  }

  send_uplink(txBuffer, 8, FPORT_PING, 0, 1);
  Serial.println("Ping Uplink sent. Rebooting...");
  ESP.restart();
}

// LoRa message event callback
void lora_msg_callback(uint8_t message) {
  static boolean seen_joined = false, seen_joining = false;
#ifdef DEBUG_LORA_MESSAGES
  if (EV_JOIN_TXCOMPLETE == message)
    Serial.println("# JOIN_TXCOMPLETE");
  if (EV_TXCOMPLETE == message)
    Serial.println("# TXCOMPLETE");
  if (EV_RXCOMPLETE == message)
    Serial.println("# RXCOMPLETE");
  if (EV_RXSTART == message)
    Serial.println("# RXSTART");
  if (EV_TXCANCELED == message)
    Serial.println("# TXCANCELED");
  if (EV_TXSTART == message)
    Serial.println("# TXSTART");
  if (EV_JOINING == message)
    Serial.println("# JOINING");
  if (EV_JOINED == message)
    Serial.println("# JOINED");
  if (EV_JOIN_FAILED == message)
    Serial.println("# JOIN_FAILED");
  if (EV_REJOIN_FAILED == message)
    Serial.println("# REJOIN_FAILED");
  if (EV_RESET == message)
    Serial.println("# RESET");
  if (EV_LINK_DEAD == message)
    Serial.println("# LINK_DEAD");
  if (EV_ACK == message)
    Serial.println("# ACK");
  if (EV_PENDING == message)
    Serial.println("# PENDING");
  if (EV_QUEUED == message)
    Serial.println("# QUEUED");
#endif

  /* This is confusing because JOINED is sometimes spoofed and comes early */
  if (EV_JOINED == message)
    seen_joined = true;
  if (EV_JOINING == message)
    seen_joining = true;
  if (!isJoined && seen_joined && seen_joining) {
    isJoined = true;
    ttn_set_sf(LORAWAN_SF);  // SF is left at SF that had a successful join so change to preferred SF
  }

  if (EV_ACK == message) {
    ack_rx++;
    ack_rec = true;
    Serial.printf("ACK! %lu / %lu\n", ack_rx, ack_req);
  }

  if (EV_RXCOMPLETE == message || EV_RESPONSE == message) {
    size_t len = ttn_response_len();
    uint8_t data[len];
    uint8_t port;
    ttn_response(&port, data, len);

    Serial.printf("Downlink on port: %d, length %d = ", port, len);
    for (int i = 0; i < len; i++) {
      if (data[i] < 16)
        Serial.print('0');
      Serial.print(data[i], HEX);
    }
    Serial.println();
    if (data[0] == 0x01) {
      ping_requested = true;
    }
  }
}

void setup() {
  // Debug
#ifdef DEBUG_PORT
  DEBUG_PORT.begin(SERIAL_BAUD);
#endif

  wakeup();

  // Make sure WiFi and BT are off
  // WiFi.disconnect(true);
  WiFi.mode(WIFI_MODE_NULL);
  btStop();

  // Make sure prefs get erased
  if (JOIN_FROM_SCRATCH) {
    ttn_erase_prefs();
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  scanI2Cdevice();

  axp192Init();
  bme280_alive = BMEsensorInit();
  ltr390_alive = LTRsensorInit();

  // GPS sometimes gets wedged with no satellites in view and only a power-cycle
  // saves it. Here we turn off power and the delay in screen setup is enough
  // time to bonk the GPS
  axp.setPowerOutPut(AXP192_LDO3, AXP202_OFF);  // GPS power off

  pinMode(RED_LED, OUTPUT);
  digitalWrite(RED_LED, HIGH);  // Off

  // GPS power on, so it has time to setttle.
  axp.setPowerOutPut(AXP192_LDO3, AXP202_ON);

  // Helium setup
  if (!ttn_setup()) {
    // Something has gone wrong, and now the tracker can't do anything. Restart and hope the problem fixes itself.
    Serial.println("TTN Setup failed. Rebooting...");
    ESP.restart();
  }

  ttn_register(lora_msg_callback);
  ttn_join();
  ttn_adr(LORAWAN_ADR);

  // Might have to add a longer delay here for GPS boot-up.  Takes longer to sync if we talk to it too early.
  delay(100);
  gps_setup(true);  // Init GPS baudrate and messages

  // This is bad.. we can't find the AXP192 PMIC, so no menu key detect:
  if (!axp192_found)
    Serial.println("** Missing AXP192! **\n");

  booted = 1;

  // Send status uplink
  while (!isJoined) {
    ttn_loop();
  }
}

uint32_t woke_time_ms = 0;
uint32_t woke_fix_count = 0;

void loop() {
  static uint32_t last_fix_count = 0;
  uint32_t now_fix_count;
  uint32_t now = millis();

  gps_loop(0);  // Update GPS
  now_fix_count = tGPS.sentencesWithFix();          // Did we get a new fix?
  if (now_fix_count != last_fix_count) {
    last_fix_count = now_fix_count;
    last_fix_time = now;  // Note the time of most recent fix
  }

  ttn_loop();

  // Check if sensors have failed
  if (!checkI2Cdevice(I2C_BME280_ADDRESS)) {
    bme280_alive = false;
  } else {
    if (!bme280_alive) {
      //The sensor was dead but is now alive
      if (BMEsensorInit()) {
        bme280_alive = true;
      }
    }
  }

  if (!checkI2Cdevice(I2C_LTR390_ADDRESS)) {
    ltr390_alive = false;
  } else {
    if (!ltr390_alive) {
      if (LTRsensorInit()) {
        ltr390_alive = true;
      }
    }
  }

  // If the number of acks requested is greater than the number of acks received, something has gone wrong.
  // Sometimes after rejoining the network, all the packets are late. To avoid this, reboot if packets don't seem to be getting through
  if (ack_req - ack_rx >= ACK_FAIL_THRESHOLD) {
    Serial.println("Mismatch between number of acks requested and number of acks recieved. Rebooting...");
    ESP.restart();
  }

  // We sent a status uplink and requested an ack, but got nothing in return so try again
  if (now - last_status_ms > 30 * 1000 && !ack_rec && ready()) {
      status_uplink();
  }

  // Transmit ping packet
  if (!ping && ready() && now - last_send_ms > 10 * 1000 && ping_requested) {
    ping_requested = false;
    Serial.println("** PING");
    ping = true;
    ttn_set_sf(LORAWAN_SF_PING);
    ping_uplink();
  }
  
  // Only transmit if joined, a ping isn't in progress and we have receieved an ack (aka no more status uplinks)
  if (!ping && ack_rec && ready()) {
    // Transmit if it is time, we haven't transmitted yet, or if Tx is ready after a failed transmission
    if (now - last_send_ms > tx_interval_s * 1000 || (!transmitted && ready()) || (uplink_failed && ready())) {
      Serial.println("** TIME");
      if (uplink() == MAPPER_UPLINK_SUCCESS) {
        transmitted = true;
        uplink_failed = false;
      } else {
        uplink_failed = true;
        Serial.println("Uplink Failed");
      }
    }
  }
}
