#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include "RepRapWebServer.h"
#include <EEPROM.h>
#include <FS.h>
#include <ESP8266SSDP.h>
#include "PooledStrings.cpp"
#include "SPITransaction.h"
#include "Config.h"

extern "C" {
#include "user_interface.h"     // for struct rst_info
}

#ifdef SHOW_PASSWORDS
# define PASSWORD_INPUT_TYPE  "\"text\""
#else
# define PASSWORD_INPUT_TYPE  "\"password\""
#endif

#define BUTTON_PIN -1
#define MAX_WIFI_FAIL 50
#define MAX_LOGGED_IN_CLIENTS 3

char ssid[32], pass[64], webhostname[64];
IPAddress sessions[MAX_LOGGED_IN_CLIENTS];
uint8_t loggedInClientsNum = 0;
MDNSResponder mdns;
RepRapWebServer server(80);
WiFiServer tcp(23);
WiFiClient tcpclient;
DNSServer dns;
String wifiConfigHtml;

enum class OperatingState
{
    Unknown = 0,
    Client = 1,
    AccessPoint = 2    
};

OperatingState currentState = OperatingState::Unknown;

ADC_MODE(ADC_VCC);          // need this for the ESP.getVcc() call to work

void fsHandler();
void handleRr();
void handleRrUpload();

void urldecode(String &input);
void StartAccessPoint();
void SendInfoToSam();
bool TryToConnect();

void setup() {
  Serial.begin(115200);
  delay(20);
  EEPROM.begin(512);
  delay(20);

  // Set up the SPI subsystem
  SPITransaction::Init();

  // Try to connect using the saved parameters
  bool success = TryToConnect();
  if (success)
  {    
    if (mdns.begin(webhostname, WiFi.localIP()))
    {
      MDNS.addService("http", "tcp", 80);
    }
      
    SSDP.setSchemaURL("description.xml");
    SSDP.setHTTPPort(80);
    SSDP.setName(webhostname);
    SSDP.setSerialNumber(WiFi.macAddress());
    SSDP.setURL("reprap.htm");
    SSDP.begin();
      
    SPIFFS.begin();
  
    server.servePrinter(true);
    server.onNotFound(fsHandler);
    server.onPrefix("/rr_", HTTP_ANY, handleRr, handleRrUpload);
    server.on("/description.xml", HTTP_GET, [](){SSDP.schema(server.client());});
  
    Serial.println(WiFi.localIP().toString());

    server.begin();
    tcp.begin();
  
    // The following causes a crash using release 2.1.0 of the Arduino ESP8266 core, and is probably unsafe even on 2.0.0
    //tcp.setNoDelay(true);

    currentState = OperatingState::Client;
  }
  else
  {
    StartAccessPoint();
    currentState = OperatingState::AccessPoint;
  }
  
  SendInfoToSam();
}

void loop()
{
  switch (currentState)
  {
  case OperatingState::Client:
    server.handleClient();
    break;

  case OperatingState::AccessPoint:
    server.handleClient();
    dns.processNextRequest();
    break;

  default:
    break;
  }
    
  SPITransaction::DoTransaction();
  if (SPITransaction::DataReady())
  {
    Serial.print("Incoming data, opcode=");
    Serial.print(SPITransaction::GetOpcode(), HEX);
    Serial.print(", length=");
    size_t length;
    (void)SPITransaction::GetData(length);
    Serial.print(length);
    Serial.println();
    SPITransaction::IncomingDataTaken();
  }
  yield();
}

// Try to connect using the saved SSID and password, returning true if successful
bool TryToConnect()
{
  EEPROM.get(0, ssid);
  EEPROM.get(32, pass);
  EEPROM.get(32+64, webhostname);

  wifi_station_set_hostname(webhostname);     // must do thia before calling WiFi.begin()
  uint8_t failcount = 0;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    failcount++;
    if (failcount % 2 == 0)
    {
      Serial.println("WAIT WIFI " + String(MAX_WIFI_FAIL/2 - (failcount/2)));
    }
    
    if (failcount > MAX_WIFI_FAIL)  // 1 min
    {
      Serial.println("WIFI ERROR");
      WiFi.mode(WIFI_STA);
      WiFi.disconnect();
      delay(100);
      return false;
    }
  }
  return true;
}

