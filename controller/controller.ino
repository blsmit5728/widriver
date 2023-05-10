// Include Libraries
#include <esp_now.h>
#include <WiFi.h>
#include <GParser.h>
#include <Preferences.h>
#include <MicroNMEA.h>
#include <time.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h" 

const String VERSION = "0.0.1";

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 32 // OLED display height, in pixels
#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define gps_allow_stale_time 60000 //ms to allow stale gps data for when lock lost.
//These variables are used for buffering/caching GPS data.
char nmeaBuffer[100];
MicroNMEA nmea(nmeaBuffer, sizeof(nmeaBuffer));
unsigned long lastgps = 0;
String last_dt_string = "";
String last_lats = "";
String last_lons = "";

//These variables are used to populate the LCD with statistics.
float temperature;
unsigned int ble_count;
unsigned int gsm_count;
unsigned int wifi_count;
unsigned int disp_gsm_count;
unsigned int disp_wifi_count;
unsigned long ble_block_at = 0;
unsigned long wifi_block_at = 0;


struct mac_addr {
   unsigned char bytes[6];
};
#define mac_history_len 512
struct mac_addr mac_history[mac_history_len];
unsigned int mac_history_cursor = 0;

#define YEAR_2020 1577836800 //epoch value for 1st Jan 2020; dates older than this are considered wrong (this code was written after 2020).
unsigned long epoch;
unsigned long epoch_updated_at;
const char* ntpServer = "pool.ntp.org";

File filewriter;
Preferences preferences;

TaskHandle_t primary_scan_loop_handle;

struct coordinates {
  double lat;
  double lon;
  int acc;
};

unsigned long lcd_last_updated;

// Structure example to send data
// Must match the receiver structure
typedef struct struct_message {

  char bssid[64];
  char ssid[32];
  uint8_t encryptionType;
  int32_t channel;
  int32_t rssi;
  int boardID;
} struct_message;

// Create a structured object
struct_message myData;

unsigned long bootcount = 0;

void boot_config(){
  //Load configuration variables and perform first time setup if required.
  Serial.println("Setting/loading boot config..");
  preferences.begin("wardriver", false);
  bool firstrun = preferences.getBool("first", true);
  bool doreset = preferences.getBool("reset", false);
  bootcount = preferences.getULong("bootcount", 0);
  Serial.println("Loaded variables");
}

// Callback function executed when data is received
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  if ( len == sizeof(myData))
  {
    memcpy(&myData, incomingData, sizeof(myData));
    Serial.print("Bytes received: ");
    Serial.println(len);
    Serial.print("Mac: ");
    Serial.println(myData.bssid);
    Serial.print("SSID: ");
    Serial.println(myData.ssid);
    String SSIDString = myData.ssid;
    SSIDString.replace(",", ".");  //commas in ssid braks the csv
  }
}

void clear_display(){
  //Clears the LCD and resets the cursor.
  display.clearDisplay();
  display.setCursor(0, 0);
}

void setup_wifi(){
  //Gets the WiFi ready for scanning by disconnecting from networks and changing mode.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
}

