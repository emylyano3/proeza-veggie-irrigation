#include <FS.h>              
#include <ESP8266WiFi.h>       
#include <ESP8266WebServer.h>
#include <WiFiClient.h>
#include <ESP8266HTTPUpdateServer.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h> 
#include <ESP8266mDNS.h>

extern "C" {
  #include "user_interface.h"
}

const char HTTP_HEAD[] PROGMEM                      = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/><title>{v}</title>";
const char HTTP_STYLE[] PROGMEM                     = "<style>.c{text-align: center;} div,input{padding:5px;font-size:1em;} input{width:95%;margin-top:3px;margin-bottom:3px;} body{text-align: center;font-family:verdana;} button{border:0;border-radius:0.3rem;background-color:#1fa3ec;color:#fff;line-height:2.4rem;font-size:1.2rem;width:100%;margin-top:3px;} .q{float: right;width: 64px;text-align: right;} .l{background: url(\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAMAAABEpIrGAAAALVBMVEX///8EBwfBwsLw8PAzNjaCg4NTVVUjJiZDRUUUFxdiZGSho6OSk5Pg4eFydHTCjaf3AAAAZElEQVQ4je2NSw7AIAhEBamKn97/uMXEGBvozkWb9C2Zx4xzWykBhFAeYp9gkLyZE0zIMno9n4g19hmdY39scwqVkOXaxph0ZCXQcqxSpgQpONa59wkRDOL93eAXvimwlbPbwwVAegLS1HGfZAAAAABJRU5ErkJggg==\") no-repeat left center;background-size: 1em;}</style>";
const char HTTP_SCRIPT[] PROGMEM                    = "<script>function c(l){document.getElementById('s').value=l.innerText||l.textContent;document.getElementById('p').focus();}</script>";
const char HTTP_HEAD_END[] PROGMEM                  = "</head><body><div style='text-align:left;display:inline-block;min-width:260px;'>";
const char HTTP_ITEM[] PROGMEM                      = "<div><a href='#p' onclick='c(this)'>{v}</a>&nbsp;<span class='q {i}'>{r}%</span></div>";
const char HTTP_FORM_START[] PROGMEM                = "<form method='get' action='wifisave'><input id='s' name='s' length=32 placeholder='SSID' required><br/><input id='p' name='p' length=64 type='password' placeholder='password' required><hr/>";
const char HTTP_FORM_INPUT[] PROGMEM                = "<input id='{i}' name='{n}' placeholder='{p}' maxlength={l} value='{v}' {c}><br/>";
const char HTTP_FORM_INPUT_LIST[] PROGMEM           = "<input id='{i}' name='{n}' placeholder='{p}' list='d' {c}><datalist id='d'{o}></datalist><br/>";
const char HTTP_FORM_INPUT_LIST_OPTION[] PROGMEM    = "<option>{o}</option>";
const char HTTP_FORM_END[] PROGMEM                  = "<hr/><button type='submit'>Save</button></form>";
const char HTTP_SCAN_LINK[] PROGMEM                 = "<br/><div class=\"c\"><a href=\"/scan\">Scan for networks</a></div>";
const char HTTP_SAVED[] PROGMEM                     = "<div>Credentials Saved<br/>Trying to connect ESP to network.<br/>If it fails reconnect to AP to try again</div>";
const char HTTP_END[] PROGMEM                       = "</div></body></html>";

// DNS server
const uint8_t DNS_PORT = 53;

const char*   CONFIG_FILE   = "/config.json";

/* Possible switch states */
const char    STATE_OFF      = '0';
const char    STATE_ON       = '1';

/* Module settings */
#ifdef ESP01
const String  MODULE_TYPE  = "light";
const String  CHANNEL_TYPE = MODULE_TYPE;
#else
const String  MODULE_TYPE  = "lightStation";
const String  CHANNEL_TYPE = "light";
#endif

enum InputType {Combo, Text};

struct ConfigParam {
  const char*         name;       // identificador
  const char*         label;      // legible por usuario
  char*               value;      // valor default
  uint8_t             length;     // longitud limite
  InputType           type;       // tipo de control en formularion
  const char*         customHTML; // html custom
  std::vector<char*>  options;    // optciones para el combo