void StartAccessPoint()
{
  uint8_t num_ssids = WiFi.scanNetworks();
  // TODO: NONE? OTHER?
  wifiConfigHtml = F("<html><body><h1>Select your WiFi network:</h1><br /><form method=\"POST\">");
  for (uint8_t i = 0; i < num_ssids; i++) {
     wifiConfigHtml += "<input type=\"radio\" id=\"" + WiFi.SSID(i) + "\"name=\"ssid\" value=\"" + WiFi.SSID(i) + "\" /><label for=\"" + WiFi.SSID(i) + "\">" + WiFi.SSID(i) + "</label><br />";
  }
  wifiConfigHtml += F("<label for=\"password\">WiFi Password:</label><input type=" PASSWORD_INPUT_TYPE " id=\"password\" name=\"password\" /><br />");
  wifiConfigHtml += F("<p><label for=\"webhostname\">Duet host name: </label><input type=\"text\" id=\"webhostname\" name=\"webhostname\" value=\"duetwifi\" /><br />");
  wifiConfigHtml += F("<i>(This would allow you to access your printer by name instead of IP address. I.e. http://duetwifi/)</i></p>");
  wifiConfigHtml += F("<input type=\"submit\" value=\"Save and reboot\" /></form></body></html>");

  Serial.println("Found " + String(num_ssids) + " WIFI");

  delay(5000);
  IPAddress apIP(192, 168, 1, 1);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(softApName);
  Serial.println("WiFi -> DuetWiFi");
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", apIP);

  server.on("/", HTTP_GET, []() {
    server.send(200, FPSTR(STR_MIME_TEXT_HTML), wifiConfigHtml);
  });

  server.on("/", HTTP_POST, []() {
    if (server.args() <= 0) {
      server.send(500, FPSTR(STR_MIME_TEXT_PLAIN), F("Got no data, go back and retry"));
      return;
    }
    for (uint8_t e = 0; e < server.args(); e++) {
      String argument = server.arg(e);
      urldecode(argument);
      if (server.argName(e) == "password") argument.toCharArray(pass, 64);//pass = server.arg(e);
      else if (server.argName(e) == "ssid") argument.toCharArray(ssid, 32);//ssid = server.arg(e);
      else if (server.argName(e) == "webhostname") argument.toCharArray(webhostname, 64);
    }
    EEPROM.put(0, ssid);
    EEPROM.put(32, pass);
    EEPROM.put(32+64, webhostname);
    EEPROM.commit();
    server.send(200, FPSTR(STR_MIME_TEXT_HTML), F("<h1>All set!</h1><br /><p>(Please reboot me.)</p>"));
    Serial.println("SSID: " + String(ssid) + ", PASS: " + String(pass));
    delay(50);
    ESP.restart();
  });
  server.begin();
  Serial.println(WiFi.softAPIP().toString());
}

// Schedule an info message to the SAM processor
void SendInfoToSam()
{
  {
    struct
    {
      uint32_t formatVersion;
      uint32_t ip;
      uint32_t freeHeap;
      uint32_t resetReason;
      uint32_t flashSize;
      uint16_t operatingState;
      uint16_t vcc;
      char firmwareVersion[16];
      char hostName[64];
      char ssid[32];
    } response;

    response.formatVersion = 1;
    response.ip = static_cast<uint32_t>(WiFi.localIP());
    response.freeHeap = ESP.getFreeHeap();
    response.resetReason = ESP.getResetInfoPtr()->reason;
    response.flashSize = ESP.getFlashChipRealSize();
    response.operatingState = (uint32_t)currentState;
    response.vcc = ESP.getVcc();
    strncpy(response.firmwareVersion, firmwareVersion, sizeof(response.firmwareVersion));
    memcpy(response.hostName, webhostname, sizeof(response.hostName));
    switch (currentState)
    {
    case OperatingState::Client:
      memcpy(response.ssid, ssid, sizeof(response.ssid));
      break;

    case OperatingState::AccessPoint:
      strncpy(response.ssid, softApName, sizeof(response.ssid));
      break;

    default:
      response.ssid[0] = 0;
      break;
    }
    SPITransaction::ScheduleInfoMessage(SPITransaction::ttNetworkInfo, &response, sizeof(response));
  }
}

void fsHandler()
{
  String path = server.uri();
  if (path.endsWith("/"))
  {
    path += F("reprap.htm");            // default to reprap.htm as the index page
  }
  bool addedGz = false;
  File dataFile = SPIFFS.open(path, "r");
  if (!dataFile && !path.endsWith(".gz") && path.length() <= 29)
  {
    // Requested file not found and wasn't a zipped file, so see if we have a zipped version
    path += F(".gz");
    addedGz = true;
    dataFile = SPIFFS.open(path, "r");
  }
  if (!dataFile)
  {
    server.send(404, FPSTR(STR_MIME_APPLICATION_JSON), "{\"err\": \"404: " + server.uri() + " NOT FOUND\"}");
    return;
  }
  // No need to add the file size or encoding headers here because streamFile() does that automatically
  String dataType = FPSTR(STR_MIME_TEXT_PLAIN);
  if (path.endsWith(".html") || path.endsWith(".htm")) dataType = FPSTR(STR_MIME_TEXT_HTML);
  else if (path.endsWith(".css") || path.endsWith(".css.gz")) dataType = F("text/css");
  else if (path.endsWith(".js") || path.endsWith(".js.gz")) dataType = F("application/javascript");
  else if (!addedGz && path.endsWith(".gz")) dataType = F("application/x-gzip");
  server.streamFile(dataFile, dataType);
  dataFile.close();
}

