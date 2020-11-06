
/*********
  William Webb (c) 11/01/2020
  Sketch to take a picture and store it in flash memory.  The picture can be retrieved with
  FTP.  This sketch is design to work only with the ESP32-CAM A.I thinker board.
  
  Credits:
  
  added littlefs and FTP server to the work below.

  Rui Santos
  Complete project details at https://RandomNerdTutorials.com/esp32-cam-take-photo-save-microsd-card
  to put your board in flashing mode

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files.
  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  You only need to format LITTLEFS the first time you run a
  test or else use the LITTLEFS plugin to create a partition
  https://github.com/lorol/arduino-esp32littlefs-plugin

  Note: No matter which compiler option selected, some libraries will not be used.
        Additionally, some file system functions are not used.  These inefficiencies 
        are intentional making the sketch a good template for modification.  All the 
        pieced for more complex code are in place.
        
  Changelog:

  11/01/2020 First working release
  11/02/2020 Added maxPictures,uSD card support,minors,
             framework for non-volatile picture counter
  11/03/2020 Implement non-volatile picture counter 
  11/04/2020 Tested flashMemory mode - worked okay, rtn 0 on bad read (was -1),
             decrease flashMemory picture count to 4
  11/05/2020 Decrease flashMemory picture count to 3 and move increment,
             minors, spelling.

*********/

//#define useSD // <----- uncomment to use uSD for picture storage (preferred)
#include <Arduino.h>
#include "FS.h"                // general file system 
#include <LITTLEFS.h>          // efficient flashMemory library
#include "SD_MMC.h"            // SD Card ESP32
#include <EEPROM.h>            // read and write from flash memory
#include "ESPFtpServer.h"      // general FTP library
#include <WiFi.h>              // WiFi Library
#include <WiFiClient.h>        // for Web page  
#include "esp_camera.h"        // camera library
#include "esp_timer.h"         // non-blocking timer
#include "img_converters.h"    // for camera
#include "soc/soc.h"           // Disable brownout problems
#include "soc/rtc_cntl_reg.h"  // Disable brownout problems
#include "driver/rtc_io.h"     // Disable brownout problems
#include <ESPAsyncWebServer.h> // Web server 
#include <StringArray.h>       // used by Web server

#define File_Name "CAM_LittleFS_3"
#define Date "11/05/2020"

#define FORMAT_LITTLEFS_IF_FAILED false  // if false don't reformat if something goes wrong
#define EEPROM_SIZE 1                    // define the number of bytes you want to access
#define FILE_PHOTO "/photo.jpg"          // filemane used in Web site

// Compiler directives for using uSD or built-in RAM
#ifdef useSD
#define FS_NAME "SD_MMC"
#define FS_ID SD_MMC
#define maxPictures 1000
#endif
#ifndef useSD
#define FS_NAME "LittleFS"
#define FS_ID LITTLEFS
#define maxPictures 3
#endif

const char* ssid = "*****";          // WiFi credentials
const char* password = "*****";     // WiFi credentials

char pName[50];                         // Name for indexed jpg 
char pCount[50];                        // for picture count string storage
int picture_index = 0;                  // Initialize at photo_0.jpg
boolean takeNewPhoto = false;           // Flag to manage Loop

FtpServer ftpSrv;   //set #define FTP_DEBUG in ESP32FtpServer.h to see ftp verbose on serial
AsyncWebServer server(80);  // Create AsyncWebServer object on port 80

// OV2640 camera module pins (CAMERA_MODEL_AI_THINKER)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// Web Page
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { text-align:center; }
    .vert { margin-bottom: 10%; }
    .hori{ margin-bottom: 0%; }
  </style>
</head>
<body>
  <div id="container">
    <h2>KI6EPW / AB6OR ESP32-CAM - Last Photo</h2>
    <p>It might take more than 5 seconds to capture a photo.</p>
    <p>
      <button onclick="rotatePhoto();">ROTATE</button>
      <button onclick="capturePhoto()">CAPTURE PHOTO</button>
      <button onclick="location.reload();">REFRESH PAGE</button>
    </p>
  </div>
  <div><img src="saved-photo" id="photo" width="70%"></div>