  ConfigParam() {
  }

  ConfigParam (InputType _type, const char* _name, const char* _label, const char* _defVal, uint8_t _length, const char* _html) {
    type = _type;
    name = _name;
    label = _label;
    customHTML = _html;
    length = _length;
    value = new char[length + 1];
    updateValue(_defVal);
  }

  ~ConfigParam() {
    delete[] value;
  }
  
  void updateValue (const char *v) {
    String s = String(v);
    s.toCharArray(value, length);
  }
};

struct Channel {
  uint8_t       switchPin;
  int           switchState;
  uint8_t       relayPin;
  char          relayState;
  ConfigParam*  nameConfig;

  Channel(uint8_t _sp, int _ss, uint8_t _rp, char _rs, ConfigParam* _cfgp) {
    switchPin = _sp;
    switchState = _ss;
    relayPin = _rp;
    relayState = _rs;
    nameConfig = _cfgp;
  }
};

std::unique_ptr<ESP8266WebServer>  _server;
std::unique_ptr<DNSServer>         _dnsServer;

bool            _connect;
const char*     _ssid               = "";
const char*     _pass               = "";
const char*     _apPass             = "12345678";
char            _stationName[PARAM_LENGTH * 3 + 4];

#ifdef WIFI_MIN_QUALITY
const uint8_t   _minimumQuality     = WIFI_MIN_QUALITY;
#else
const uint8_t   _minimumQuality = -1;
#endif

#ifdef WIFI_CONN_TIMEOUT
const long      _connectionTimeout  = WIFI_CONN_TIMEOUT * 1000;
#else
const uint16_t  _connectionTimeout  = 0;
#endif

WiFiClient              _wifiClient;
PubSubClient            _mqttClient(_wifiClient);
ESP8266WebServer        _httpServer(80);
ESP8266HTTPUpdateServer _httpUpdater;
long                    _nextBrokerConnAtte = 0;

ConfigParam _moduleNameCfg     = {Text, "moduleName", "Module name", "", PARAM_LENGTH, "required"};
ConfigParam _moduleLocationCfg = {Text, "moduleLocation", "Module location", "", PARAM_LENGTH, "required"};
ConfigParam _mqttPortCfg       = {Text, "mqttPort", "MQTT port", "", PARAM_LENGTH, ""};
ConfigParam _mqttHostCfg       = {Text, "mqttHost", "MQTT host", "", PARAM_LENGTH, ""};

#ifndef ESP01
ConfigParam _cfgChA = {Text, "channelA", "Channel A", "", PARAM_LENGTH, ""};
ConfigParam _cfgChB = {Text, "channelB", "Channel B", "", PARAM_LENGTH, ""};
ConfigParam _cfgChC = {Text, "channelC", "Channel C", "", PARAM_LENGTH, ""};
#endif

#ifdef ESP01
Channel _channels[] = {
  {3, LOW, 2, STATE_OFF, &_moduleNameCfg}
};
ConfigParam   _configParams[] = {_moduleLocationCfg, _moduleNameCfg, _mqttHostCfg, _mqttPortCfg};

const uint8_t PARAMS_COUNT    = 4;
const uint8_t CHANNELS_COUNT  = 1;
const uint8_t TX_PIN          = 1;
#elif NODEMCUV2
Channel _channels[] = {
  {D7, LOW, D1, STATE_OFF, &_cfgChA},
  {D6, LOW, D2, STATE_OFF, &_cfgChB},
  {D0, LOW, D4, STATE_OFF, &_cfgChC}
};

ConfigParam   _configParams[] = {_cfgChA, _cfgChB, _cfgChC, _moduleLocationCfg, _moduleNameCfg, _mqttHostCfg, _mqttPortCfg};

const uint8_t PARAMS_COUNT    = 7;
const uint8_t CHANNELS_COUNT  = 3;
#else
Channel _channels[] = {
  {13, LOW, 5, STATE_OFF, &_cfgChA},
  {12, LOW, 4, STATE_OFF, &_cfgChB},
  {16, LOW, 2, STATE_OFF, &_cfgChC}
};

