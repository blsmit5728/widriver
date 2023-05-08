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

#define VERSION "0.0.1"

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
  memcpy(&myData, incomingData, sizeof(myData));
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
          if (use_blocklist){
            if (is_blocked(ssid)){
              Serial.print("BLOCK: ");
              Serial.println(ssid);
              wifi_block_at = millis();
              continue;
            }
            String tmp_mac_str = WiFi.BSSIDstr(i).c_str();
            tmp_mac_str.toUpperCase();
            if (is_blocked(tmp_mac_str)){
              Serial.print("BLOCK: ");
              Serial.println(tmp_mac_str);
              wifi_block_at = millis();
              continue;
            }
          }
          
          filewriter.printf("%s,%s,%s,%s,%d,%d,%s,WIFI\n", WiFi.BSSIDstr(i).c_str(), ssid.c_str(), security_int_to_string(WiFi.encryptionType(i)).c_str(), dt_string().c_str(), WiFi.channel(i), WiFi.RSSI(i), gps_string().c_str());
         
        }
      }
      filewriter.flush();
    }
    yield();
  }
}


void loop() {

}