</body>
<script>
  var deg = 0;
  function capturePhoto() {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', "/capture", true);
    xhr.send();
  }
  function rotatePhoto() {
    var img = document.getElementById("photo");
    deg += 90;
    if(isOdd(deg/90)){ document.getElementById("container").className = "vert"; }
    else{ document.getElementById("container").className = "hori"; }
    img.style.transform = "rotate(" + deg + "deg)";
  }
  function isOdd(n) { return Math.abs(n % 2) == 1; }
</script>
</html>)rawliteral";

// ------------------------------ LittleFS Functions - Not all are used ------------------------------------

void listDir(fs::FS &fs, const char * dirname, uint8_t levels) {
  Serial.printf("Listing directory: %s\r\n", dirname);

  File root = fs.open(dirname);
  if (!root) {
    Serial.println("- failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    Serial.println(" - not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.print("  DIR : ");
      Serial.println(file.name());
      if (levels) {
        listDir(fs, file.name(), levels - 1);
      }
    } else {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("\tSIZE: ");
      Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}

void createDir(fs::FS &fs, const char * path) {
  Serial.printf("Creating Dir: %s\n", path);
  if (fs.mkdir(path)) {
    Serial.println("Dir created");
  } else {
    Serial.println("mkdir failed");
  }
}

void removeDir(fs::FS &fs, const char * path) {
  Serial.printf("Removing Dir: %s\n", path);
  if (fs.rmdir(path)) {
    Serial.println("Dir removed");
  } else {
    Serial.println("rmdir failed");
  }
}

void readFile(fs::FS &fs, const char * path) {
  Serial.printf("Reading file: %s\r\n", path);

  File file = fs.open(path);
  if (!file || file.isDirectory()) {
    Serial.println("- failed to open file for reading");
    return;
  }

  Serial.println("- read from file:");
  while (file.available()) {
    Serial.write(file.read());
  }
  file.close();
}
int readFileInt(fs::FS &fs, const char * path) {
  char index_buffer[4] = {0, 0, 0, 0};
  int index = 0;
  int i = 0;
  Serial.printf("Reading file: %s\r\n", path);

  File file = fs.open(path);
  if (!file || file.isDirectory()) {
    Serial.println("- failed to open file for reading");
    return 0;
  }

  Serial.print("- read from file: ");
  while (file.available()) {
    index_buffer[i] = (file.read());
    i++;
  }
  index = atoi(index_buffer); // change to int
  file.close();
  return index;
}

void writeFile(fs::FS &fs, const char * path, const char * message) {
  Serial.printf("Writing file: %s\r\n", path);

  File file = fs.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("- failed to open file for writing");
    return;
  }
  if (file.print(message)) {
    Serial.println("- file written");
  } else {
    Serial.println("- write failed");
  }
  file.close();
}

void appendFile(fs::FS &fs, const char * path, const char * message) {
  Serial.printf("Appending to file: %s\r\n", path);

  File file = fs.open(path, FILE_APPEND);
  if (!file) {
    Serial.println("- failed to open file for appending");
    return;
  }
  if (file.print(message)) {
    Serial.println("- message appended");
  } else {
    Serial.println("- append failed");
  }
  file.close();
}

void renameFile(fs::FS &fs, const char * path1, const char * path2) {
  Serial.printf("Renaming file %s to %s\r\n", path1, path2);
  if (fs.rename(path1, path2)) {
    Serial.println("- file renamed");
  } else {
    Serial.println("- rename failed");
  }
}

void deleteFile(fs::FS &fs, const char * path) {
  Serial.printf("Deleting file: %s\r\n", path);
  if (fs.remove(path)) {
    Serial.println("- file deleted");
  } else {
    Serial.println("- delete failed");
  }
}
// ------------------------------ Capture and Store Function ------------------------------------

void capturePhotoSaveLITTLEFS( void ) {

  camera_fb_t * fb = NULL; // pointer magic
  bool ok = 0; // Boolean indicating if the picture has been taken correctly

  do {
    // Take a photo with the camera
    Serial.println("Taking a photo...");

    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      picture_index--; // do not advance picture counter
      return;
    }

    // Photo file name for storage
    sprintf (pName, "/photo_%2d.jpg", picture_index);
    File file = FS_ID.open(pName, FILE_WRITE);
    // Insert the data in the photo file
    if (!file) {
      Serial.println("Failed to open file in writing mode");
      picture_index--; // do not advance picture counter
    }
    else {
      file.write(fb->buf, fb->len); // payload (image), payload length
      Serial.print("The picture has been saved in ");
      Serial.print(pName);
    }
    
    file.close();   // Close the file

    // Photo file name for display
    file = FS_ID.open(FILE_PHOTO, FILE_WRITE);
    if (!file) {
      Serial.println("Failed to open display file in writing mode");
    }
    else {
      file.write(fb->buf, fb->len); // payload (image), payload length
      Serial.print(" and ");
      Serial.println(FILE_PHOTO);
    }
    // Close the file
    file.close();

    esp_camera_fb_return(fb);

    // check if file has been correctly saved in SPIFFS
    ok = checkPhoto(FS_ID);
    //ok = true;
  } while ( !ok );
}