ConfigParam   _configParams[] = {_cfgChA, _cfgChB, _cfgChC, _moduleLocationCfg, _moduleNameCfg, _mqttHostCfg, _mqttPortCfg};

const uint8_t PARAMS_COUNT    = 7;
const uint8_t CHANNELS_COUNT  = 3;
#endif

template <class T> void log (T text) {
  if (LOGGING) {
    Serial.print("*SW: ");
    Serial.println(text);
  }
}

template <class T, class U> void log (T key, U value) {
  if (LOGGING) {
    Serial.print("*SW: ");
    Serial.print(key);
    Serial.print(": ");
    Serial.println(value);
  }
}

void setup() {
#ifdef ESP01
  //to avoid using pin 0 as input
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY, TX_PIN);
#else
  Serial.begin(115200);
#endif
  delay(500);
  Serial.println();
  log("Starting module");
  connectWifiNetwork(loadConfig());
  log(F("Connected to wifi network. Local IP"), WiFi.localIP());
  // pins settings
  for (size_t i = 0; i < CHANNELS_COUNT; ++i) {
    pinMode(_channels[i].relayPin, OUTPUT);
    pinMode(_channels[i].switchPin, INPUT);
    digitalWrite(_channels[i].switchPin, HIGH);
  }
  log(F("Configuring MQTT broker"));
  String port = String(_mqttPortCfg.value);
  log(F("Port"), port);
  log(F("Server"), _mqttHostCfg.value);
  _mqttClient.setServer(_mqttHostCfg.value, (uint16_t) port.toInt());
  _mqttClient.setCallback(receiveMqttMessage);

  // OTA Update Stuff
  MDNS.begin(getStationName());
  MDNS.addService("http", "tcp", 80);
  _httpUpdater.setup(&_httpServer);
  _httpServer.begin();
  log("HTTPUpdateServer ready."); 
  log("Open http://" + String(getStationName()) + ".local/update");
  log("Open http://" + WiFi.localIP().toString() + "/update");
}

bool loadConfig() { 
  //read configuration from FS json
  if (SPIFFS.begin()) {
    if (SPIFFS.exists(CONFIG_FILE)) {
      //file exists, reading and loading
      File configFile = SPIFFS.open(CONFIG_FILE, "r");
      if (configFile) {
        size_t size = configFile.size();
        if (size > 0) {
          // Allocate a buffer to store contents of the file.
          char buf[size];
          configFile.readBytes(buf, size);
          DynamicJsonBuffer jsonBuffer;
          JsonObject& json = jsonBuffer.parseObject(buf);
          log("Configuration file loaded");
          if (LOGGING) {
            json.printTo(Serial);
            Serial.println();
          }
          if (json.success()) {
            for (uint8_t i = 0; i < PARAMS_COUNT; ++i) {
              _configParams[i].updateValue(json[_configParams[i].name]);
              log(_configParams[i].name, _configParams[i].value);
            }
            return true;
          } else {
            log(F("Failed to load json config"));
          }
        } else {
          log(F("Config file empty"));
        }
      } else {
        log(F("Config file found but could not be opened"));
      }
      configFile.close();
    } else {
      log(F("No config file found"));
    }
  } else {
    log(F("Failed to mount FS"));
  }
  return false;
}

/** callback notifying the need to save config */
void saveConfig () {
  File configFile = SPIFFS.open(CONFIG_FILE, "w");
  if (configFile) {
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    //TODO Trim param values
    for (uint8_t i = 0; i < PARAMS_COUNT; ++i) {
      json[_configParams[i].name] = _configParams[i].value;
    }
    json.printTo(configFile);
    log("Configuration file saved");
    json.printTo(Serial);
    Serial.println();
    configFile.close();
  } else {
    log(F("Failed to open config file for writing"));
  }
}

void loop() {
  _httpServer.handleClient();
  processPhysicalInput();
  if (!_mqttClient.connected()) {
    connectBroker();
  }
  _mqttClient.loop();
}

