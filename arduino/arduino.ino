#include <FS.h> //this needs to be first, or it all crashes and burns...
#include <ESP8266WiFi.h>
#include <Esp.h>
#include "WiFiManager/WiFiManager.h"
#include <ArduinoJson.h>
#include "config.h"
#include <GDBStub.h>

#define CONFIG_FILE "/config.ini"

const int DISCOVER_PORT = 24320;

char node_type[32];
char node_desc[32];
char mqtt_server[40];
int mqtt_port;
char static_ip[16] = "";
char static_nm[16] = "";
char static_gw[16] = "";
char dns[40] = "";
bool shouldSaveConfig;

void print_str(const char *name, const char *val)
{
  Serial.print(name);
  Serial.print(": \"");
  Serial.print(val);
  Serial.println('"');
}

char nibbleToChar(uint32_t val)
{
  val &=0xF;
  if (val < 10)
    return '0' + val;
  else
    return 'A' + val - 10;
}

void spiffs_mount() {
  if (!SPIFFS.begin()) {
    Serial.println("SPIFFs not initialized, reinitializing");
    if (!SPIFFS.format()) {
      Serial.println("SPIFFs format failed.");
      while (1);
    }
  }
}

void config_load() {
  Serial.println("SPIFFS initialized");
  Config cfg(CONFIG_FILE);

  Serial.println("Loading config");
  cfg.readFile();
  Serial.println("Config loaded");
  strcpy(static_ip, cfg.getValueStr("ip"));
  strcpy(static_gw, cfg.getValueStr("gw"));
  strcpy(static_nm, cfg.getValueStr("nm"));
  strcpy(static_nm, cfg.getValueStr("dns"));
  strcpy(node_desc, cfg.getValueStr("desc"));

  print_str("IP", static_ip);
  print_str("GW", static_gw);
  print_str("NM", static_nm);
  print_str("DNS", static_nm);
  print_str("Node Desc", node_desc);
}

void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void net_config() {
  WiFiManager wifiManager;

  WiFiManagerParameter node_desc_param("desc", "Description", node_desc, 32);
  WiFiManagerParameter custom_ip("ip", "Static IP", static_ip, 40);
  WiFiManagerParameter custom_gw("gw", "Static Gateway", static_gw, 5);
  WiFiManagerParameter custom_nm("nm", "Static Netmask", static_nm, 5);
  WiFiManagerParameter custom_dns("dns", "Domain Name Server", dns, 40);
  
  if (strlen(static_ip) > 0) {
    IPAddress _ip, _gw, _nm;
    _ip.fromString(static_ip);
    _gw.fromString(static_gw);
    _nm.fromString(static_nm);
    
    wifiManager.setSTAStaticIPConfig(_ip, _gw, _nm);
  }

  wifiManager.addParameter(&node_desc_param);
  wifiManager.addParameter(&custom_ip);
  wifiManager.addParameter(&custom_nm);
  wifiManager.addParameter(&custom_gw);
  wifiManager.addParameter(&custom_dns);

  //wifiManager.setTimeout(600);
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  if (!wifiManager.autoConnect()) {
    // Not connected
    Serial.println("Failed to connect to AP");
    delay(1000);
    ESP.restart();
  }

  Serial.println("Connected");
  Serial.println(WiFi.localIP());
  Serial.println(WiFi.gatewayIP());
  Serial.println(WiFi.subnetMask());


  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    Config cfg(CONFIG_FILE);
    cfg.setValueStr("desc", node_desc_param.getValue());
    cfg.setValueStr("ip", custom_ip.getValue());
    cfg.setValueStr("gw", custom_gw.getValue());
    cfg.setValueStr("nm", custom_nm.getValue());
    cfg.setValueStr("dns", custom_dns.getValue());
    cfg.writeFile();
    
    delay(100);
    ESP.restart();
    delay(1000);
 }
}