void setup() {

  // Set up Serial Monitor
  Serial.begin(115200);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // Address 0x3C for 128x32
    Serial.println(F("SSD1306 allocation failed"));
  }
  display.setRotation(2);
  display.clearDisplay();
  display.setTextSize(1);      // Normal 1:1 pixel scale
  display.setTextColor(WHITE); // Draw white text
  display.setCursor(0, 0);     // Start at top-left corner
  display.cp437(true);         // Use full 256 char 'Code Page 437' font
  display.println("Starting");
  display.print("Version ");
  display.println(VERSION);
  display.display();
  delay(4000);
  clear_display();
  display.println("Starting GPS");
  // Setup GPS
  Serial2.begin(9600);
  display.println("GPS started");
  display.display();
  
  if(!SD.begin()){
    Serial.println("SD Begin failed!");
    clear_display();
    display.println("SD Begin failed!");
    display.display();
    delay(4000);
  }
  uint8_t cardType = SD.cardType();
  if(cardType == CARD_NONE){
    Serial.println("No SD card attached!");
    clear_display();
    display.println("No SD Card!");
    display.display();
    delay(10000);
  }
  Serial.print("SD Card Type: ");
  if(cardType == CARD_MMC){
    Serial.println("MMC");
  } else if(cardType == CARD_SD){
    Serial.println("SDSC");
  } else if(cardType == CARD_SDHC){
    Serial.println("SDHC");
  } else {
    Serial.println("UNKNOWN");
  }
  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("SD Card Size: %lluMB\n", cardSize);

  while (!filewriter){
    filewriter = SD.open("/test.txt", FILE_APPEND);
    if (!filewriter){
      Serial.println("Failed to open file for writing.");
      clear_display();
      display.println("SD File open failed!");
      display.display();
      delay(1000);
    }
  }
  int wrote = filewriter.print("\n_BOOT_");
  filewriter.print(VERSION);
  filewriter.print(", ut=");
  filewriter.print(micros());
  filewriter.print(", rr=");
  filewriter.print(esp_reset_reason());
  filewriter.print(", id=");
  filewriter.print(0);
  filewriter.flush();
  if (wrote < 1){
    while(true){
      Serial.println("Failed to write to SD card!");
      clear_display();
      display.println("SD Card write failed!");
      display.display();
      delay(4000);
    }
  }
  
  // Set ESP32 as a Wi-Fi Station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Initilize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  
  // Register callback function
  esp_now_register_recv_cb(OnDataRecv);

  Serial.println("Opening destination file for writing");

  String filename = "/wd3-";
  filename = filename + bootcount;
  filename = filename + ".csv";
  Serial.println(filename);
  filewriter = SD.open(filename, FILE_APPEND);
  filewriter.print("WigleWifi-1.4,appRelease=" + VERSION + ",model=wardriver.uk Rev3 ESP32,release=1.0.0,device=wardriver.uk Rev3 ESP32,display=i2c LCD,board=wardriver.uk Rev3 ESP32,brand=JHewitt\nMAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n");
  filewriter.flush();
  
  clear_display();
  display.println("Starting main..");
  display.display();

  xTaskCreatePinnedToCore(
    primary_scan_loop, /* Function to implement the task */
    "primary_scan_loop", /* Name of the task */
    10000,  /* Stack size in words */
    NULL,  /* Task input parameter */
    3,  /* Priority of the task */
    &primary_scan_loop_handle,  /* Task handle. */
    0); /* Core where the task should run */

}

void save_mac(unsigned char* mac){
  //Save a MAC address into the recently seen array.
  if (mac_history_cursor >= mac_history_len){
    mac_history_cursor = 0;
  }
  struct mac_addr tmp;
  for (int x = 0; x < 6 ; x++){
    tmp.bytes[x] = mac[x];
  }

  mac_history[mac_history_cursor] = tmp;
  mac_history_cursor++;
  Serial.print("Mac len ");
  Serial.println(mac_history_cursor);
}

boolean seen_mac(unsigned char* mac){
  //Return true if this MAC address is in the recently seen array.

  struct mac_addr tmp;
  for (int x = 0; x < 6 ; x++){
    tmp.bytes[x] = mac[x];
  }

  for (int x = 0; x < mac_history_len; x++){
    if (mac_cmp(tmp, mac_history[x])){
      return true;
    }
  }
  return false;
}

void print_mac(struct mac_addr mac){
  //Print a mac_addr struct nicely.
  for (int x = 0; x < 6 ; x++){
    Serial.print(mac.bytes[x],HEX);
    Serial.print(":");
  }
}

boolean mac_cmp(struct mac_addr addr1, struct mac_addr addr2){
  //Return true if 2 mac_addr structs are equal.
  for (int y = 0; y < 6 ; y++){
    if (addr1.bytes[y] != addr2.bytes[y]){
      return false;
    }
  }
  return true;
}