void receiveMqttMessage(char* topic, unsigned char* payload, unsigned int length) {
  log(F("Received mqtt message topic"), topic);
  for (size_t i = 0; i < CHANNELS_COUNT; ++i) {
    if (isChannelEnabled(&_channels[i])) {
      if (getChannelTopic(&_channels[i], "cmd").equals(topic)) {
        processCommand(&_channels[i], payload, length);
        return;
      }
    }
  }
  if (String(topic).equals(getStationTopic("hrst"))) {
    hardReset();
  } else {
    log(F("Unknown topic"));
  }
}

void hardReset () {
  log(F("Doing a module hard reset"));
  SPIFFS.format();
  WiFi.disconnect();
  delay(200);
  ESP.restart();
}

void publishState (Channel *c) {
  _mqttClient.publish(getChannelTopic(c, "state").c_str(), new char[2]{c->relayState, '\0'});
}

void processCommand (Channel *c, unsigned char* payload, unsigned int length) {
  if (length != 1 || !payload) {
    log(F("Invalid payload. Ignoring."));
    return;
  }
  if (!isDigit(payload[0])) {
    log(F("Invalid payload format. Ignoring."));
    return;
  }
  switch (payload[0]) {
    case '0':
    case '1':
      setRelayState(c, payload[0]);
    break;
    default:
      log(F("Invalid state"), payload[0]);
    return;
  } 
  publishState(c);
}

void processPhysicalInput() {
  for (size_t i = 0; i < CHANNELS_COUNT; ++i) {
    if (isChannelEnabled(&_channels[i])) {
      processChannelInput(&_channels[i]);
    }
  }
}

void processChannelInput(Channel *c) {
  int read = digitalRead(c->switchPin);
  if (read != c->switchState) {
    log(F("Phisical switch state has changed. Updating module"), c->nameConfig->value);
    c->switchState = read;
    flipRelayState(c);
    publishState(c);
  }
}

bool isChannelEnabled (Channel *c) {
  return c->nameConfig->value != NULL && strlen(c->nameConfig->value) > 0;
}

void setRelayState (Channel *c, char newState) {
  if (c->relayState != newState) {
    flipRelayState(c);
  } else {
    log("Channel previous state is the same, no update done", c->nameConfig->value);
  }
}

void flipRelayState (Channel *c) {
  c->relayState = c->relayState == STATE_OFF ? STATE_ON : STATE_OFF;
  switch (c->relayState) {
    case STATE_OFF:
      digitalWrite(c->relayPin, LOW);
      break;
    case STATE_ON:
      digitalWrite(c->relayPin, HIGH);
      break;
    default:
      break;
  }
  log(F("Channel updated"), c->nameConfig->value);
  log(F("State changed to"), c->relayState);
}

void connectBroker() {
  if (_nextBrokerConnAtte <= millis()) {
    _nextBrokerConnAtte = millis() + MQTT_BROKER_CONNECTION_RETRY;
    log(F("Connecting MQTT broker as"), getStationName());
    if (_mqttClient.connect(getStationName())) {
      log(F("MQTT broker Connected"));
      #ifdef ESP01
      subscribeTopic(getStationTopic("hrst").c_str());
      #else
      subscribeTopic(getStationTopic("#").c_str());
      #endif
      for (size_t i = 0; i < CHANNELS_COUNT; ++i) {
        if (isChannelEnabled(&_channels[i])) {
          subscribeTopic(getChannelTopic(&_channels[i], "cmd").c_str());
        }
      }
    }
  } else {
    log(F("Failed. RC:"), _mqttClient.state());
  }
}

void connectWifiNetwork (bool existsConfig) {
  log(F("Connecting to wifi network"));
  bool connected = false;
  while (!connected) {
    if (existsConfig) {
        log(F("Connecting to saved network"));
        if (connectWiFi() == WL_CONNECTED) {
          connected = true;
        } else {
          log(F("Could not connect to saved network. Going into config mode."));
          connected = startConfigPortal();
        }
    } else {
      log(F("Going into config mode cause no config was found"));
      connected = startConfigPortal();
    }
  }
  // free mem used for configuration
  for (uint8_t i = 0; i < PARAMS_COUNT; ++i) {
    _configParams[i].name = NULL;
    _configParams[i].label = NULL;
    _configParams[i].customHTML = NULL;
  }
}