void discover_server() {
  WiFiUDP udp;
  int res;
  const IPAddress INADDR_BCAST(255,255,255,255);
  char buf[64];

  udp.begin(DISCOVER_PORT);
  res = udp.beginPacket(INADDR_BCAST, DISCOVER_PORT);
  if (res != 1) {
    Serial.println("Failed to prepare udp packet for send");
    return;
  }

  uint32_t id;
  int pktlen;
  int i;

  buf[0] = 'S';
  buf[1] = 16;
  pktlen = 2;

  id = ESP.getChipId();
  for (i = 0; i < 8; i++, id>>=4) {
    buf[pktlen++] = nibbleToChar(id);
  }
  id = ESP.getFlashChipId();
  for (i = 0; i < 8; i++, id>>=4) {
    buf[pktlen++] = nibbleToChar(id);
  }
  
  buf[pktlen++] = strlen(node_type);
  strcpy(buf+pktlen, node_type);
  pktlen += strlen(node_type);
  
  buf[pktlen++] = strlen(node_desc);
  strcpy(buf+pktlen, node_desc);
  pktlen += strlen(node_desc);
  
  udp.write(buf, pktlen);
    
  res = udp.endPacket();
  if (res != 1) {
    Serial.println("Failed to send udp discover packet");
    return;
  }

  for (i = 0; i < 5; i ++) {
    Serial.println("Packet sent");
    int wait;
    for (wait = 0, res = -1; res == -1 && wait < 250; wait++) {
      delay(1);
      res = udp.parsePacket();
    }

    if (res == -1) {
      Serial.println("Packet reply timed out, retrying");
      continue;
    }
      
    Serial.print("Pkt reply took ");
    Serial.print(wait);
    Serial.println(" msec");
  
    char reply[32];
    res = udp.read(reply, sizeof(reply));
    if (res < 9) {
      Serial.println("Not enough data in the reply");
      continue;
    }
    Serial.print("Packet:");
    for (i = 0; i < res; i++) {
      Serial.print(' ');
      Serial.print(reply[i], HEX);
    }
    Serial.print('\n');

    if (reply[0] != 'R') {
      Serial.println("Invalid reply header");
      continue;
    }

    if (reply[1] != 4) {
      Serial.println("Invalid ip length");
      continue;
    }

    sprintf(mqtt_server, "%d.%d.%d.%d", reply[2], reply[3], reply[4], reply[5]);
    print_str("MQTT IP", mqtt_server);

    if (reply[6] != 2) {
      Serial.println("Invalid port length");
      continue;
    }

    mqtt_port = reply[7] | (reply[8] << 8);
    Serial.print("MQTT Port: ");
    Serial.println(mqtt_port);
  }

  udp.stop();
}

void setup() {
  node_type[0] = 0;
  node_desc[0] = 0;

  Serial.begin(115200);
#ifdef DEBUG
  delay(10000); // Wait for port to open, debug only
#endif

  Serial.println(ESP.getResetInfo());
  Serial.println(ESP.getResetReason());

  uint32_t t1 = ESP.getCycleCount();
  spiffs_mount();
  config_load();
  net_config();
  discover_server();
  mqtt_setup();
  uint32_t t2 = ESP.getCycleCount();

  Serial.print("setup time ");
  Serial.print(t2-t1);
  Serial.print(" in micros ");
  Serial.println(clockCyclesToMicroseconds(t2-t1));

  // Load node params
  // Load wifi config
  // Button reset check
  Serial.println("Setup done");
}

void read_serial_commands() {
  if (Serial.available()) {
    char ch = Serial.read();
    if (ch == 'f') {
      Serial.println("Clearing Config");
      WiFi.disconnect(true);
      SPIFFS.remove(CONFIG_FILE);
      Serial.println("Clearing done");
      delay(100);
      ESP.restart();
    }
  }  
}

void loop() {
  read_serial_commands();
}