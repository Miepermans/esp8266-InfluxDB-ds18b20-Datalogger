/*
 * Created by Roy van Dongen
 * See https://www.github.com/miepermans/ for details
 *
 * NTP DST code made by Niek Blankers https://www.github.com/niekproductions/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */


/*
 * Schema
 * 5volt ---------------------------------
 *             |                         |
 *           regulator                   |
 *        |--to 3.3 volt                 |
 *        |    |                 |       |
 *        |    |------4.7kOhm----|       |
 *        |    |                 |       |
 *        | ESP8266              |       |
 *        |  GPIO2  -----------------  DS18B20
 *        |    |                         |
 *        |    |                         |
 * Ground --------------------------------
 */
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <OneWire.h>
#include <DallasTemperature.h>


//WiFi Settings
#define AP_SSID     "YourSSID"
#define AP_PASSWORD "YourPassword"
WiFiClient client;

//InfluxDB Server
#define INFLUXDB_SERVER        "something.something.something"  // Your InfluxDB Server FQDN
#define INFLUXDB_PORT          8089                             // Default InfluxDB UDP Port
#define INFLUXDB_INTERVAL      10000                            // Milliseconds between measurements 
String SENSOR_LOCATION      =  "livingroom";                      // This location is used for the "device=" part of the InfluxDB update
WiFiUDP udp;

//Time settings
#define SECS_PER_MIN  (60UL)
#define SECS_PER_HOUR (3600UL)
#define SECS_PER_DAY  (SECS_PER_HOUR * 24L)
#define SYNC_INTERVAL   1200

const int   timeZone        = 1;                        //Central European Time
bool        autoDST         = true;                     //Enable auto Daylight Savings time
const char* ntpServerName   = "0.pool.ntp.org";         //Fill in your NTP server
bool isDST(int d, int m, int y);
bool isDSTSwitchDay(int d, int m, int y);
const int   NTP_PACKET_SIZE = 48;                       //NTP time stamp is in the first 48 bytes of the message
byte        packetBuffer[ NTP_PACKET_SIZE];             //buffer to hold incoming and outgoing packets
WiFiUDP     Udp;
unsigned int localPort = 8888;                          //If you do not set a localport, NTP wont work

time_t getNtpTime();
void sendNTPpacket(IPAddress &address);

unsigned long   lastdisplayupdate   = 0;
unsigned long   lastInfluxDBupdate  = 0;

//OneWire settings
#define ONE_WIRE_BUS 2  // DS18B20 pin use a 4.7K resistor

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);


void setup(void) {
  Serial.begin ( 115200 );
  WiFi.begin ( AP_SSID, AP_PASSWORD );
  Serial.println ( "" );
  // Wait for connection
  while ( WiFi.status() != WL_CONNECTED ) {
    delay ( 500 );
    Serial.print ( "." );                              
  }

  Serial.println ( "" );
  Serial.print ( "Connected to " );
  Serial.println ( AP_SSID );
  Serial.print ( "IP address: " );
  Serial.println ( WiFi.localIP() );

  Serial.print ( "Starting UDP: " );
  Udp.begin(localPort);

  setSyncProvider(getNtpTime);
  setSyncInterval(SYNC_INTERVAL);

  sensors.begin();
  
}

void loop(void) {

  // only update clock every 50ms
  if(millis()-lastdisplayupdate > 50) {
    lastdisplayupdate = millis();
  }else{
    return;
  }
    if(timeStatus() == timeNotSet) {
    Serial.println("time not yet known");
  }
  time_t t = now();

  // only send update to InfluxDB every INFLUXDB_INTERVAL
  if(millis()-lastInfluxDBupdate > INFLUXDB_INTERVAL) {
    lastInfluxDBupdate = millis();
    sensors.requestTemperatures();
    
    sendData(t);
  }
}

/*-------- NTP code ----------*/