bool startConfigPortal() {
  WiFi.mode(WIFI_AP_STA);
  _connect = false;
  setupConfigPortal();
  while(1) {
    _dnsServer->processNextRequest();
    _server->handleClient();
    if (_connect) {
      _connect = false;
      delay(1000);
      log(F("Connecting to new AP"));
      // using user-provided  _ssid, _pass in place of system-stored ssid and pass
      if (connectWifi(_ssid, _pass) != WL_CONNECTED) {
        log(F("Failed to connect."));
      } else {
        WiFi.mode(WIFI_STA);
        //notify that configuration has changed and any optional parameters should be saved
        saveConfig();
        break;
      }
    }
    yield();
  }
  _server.reset();
  _dnsServer.reset();
  return  WiFi.status() == WL_CONNECTED;
}

uint8_t connectWifi(String ssid, String pass) {
  log(F("Connecting as wifi client..."));
  if (WiFi.status() == WL_CONNECTED) {
    log("Already connected. Bailing out.");
    return WL_CONNECTED;
  }
  WiFi.hostname(getStationName());
  WiFi.begin(ssid.c_str(), pass.c_str());
  return waitForConnectResult();
}

uint8_t connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.hostname(getStationName());
  if (WiFi.SSID()) {
    log("Using last saved values, should be faster");
    //trying to fix connection in progress hanging
    ETS_UART_INTR_DISABLE();
    wifi_station_disconnect();
    ETS_UART_INTR_ENABLE();
    WiFi.begin();
    return waitForConnectResult();
  } else {
    log("No saved credentials");
    return WL_CONNECT_FAILED;
  }
}

uint8_t waitForConnectResult() {
  if (_connectionTimeout == 0) {
    return WiFi.waitForConnectResult();
  } else {
    log(F("Waiting for connection result with time out"));
    unsigned long start = millis();
    boolean keepConnecting = true;
    uint8_t status, retry = 0;
    while (keepConnecting) {
      status = WiFi.status();
      if (millis() > start + _connectionTimeout) {
        keepConnecting = false;
        log(F("Connection timed out"));
      }
      if (status == WL_CONNECTED) {
        keepConnecting = false;
      } else if (status == WL_CONNECT_FAILED) {
        log(F("Connection failed. Retrying: "));
        log(++retry);
        log("Trying to begin connection again");
        WiFi.begin();
      }
      delay(100);
    }
    return status;
  }
}

void setupConfigPortal() {
  _server.reset(new ESP8266WebServer(80));
  _dnsServer.reset(new DNSServer());
  String id = String(ESP.getChipId());
  const char* apName = id.c_str();
  log(F("Configuring access point... "), apName);
  if (_apPass != NULL) {
    if (strlen(_apPass) < 8 || strlen(_apPass) > 63) {
      log(F("Invalid AccessPoint password. Ignoring"));
      _apPass = NULL;
    }
    log(_apPass);
  }
  WiFi.softAPConfig(IPAddress(10,10,10,10),IPAddress(IPAddress(10,10,10,10)),IPAddress(IPAddress(255,255,255,0)));
  if (_apPass != NULL) {
    WiFi.softAP(apName, _apPass);
  } else {
    WiFi.softAP(apName);
  }
  // Without delay I've seen the IP address blank
  delay(500); 
  log(F("AP IP address: "), WiFi.softAPIP());
  /* Setup the DNS server redirecting all the domains to the apIP */
  _dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  _dnsServer->start(DNS_PORT, "*", WiFi.softAPIP());
  /* Setup web pages */
  _server->on("/", std::bind(handleWifi, false));
  _server->on("/config", std::bind(handleWifi, false));
  //Microsoft captive portal. Maybe not needed. Might be handled by notFound handler.
  _server->on("/scan", std::bind(handleWifi, true)); 
  _server->on("/wifisave", handleWifiSave);
  _server->onNotFound(handleNotFound);
  _server->begin();
  log(F("HTTP server started"));
}