// Handle a rr_ request from the client
void handleRr() {
#ifdef SPI_DEBUG
  Serial.print("handleRr: ");
  Serial.print(server.uri());
  Serial.println();
#endif

  uint32_t postLength = server.getPostLength();
  String text = server.fullUri();
  if (postLength != 0)
  {
    text += "&length=" + (String)postLength;    // pass the post length to the SAM as well
  }
  const uint32_t ip = static_cast<uint32_t>(server.client().remoteIP());
  SPITransaction::ScheduleRequestMessage(SPITransaction::trTypeRequest | SPITransaction::ttRr, ip, postLength == 0, text.c_str() + 4, text.length() - 4);
  uint32_t now = millis();
  bool hadReply = false;
  uint32_t fragment = 1;
  do
  {
    // Send our data and/or get a response
    SPITransaction::DoTransaction();              // try to do a transaction, if the SAM is willing

    // See if we have a response yet
    if (SPITransaction::DataReady())
    {
      uint32_t opcode = SPITransaction::GetOpcode();
      size_t length;
      const uint8_t *data = (const uint8_t*)SPITransaction::GetData(length);
      if (opcode == (SPITransaction::trTypeResponse | SPITransaction::ttRr))
      {
#ifdef SPI_DEBUG
        Serial.print("Reply");
        for (size_t i = 0; i <= length; ++i)
        {
          Serial.print(" ");
          Serial.print((unsigned int)data[i], HEX);
        }
        Serial.println();
#endif
        bool isLast;
        uint32_t fragment = SPITransaction::GetFragment(isLast);
        if (fragment == 0 && length >= 8)
        {
          uint32_t rc = *(const uint32_t*)data;
          uint32_t contentLength = *(const uint32_t*)(data + 4);
          if (rc & SPITransaction::rcJson)
          {
            server.send(rc & SPITransaction::rcNumber, contentLength, FPSTR(STR_MIME_APPLICATION_JSON), data + 8, length - 8, isLast);
          }
          else
          {
            server.send(rc & SPITransaction::rcNumber, contentLength, FPSTR(STR_MIME_TEXT_PLAIN), data + 8, length - 8, isLast);
          }
        }
        else
        {
          server.sendMore(data, length, isLast);
        }
        SPITransaction::IncomingDataTaken();
        if (isLast)
        {
          hadReply = true;
        }
      }
      else
      {
//        Serial.print("Incoming data, opcode=");
//        Serial.print(opcode, HEX);
//        Serial.print(", length=");
//        Serial.print(length);
//        Serial.println();
        SPITransaction::IncomingDataTaken();
      }
    }

    // Send our next fragment of postdata
    if (postLength != 0)
    {
      uint8_t* buf;
      size_t len;
      if (SPITransaction::GetBufferAddress(&buf, len))      // if the output buffer is free
      {
        if (len > postLength)
        {
          len = postLength;
        }
        size_t len2 = server.readPostdata(server.client(), buf, len);
        if (len2 != len)
        {
          Serial.print("read ");
          Serial.print(len2);
          Serial.print(" bytes but ");
          Serial.print(postLength);
          Serial.println(" remaining");
        }
        if (len2 != 0)
        {
          postLength -= len2;
#ifdef SPI_DEBUG
          Serial.print("sending POST fragment, bytes=");
          Serial.print(len2);
          Serial.print(" remaining=");
          Serial.println(postLength);
#endif
          SPITransaction::SchedulePostdataMessage(SPITransaction::trTypeRequest | SPITransaction::ttRr, ip, len2, fragment, postLength == 0);
          ++fragment;
          now = millis();
        }
        else
        {
          Serial.println("read 0 bytes");
        }
      }
      else
      {
        yield();
        //delay(1);
      }
    }
    else
    {
      yield();
      //delay(1);
    }
    
    // Quit if all done
    if (hadReply && postLength == 0)
    {
      return;
    }
  } while (millis() - now < 5000);
  
  server.send(200, FPSTR(STR_MIME_APPLICATION_JSON), FPSTR(STR_JSON_ERR_1));
}

void handleRrUpload() {
}

void urldecode(String &input) { // LAL ^_^
  input.replace("%0A", String('\n'));
  input.replace("%20", " ");
  input.replace("+", " ");
  input.replace("%21", "!");
  input.replace("%22", "\"");
  input.replace("%23", "#");
  input.replace("%24", "$");
  input.replace("%25", "%");
  input.replace("%26", "&");
  input.replace("%27", "\'");
  input.replace("%28", "(");
  input.replace("%29", ")");
  input.replace("%30", "*");
  input.replace("%31", "+");
  input.replace("%2C", ",");
  input.replace("%2E", ".");
  input.replace("%2F", "/");
  input.replace("%2C", ",");
  input.replace("%3A", ":");
  input.replace("%3A", ";");
  input.replace("%3C", "<");
  input.replace("%3D", "=");
  input.replace("%3E", ">");
  input.replace("%3F", "?");
  input.replace("%40", "@");
  input.replace("%5B", "[");
  input.replace("%5C", "\\");
  input.replace("%5D", "]");
  input.replace("%5E", "^");
  input.replace("%5F", "-");
  input.replace("%60", "`");
}