// Check if photo capture was successful
bool checkPhoto( fs::FS &fs ) {
  File f_pic = fs.open( FILE_PHOTO );
  unsigned int pic_sz = f_pic.size();
  return ( pic_sz > 100 );
}

// ------------------------------ Set-Up ------------------------------------

void setup() {

  Serial.begin (115200);  // Standart intro boilerplate follows
  WiFi.begin (ssid, password);
  Serial.println ("");
  Serial.print ("File Name: "); Serial.print (File_Name);
  Serial.print ("  Release Date: "); Serial.println (Date);
  Serial.println ("");
  Serial.print ("Opening WiFi Connection ");

  // Wait for connection
  while (WiFi.status () != WL_CONNECTED) {
    delay (500);
    Serial.print (".");
  }
  Serial.println ("");
  Serial.print ("Connected to ");
  Serial.println (ssid);
  Serial.print ("IP address: ");
  Serial.println (WiFi.localIP ());

  if (!LITTLEFS.begin(FORMAT_LITTLEFS_IF_FAILED)) {  // Initialize LITTLFS enven if not used
    Serial.println("LITTLEFS Mount Failed");
    return;
  }
  // Initialize uSD even if not used
  Serial.println(""); Serial.println("Starting SD Card"); Serial.println("");
  if (!SD_MMC.begin()) {
    Serial.println("SD Card Mount Failed");
  }

  // initialize EEPROM with predefined size
  EEPROM.begin(EEPROM_SIZE);
  fs::FS &fs = SD_MMC;

  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // Turn-off the 'brownout detector'

  // OV2640 camera module
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  
  // jpg setup
  if (psramFound()) {
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }
  // Camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    ESP.restart();
  }

  // Route for root / web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send_P(200, "text/html", index_html);
  });

  server.on("/capture", HTTP_GET, [](AsyncWebServerRequest * request) {
    takeNewPhoto = true;
    request->send_P(200, "text/plain", "Taking Photo");
  });

  server.on("/saved-photo", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(FS_ID, FILE_PHOTO, "image/jpg", false);
  });
 
  server.begin();  // Start server

  // Show storage contents on serial port as a function test
  Serial.print ("SD ");
  listDir(SD_MMC, "/", 0);
  Serial.println(); Serial.print ("flashMemory ");
  listDir(LITTLEFS, "/", 0);
  readFile(LITTLEFS, "/test.txt");
  Serial.println();
  Serial.println( "Test complete" );
  Serial.print( "MaxPictures set to: " ); Serial.println( maxPictures );  // don't blow thru the top of storage

  // Set-up FTP
  if (FS_ID.begin ()) {
    Serial.println ("File system opened (" + String (FS_NAME) + ")");
    ftpSrv.begin ("esp32", "esp32");    //username, password for ftpon default port 21
  }
  else {
    Serial.println ("File system could not be opened; ftp server will not work");
  }
  
  picture_index = readFileInt (FS_ID, "/picture_count.txt");  // Grab non-volatile picture count
  Serial.println();
  Serial.print ("Start picture index at: ");Serial.println(picture_index);
  Serial.println();
}

// ------------------------------ Loop ------------------------------------

void loop() {

  ftpSrv.handleFTP (FS_ID);        //call handleFTP()!

  if (takeNewPhoto) {
    capturePhotoSaveLITTLEFS();    // take a picture
    picture_index++;               // index picture counter
    if (picture_index > maxPictures) picture_index = 0; // keep index in bounds
    sprintf (pCount, "%d", picture_index);              // convert the counter to a char
    writeFile(FS_ID, "/picture_count.txt", pCount);     // write the counter to a file
    takeNewPhoto = false;         // loop thru Loop if no call for picture
  }
  delay(1);                       // all for WiFi service and housekeeping
}