void handleWifi(bool scan) {
  // If captive portal redirect instead of displaying the page.
  if (captivePortal()) { 
    return;
  }
  String page = FPSTR(HTTP_HEAD);
  page.replace("{v}", "Proeza Domotics");
  page += FPSTR(HTTP_SCRIPT);
  page += FPSTR(HTTP_STYLE);
  page += "<h2>Module config</h2>";
  page += FPSTR(HTTP_HEAD_END);
  if (scan) {
    int n = WiFi.scanNetworks();
    log(F("Scan done"));
    if (n == 0) {
      log(F("No networks found"));
      page += F("No networks found. Refresh to scan again.");
    } else {
      //sort networks
      int indices[n];
      for (int i = 0; i < n; i++) {
        indices[i] = i;
      }
      // old sort
      for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
          if (WiFi.RSSI(indices[j]) > WiFi.RSSI(indices[i])) {
            std::swap(indices[i], indices[j]);
          }
        }
      }
      // remove duplicates ( must be RSSI sorted )
      String cssid;
      for (int i = 0; i < n; i++) {
        if (indices[i] == -1) continue;
        cssid = WiFi.SSID(indices[i]);
        for (int j = i + 1; j < n; j++) {
          if (cssid == WiFi.SSID(indices[j])) {
            log("DUP AP: " + WiFi.SSID(indices[j]));
            indices[j] = -1; // set dup aps to index -1
          }
        }
      }
      //display networks in page
      for (int i = 0; i < n; i++) {
        if (indices[i] == -1) continue; // skip dups
        log(WiFi.SSID(indices[i]));
        log(WiFi.RSSI(indices[i]));
        int quality = getRSSIasQuality(WiFi.RSSI(indices[i]));
        if (_minimumQuality == -1 || _minimumQuality < quality) {
          String item = FPSTR(HTTP_ITEM);
          String rssiQ;
          rssiQ += quality;
          item.replace("{v}", WiFi.SSID(indices[i]));
          item.replace("{r}", rssiQ);
          if (WiFi.encryptionType(indices[i]) != ENC_TYPE_NONE) {
            item.replace("{i}", "l");
          } else {
            item.replace("{i}", "");
          }
          page += item;
        } else {
          log(F("Skipping due to quality"));
        }
      }
      page += "<br/>";
    }
  }
  page += FPSTR(HTTP_FORM_START);
  char parLength[5];
  // add the extra parameters to the form
  for (int i = 0; i < PARAMS_COUNT; i++) {
    if (_configParams[i].name != NULL) {
      if (_configParams[i].type == Combo) {
        String pitem = FPSTR(HTTP_FORM_INPUT_LIST);
        pitem.replace("{i}", _configParams[i].name);
        pitem.replace("{n}", _configParams[i].name);
        String ops = "";
        for (size_t j = 0; j < _configParams[i].options.size(); ++j) {
          String op = FPSTR(HTTP_FORM_INPUT_LIST_OPTION);
          op.replace("{o}", _configParams[i].options[j]);
          ops.concat(op);
        }
        pitem.replace("{p}", _configParams[i].label);
        pitem.replace("{o}", ops);
        pitem.replace("{c}", _configParams[i].customHTML);
        page += pitem;
      } else {
        String pitem = FPSTR(HTTP_FORM_INPUT);
        pitem.replace("{i}", _configParams[i].name);
        pitem.replace("{n}", _configParams[i].name);
        pitem.replace("{p}", _configParams[i].label);
        snprintf(parLength, 5, "%d", _configParams[i].length);
        pitem.replace("{l}", parLength);
        pitem.replace("{v}", _configParams[i].value);
        pitem.replace("{c}", _configParams[i].customHTML);
        page += pitem;
      }
    } 
  }
  page += FPSTR(HTTP_FORM_END);
  page += FPSTR(HTTP_SCAN_LINK);
  page += FPSTR(HTTP_END);
  _server->sendHeader("Content-Length", String(page.length()));
  _server->send(200, "text/html", page);
  log(F("Sent config page"));
}