String security_int_to_string(int security_type){
  //Provide a security type int from WiFi.encryptionType(i) to convert it to a String which Wigle CSV expects.
  String authtype = "";
  switch (security_type){
    case WIFI_AUTH_OPEN:
      authtype = "[OPEN]";
      break;
  
    case WIFI_AUTH_WEP:
      authtype = "[WEP]";
      break;
  
    case WIFI_AUTH_WPA_PSK:
      authtype = "[WPA_PSK]";
      break;
  
    case WIFI_AUTH_WPA2_PSK:
      authtype = "[WPA2_PSK]";
      break;
  
    case WIFI_AUTH_WPA_WPA2_PSK:
      authtype = "[WPA_WPA2_PSK]";
      break;
  
    case WIFI_AUTH_WPA2_ENTERPRISE:
      authtype = "[WPA2]";
      break;

    //Requires at least v2.0.0 of https://github.com/espressif/arduino-esp32/
    case WIFI_AUTH_WPA3_PSK:
      authtype = "[WPA3_PSK]";
      break;

    case WIFI_AUTH_WPA2_WPA3_PSK:
      authtype = "[WPA2_WPA3_PSK]";
      break;

    case WIFI_AUTH_WAPI_PSK:
      authtype = "[WAPI_PSK]";
      break;
        
    default:
      authtype = "[UNDEFINED]";
  }

  return authtype;
}

void primary_scan_loop(void * parameter){
  //This core will be dedicated entirely to WiFi scanning in an infinite loop.
  while (true){
    disp_wifi_count = wifi_count;
    wifi_count = 0;
    setup_wifi();
    for(int scan_channel = 1; scan_channel < 14; scan_channel++){
      yield();
      //scanNetworks(bool async, bool show_hidden, bool passive, uint32_t max_ms_per_chan, uint8_t channel)
      int n = WiFi.scanNetworks(false,true,false,110,scan_channel);
      if (n > 0){
        wifi_count = wifi_count + n;
        for (int i = 0; i < n; i++) {
          if (seen_mac(WiFi.BSSID(i))){
            //Skip any APs which we've already logged.
            continue;
          }
          //Save the AP MAC inside the history buffer so we know it's logged.
          save_mac(WiFi.BSSID(i));

          String ssid = WiFi.SSID(i);
          ssid.replace(",","_");
          
          filewriter.printf("%s,%s,%s,%s,%d,%d,%s,WIFI\n", WiFi.BSSIDstr(i).c_str(), ssid.c_str(), security_int_to_string(WiFi.encryptionType(i)).c_str(), dt_string().c_str(), WiFi.channel(i), WiFi.RSSI(i), gps_string().c_str());
         
        }
      }
      filewriter.flush();
    }
    yield();
  }
}

String gps_string(){
  //Return a String which can be used in a Wigle CSV line to show the current position.
  //This uses data from GPS and GSM tower locations.
  //output: lat,lon,alt,acc
  String out = "";
  long alt = 0;
  if (!nmea.getAltitude(alt)){
    alt = 0;
  }
  float altf = (float)alt / 1000;

  String lats = String((float)nmea.getLatitude()/1000000, 7);
  String lons = String((float)nmea.getLongitude()/1000000, 7);
  if (nmea.isValid() && nmea.getHDOP() <= 250){
    last_lats = lats;
    last_lons = lons;
  }

  if (nmea.getHDOP() > 250){
    lats = "";
    lons = "";
  }

  //HDOP returned here is in tenths and needs dividing by 10 to make it 'true'.
  //We're using this as a very basic estimate of accuracy by multiplying HDOP with the precision of the GPS module (2.5)
  //This isn't precise at all, but is a very rough estimate to your GPS accuracy.
  float accuracy = ((float)nmea.getHDOP()/10);
  accuracy = accuracy * 2.5;

  if (!nmea.isValid()){
    lats = "";
    lons = "";
    accuracy = 1000;
    if (lastgps + gps_allow_stale_time > millis()){
      lats = last_lats;
      lons = last_lons;
      accuracy = 5 + (millis() - lastgps) / 100;
    } else {
      //Serial.println("Bad GPS, using GSM location");
      // struct coordinates pos = gsm_get_current_position();
      // if (pos.acc > 0){
      //   lats = String(pos.lat, 6);
      //   lons = String(pos.lon, 6);
      //   accuracy = pos.acc;
      // }
    }
  }

  //The module we are using has a precision of 2.5m, accuracy can never be better than that.
  if (accuracy <= 2.5){
    accuracy = 2.5;
  }

  out = lats + "," + lons + "," + altf + "," + accuracy;
  return out;
}

