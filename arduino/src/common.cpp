#include "common.h"
#include <Arduino.h>
#include "node_mqtt.h"
#include "rtc_store.h"
#include "cached_vars.h"
#include <ESP8266WiFi.h>

static void print_hexdump_line(const char *buf, size_t buf_len)
{
  size_t i;

  for (i = 0; i < buf_len && i < 16; i++) {
    Serial.print(buf[i], HEX);
    Serial.print(' ');
  }
  Serial.print("   ");
  for (i = 0; i < buf_len && i < 16; i++) {
    char ch = buf[i];
    if (!isprint(ch))
      ch = '.';
    Serial.print(ch);
  }
  Serial.print('\n');
}

void print_hexdump(const char *buf, size_t buf_len)
{
  size_t i;

  for (i = 0; i < buf_len; i += 16)
    print_hexdump_line(buf + i, buf_len - i);
}

char nibbleToChar(uint32_t val)
{
  val &= 0xF;
  if (val < 10)
    return '0' + val;
  else
    return 'A' + val - 10;
}

void restart(void)
{
  // We want to send any message pending
  delay(50);

  // Next we want to disconnect and let the disconnect propogate to the clients
  mqtt_disconnect();
  debug.log("Restarting"); // Also tries to flush any remaining data
  delay(25);
  debug.stop();
  WiFiClient::stopAll();
  delay(25);

  // Now we reset
  ESP.restart();

  // It was shown that after a restart we need to delay for the restart to work reliably
  delay(1000);
}

void deep_sleep(unsigned seconds)
{
  debug.log("Going to sleep for ", seconds, " seconds");
  rtc_store_save(&rtc_store);

  delay(1);
  mqtt_loop();
  delay(1);
  debug.stop();
  WiFiClient::stopAll();
  delay(1);
  ESP.deepSleep(seconds * 1000 * 1000);
}

void battery_check(float battery)
{
  if (battery > 0.5 && battery < 3.7)
    deep_sleep(15*60);
}

void config_time(void)
{
  debug.log("Reconfiguring time");
  IPAddress ip_val = cache.get_sntp_server();
  String ip = ip_val.toString();
  configTime(0, 0, ip.c_str(), NULL, NULL);
}