time_t getNtpTime()
{
    IPAddress ntpServerIP; // NTP server's ip address

    while (Udp.parsePacket() > 0) ; // discard any previously received packets
    Serial.println("Transmit NTP Request");
    // get a random server from the pool
    WiFi.hostByName(ntpServerName, ntpServerIP);
    Serial.print(ntpServerName);
    Serial.print(": ");
    Serial.println(ntpServerIP);
    sendNTPpacket(ntpServerIP);
    uint32_t beginWait = millis();
    while (millis() - beginWait < 1500) {
        int size = Udp.parsePacket();
        if (size >= NTP_PACKET_SIZE) {
            Serial.println("Receive NTP Response");
            Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
            unsigned long secsSince1900;
            // convert four bytes starting at location 40 to a long integer
            secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
            secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
            secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
            secsSince1900 |= (unsigned long)packetBuffer[43];
            // New time in seconds since Jan 1, 1970
            unsigned long newTime = secsSince1900 - 2208988800UL +
                timeZone * SECS_PER_HOUR;

            // Auto DST
            if (autoDST) {
                if (isDSTSwitchDay(day(newTime), month(newTime), year(newTime))) {
                    if (month(newTime) == 3 && hour(newTime) >= 2) {
                        newTime += SECS_PER_HOUR;
                    } else if (month(newTime) == 10 && hour(newTime) < 2) {
                        newTime += SECS_PER_HOUR;
                    }
                } else if (isDST(day(newTime), month(newTime), year(newTime))) {
                    newTime += SECS_PER_HOUR;
                }
            }

            setSyncInterval(SYNC_INTERVAL);
            return newTime;
        }
    }
    Serial.println("No NTP Response :-(");
    // Retry soon
    setSyncInterval(10);
    return 0; // return 0 if unable to get the time
}

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress &address)
{
    // set all bytes in the buffer to 0
    memset(packetBuffer, 0, NTP_PACKET_SIZE);
    // Initialize values needed to form NTP request
    // (see URL above for details on the packets)
    packetBuffer[0] = 0b11100011;   // LI, Version, Mode
    packetBuffer[1] = 0;     // Stratum, or type of clock
    packetBuffer[2] = 6;     // Polling Interval
    packetBuffer[3] = 0xEC;  // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    packetBuffer[12] = 49;
    packetBuffer[13] = 0x4E;
    packetBuffer[14] = 49;
    packetBuffer[15] = 52;
    // all NTP fields have been given values, now
    // you can send a packet requesting a timestamp:
    Udp.beginPacket(address, 123); //NTP requests are to port 123
    Udp.write(packetBuffer, NTP_PACKET_SIZE);
    Udp.endPacket();
}

// Check if Daylight saving time (DST) applies
// Northern Hemisphere - +1 hour between March and October
bool isDST(int d, int m, int y){
    bool dst = false;
    dst = (m > 3 && m < 10); // October-March

    if (m == 3){
        // Last sunday of March
        dst = (d >= ((31 - (5 * y /4 + 4) % 7)));
    }else if (m == 10){
        // Last sunday of October
        dst = (d < ((31 - (5 * y /4 + 1) % 7)));
    }

    return dst;
}

bool isDSTSwitchDay(int d, int m, int y){
    bool dst = false;
    if (m == 3){
        // Last sunday of March
        dst = (d == ((31 - (5 * y /4 + 4) % 7)));
    }else if (m == 10){
        // Last sunday of October
        dst = (d == ((31 - (5 * y /4 + 1) % 7)));
    }
    return dst;
}

void sendData( time_t time ) {
  String line, temperature;
    
  line = String("temperature,device=" + SENSOR_LOCATION + " value=" + sensors.getTempCByIndex(0));
  Serial.println(line);
  
  // send the packet
  Serial.println("Sending UDP packet...");
  udp.beginPacket(INFLUXDB_SERVER, INFLUXDB_PORT);
  udp.print(line);
  udp.endPacket();
}