void handleNotFound() {
  // If captive portal redirect instead of displaying the error page.
  if (captivePortal()) { 
    return;
  }
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += _server->uri();
  message += "\nMethod: ";
  message += _server->method() == HTTP_GET ? "GET" : "POST";
  message += "\nArguments: ";
  message += _server->args();
  message += "\n";
  for (int i = 0; i < _server->args(); i++) {
    message += " " + _server->argName(i) + ": " + _server->arg(i) + "\n";
  }
  _server->sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  _server->sendHeader("Pragma", "no-cache");
  _server->sendHeader("Expires", "-1");
  _server->sendHeader("Content-Length", String(message.length()));
  _server->send(404, "text/plain", message);
}

/** Handle the WLAN save form and redirect to WLAN config page again */
void handleWifiSave() {
  _ssid = _server->arg("s").c_str();
  _pass = _server->arg("p").c_str();
  //parameters
  for (int i = 0; i < PARAMS_COUNT; i++) {
    //read parameter
    String value = _server->arg(_configParams[i].name);
    //store it in array
    value.toCharArray(_configParams[i].value, _configParams[i].length);
    log(_configParams[i].name, value);
  }
  String page = FPSTR(HTTP_HEAD);
  page.replace("{v}", "Credentials Saved");
  page += FPSTR(HTTP_SCRIPT);
  page += FPSTR(HTTP_STYLE);
  page += "<h2>Module config</h2>";
  page += FPSTR(HTTP_HEAD_END);
  page += FPSTR(HTTP_SAVED);
  page += FPSTR(HTTP_END);
  _server->sendHeader("Content-Length", String(page.length()));
  _server->send(200, "text/html", page);
  _connect = true; //signal ready to connect/reset
}

/** Redirect to captive portal if we got a request for another domain. Return true in that case so the page handler do not try to handle the request again. */
bool captivePortal() {
  if (!isIp(_server->hostHeader()) ) {
    log(F("Request redirected to captive portal"));
    _server->sendHeader("Location", String("http://") + toStringIp(_server->client().localIP()), true);
    _server->send ( 302, "text/plain", ""); // Empty content inhibits Content-length header so we have to close the socket ourselves.
    _server->client().stop(); // Stop is needed because we sent no content length
    return true;
  }
  return false;
}

char* getStationName () {
  if (strlen(_stationName) <= 0) {
    size_t size = MODULE_TYPE.length() + _moduleLocationCfg.length + _moduleNameCfg.length + 4;
    String sn;
    sn.concat(MODULE_TYPE);
    sn.concat("_");
    sn.concat(_moduleLocationCfg.value); 
    sn.concat("_");
    sn.concat(_moduleNameCfg.value);
    sn.toCharArray(_stationName, size);
  } 
  return _stationName;
}

bool isIp(String str) {
  for (int i = 0; i < str.length(); i++) {
    int c = str.charAt(i);
    if (c != '.' && (c < '0' || c > '9')) {
      return false;
    }
  }
  return true;
}

/** IP to String? */
String toStringIp(IPAddress ip) {
  String res = "";
  for (int i = 0; i < 3; i++) {
    res += String((ip >> (8 * i)) & 0xFF) + ".";
  }
  res += String(((ip >> 8 * 3)) & 0xFF);
  return res;
}

int getRSSIasQuality(int RSSI) {
  int quality = 0;
  if (RSSI <= -100) {
      quality = 0;
  } else if (RSSI >= -50) {
      quality = 100;
  } else {
      quality = 2 * (RSSI + 100);
  }
  return quality;
}

void subscribeTopic(const char *t) {
  log("Subscribing mqtt topic", t);
  _mqttClient.subscribe(t);
}

String getChannelTopic (Channel *c, String cmd) {
  return CHANNEL_TYPE + F("/") + _moduleLocationCfg.value + F("/") + c->nameConfig->value + F("/") + cmd;
}

String getStationTopic (String cmd) {
  return MODULE_TYPE + F("/") + _moduleLocationCfg.value + F("/") + _moduleNameCfg.value + F("/") + cmd;
}