void loop() {
  while (Serial2.available()){
    char c = Serial2.read();
    if (nmea.process(c)){
      if (nmea.isValid()){
        lastgps = millis();
        update_epoch();
      }
    }
  }
  if (lcd_last_updated == 0 || millis() - lcd_last_updated > 1000){
    lcd_show_stats();
    lcd_last_updated = millis();
  }
}

void lcd_show_stats(){
  //Clear the LCD then populate it with stats about the current session.
  boolean ble_did_block = false;
  boolean wifi_did_block = false;
  if (millis() - wifi_block_at < 30000){
    wifi_did_block = true;
  }
  if (millis() - ble_block_at < 30000){
    ble_did_block = true;
  }
  clear_display();
  display.print("WiFi:");
  display.print(disp_wifi_count);
  if (wifi_did_block){
    display.print("X");
  }
  if (int(temperature) != 0){
    display.print(" Temp:");
    display.print(temperature);
    display.print("c");
  }
  display.println();
  if (nmea.getHDOP() < 250 && nmea.getNumSatellites() > 0){
    display.print("HDOP:");
    display.print(nmea.getHDOP());
    display.print(" Sats:");
    display.print(nmea.getNumSatellites());
    display.println(nmea.getNavSystem());
  } else {
    display.print("No GPS: ");
    //struct coordinates gsm_loc = gsm_get_current_position();
    //if (gsm_loc.acc > 0){
    //  display.println("GSM pos OK");
    //} else {
    //  display.println("No GSM pos");
    //}
  }
  //if (b_working){
    display.print("BLE:");
    display.print(ble_count);
    if (ble_did_block){
      display.print("X");
    }
  display.print(" GSM:");
  display.println(disp_gsm_count);
  //} else {
  //  display.println("ESP-B NO DATA");
  //}
  display.println(dt_string());
  display.display();
  if (gsm_count > 0){
    disp_gsm_count = gsm_count;
    gsm_count = 0;
  }
}

String dt_string(){
  //Return a datetime String using local timekeeping and GPS data.
  time_t now = epoch;
  struct tm ts;
  char buf[80];

  ts = *localtime(&now);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ts);
  String out = String(buf);
  
  Serial.print("New dt_str: ");
  Serial.println(out);
  
  return out;
}

void update_epoch(){
  //Update the global epoch variable using the GPS time source.
  String gps_dt = dt_string_from_gps();
  if (!nmea.isValid() || lastgps == 0 || gps_dt.length() < 5){
    unsigned int tdiff_sec = (millis()-epoch_updated_at)/1000;
    if (tdiff_sec < 1){
      return;
    }
    epoch += tdiff_sec;
    epoch_updated_at = millis();
    Serial.print("Added ");
    Serial.print(tdiff_sec);
    Serial.println(" seconds to epoch");
    return;
  }
  
  struct tm tm;

  strptime(gps_dt.c_str(), "%Y-%m-%d %H:%M:%S", &tm );
  epoch = mktime(&tm);
  epoch_updated_at = millis();
}

String dt_string_from_gps(){
  //Return a datetime String using GPS data only.
  String datetime = "";
  if (nmea.isValid() && nmea.getYear() > 0){
    datetime += nmea.getYear();
    datetime += "-";
    datetime += nmea.getMonth();
    datetime += "-";
    datetime += nmea.getDay();
    datetime += " ";
    datetime += nmea.getHour();
    datetime += ":";
    datetime += nmea.getMinute();
    datetime += ":";
    datetime += nmea.getSecond();
    last_dt_string = datetime;
  } else if (lastgps + gps_allow_stale_time > millis()) {
    datetime = last_dt_string;
  }
  return datetime;
}
