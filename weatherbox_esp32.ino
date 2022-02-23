#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>

#include "favicon.h"
#include "weatherbox_common/weather.h"
#include "weatherbox_common/common.h"

static Weather w;
static SemaphoreHandle_t wmutex;

// WiFi stuff
static const char *ssid = "wireless fidelity";
static const char *password = "trial by fire";  // I don't care who knows this.
static const char *hostname = "weatherbox";
static const int refresh_rate = 1800;  // In seconds
WiFiServer server(80);

// Tasks
TaskHandle_t task_handle_wifi_client;


int day_of_week(int year, int month, int day) { // 1 <= m <= 12, y > 1752 (in the U.K.)
  static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 3) {
    year -= 1;
  }
  return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

void client_send_style(WiFiClient& client) {
  // Feel free to change the background-color and font-size attributes to fit your preferences
  client.println("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
  client.println("<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: center;}");
  client.println("text-decoration: none; font-size: 30px; margin: 2px; cursor: pointer;}");
  client.println("</style>");
}

// Send a 400 response to a client
void client_send_400(WiFiClient& client) {
  client.println("HTTP/1.1 400 Bad Request");
  client.println("Content-Type: text/html");
  client.println("Cache-Control: no-cache");
  client.println("Connection: close");
  client.println();

  // Display the HTML web page
  client.println("<!DOCTYPE html><html>");

  client_send_style(client);

  // Web Page Heading
  client.println("<body><h1>400</h1>");
  client.println("<p>Bad request. How naughty~.</p>");
  client.println("</body></html>");

  // The client response ends with another blank line
  client.println();
}

// Send a 404 response to a client
void client_send_404(WiFiClient& client) {
  client.println("HTTP/1.1 404 Not Found");
  client.println("Content-Type: text/html");
  client.println("Cache-Control: no-cache");
  client.println("Connection: close");
  client.println();

  // Display the HTML web page
  client.println("<!DOCTYPE html><html>");

  client_send_style(client);

  // Web Page Heading
  client.println("<body><h1>404</h1>");
  client.println("<p>Not found</p>");
  client.println("</body></html>");

  // The client response ends with another blank line
  client.println();
}

// Send a webpage with the current weather
void client_send_weather_page(WiFiClient& client) {
  // Send back response headers; tell the client to refresh every so often
  client.println("HTTP/1.1 200 OK");
  client.println("Content-type: text/html");
  client.println("Cache-Control: no-cache");
  client.print("Refresh: "); client.println(refresh_rate);
  client.println("Connection: close");
  client.println();

  // Display the HTML web page
  client.println("<!DOCTYPE html><html>");
  client.println("<head>");

  client_send_style(client);

  // Web Page Heading
  client.println("<body><h1>Weather data</h1>");

  // We're working with the weather structure, take the mutex
  xSemaphoreTake(wmutex, portMAX_DELAY);

  client.print("<p>Time: ");
  struct tm t = {
    .tm_sec = w.get_second(),
    .tm_min = w.get_minute(),
    .tm_hour = w.get_hour(),
    .tm_mday = w.get_day(),
    .tm_mon = w.get_month() - 1,
    .tm_year = w.get_year() - 1900,
    .tm_wday = day_of_week(w.get_year(), w.get_month(), w.get_day())
  };
  client.print(asctime(&t));                  client.println("</p>");
  client.print("<p>Temp: ");                  client.print(w.get_temperature());       client.println("&deg;C</p>");
  client.print("<p>Humidity: ");              client.print(w.get_humidity());          client.println("%</p>");
  client.print("<p>Wind: ");                  client.print(w.get_wind());              client.print("KPH, direction ");
  client.print(w.get_wind_direction());       client.println("&deg;</p>");
  client.print("<p>UV index: ");              client.print(w.get_uv());                client.println("</p>");
  client.print("<p>AQI: ");                   client.print(w.get_aqi());               client.println("</p>");
  client.print("<p>CO2 equivalent: ");        client.print(w.get_co2e());              client.println("</p>");
  client.print("<p>VOC breath equivalent: "); client.print(w.get_co2e());              client.println("</p>");
  client.print("<p>Rainfall per hour: ");     client.print(w.get_rainfall_per_hour()); client.println("</p>");

  // Give up the mutex
  xSemaphoreGive(wmutex);

  client.println("</body></html>");

  // The HTTP response ends with another blank line
  client.println();
}

// Send JSON data
void client_send_api(WiFiClient& client) {
  // We're working with the weather structure, take the mutex
  xSemaphoreTake(wmutex, portMAX_DELAY);

  String data;
  w.to_json(data);

  // Give up the mutex
  xSemaphoreGive(wmutex);

  // Send back response headers; tell the client to refresh every so often
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Cache-Control: no-cache");
  client.print("Refresh: "); client.println(refresh_rate);
  client.println("Connection: close");
  client.println();
  client.print(data);
  client.println();
}

void client_send_favicon(WiFiClient& client) {
  Serial.print("Favicon size is: "); Serial.println(favicon_size);

  // Send back response headers
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: image/png");
  client.print("Content-Length: "); client.println(favicon_size);
  client.println("Cache-Control: public,max-age=31536000,immutable");
  client.println("Connection: close");
  client.println();

  // Send the icon
  client.write(favicon, favicon_size);
  client.println("\n");
}

// Route the client's GET request
void route_client(WiFiClient& client, String& path) {
  if (path == "/api")
    client_send_api(client);
  else if (path == "/")
    client_send_weather_page(client);
  else if (path == "/favicon.ico")
    client_send_favicon(client);
  else
    client_send_404(client);
}

// Handle incoming clients
void task_wifi_client(void* pv_params) {
  Serial.print("Task wifi client running on core ");
  Serial.println(xPortGetCoreID());

  // Current client header
  String header = "";

  // Current time
  unsigned long current_time = millis();
  // Previous time
  unsigned long previous_time = 0;
  // Define timeout time in milliseconds (example: 2000ms = 2s)
  const long timeout_time = 2000;

  while (true) {
    WiFiClient client = server.available();  // Listen for incoming clients
    if (client) {
      current_time = millis();
      previous_time = current_time;

      Serial.print("New Client: ");
      Serial.println(client.remoteIP());

      String current_line = "";
      while (client.connected()
             && current_time - previous_time <= timeout_time) {
        current_time = millis();
        if (client.available()) {
          char c = client.read();
          header += c;
          if (c == '\n') {
            // if the current line is blank, you got two newline characters in a row.
            // that's the end of the client HTTP request, so send a response
            if (current_line.length() == 0) {
              // We only care about GET requests
              int request_type_pos = header.indexOf("GET");
              if (request_type_pos < 0) {
                // Bad request
                client_send_400(client);
              }
              else {
                // We have a valid request, advance to the position of the path
                request_type_pos += 4;
                // Now at the page path.
                // Everything to the next space is the path
                int path_end_pos = header.indexOf(" ", request_type_pos);
                if (path_end_pos < 0) {
                  client_send_400(client);
                  break;
                }

                // Route to the correct path
                String path = header.substring(request_type_pos, path_end_pos);
                route_client(client, path);
              }
              // Break out of the while loop
              break;
            }
            else {
              current_line = "";
            }
          }
          else if (c != '\r') {  // if you got anything else but a carriage return character,
            current_line += c;   // add it to the end of the current_line
          }
        }
      }
      // Clear the header variable
      header = "";
      // Close the connection
      client.stop();
      Serial.println("Client disconnected.");
      Serial.println("");
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

void i2c_receive(int len) {
  if (len < w.packet_size) {
    Serial.printf("Error with packet size; expected %d, got %d", w.packet_size, len);
    return;
  }

  byte buf[w.packet_size];
  size_t i = 0;
  while (Wire.available()) {
    buf[i++] = Wire.read();
  }

  Serial.println("Got data!");
  w.from_data(buf);
  if (w.get_year() < 2022) {
    struct tm t;
    if (!getLocalTime(&t)) {
      Serial.println("Failed to get time!");
      return;
    }

    w.set_year(t.tm_year + 1900);
    w.set_month(t.tm_mon + 1);
    w.set_day(t.tm_mday);
    w.set_hour(t.tm_hour);
    w.set_minute(t.tm_min);
    w.set_second(t.tm_sec);
  }
}

void wifi_disconnected(WiFiEvent_t event, arduino_event_info_t info) {
  WiFi.reconnect();
}

void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(80);

  Wire.begin(I2C_ADDRESS_ESP32);
  Wire.onReceive(i2c_receive);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi...");

  int counter = 0;
  while (WiFi.waitForConnectResult() != WL_CONNECTED && counter++ < 60) {
    Serial.print(".");
    delay(1000);
  }

  if (counter == 60) {
    Serial.println("Failed to connect, rebooting!");
    ESP.restart();
  }

  Serial.println(" Connected!");
  Serial.print("RSSI: ");
  Serial.println(WiFi.RSSI());
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  WiFi.onEvent(wifi_disconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  configTime(0, 0, "0.pool.ntp.org", "1.pool.ntp.org", "2.pool.ntp.org");
  struct tm t;
  if (!getLocalTime(&t)) {
    Serial.println("Failed to get time!");
  }
  else {
    Serial.println(&t, "%A %d %B %Y %H:%M:%S");
  }

  if (!MDNS.begin(hostname)) {
    Serial.println("Error starting mDNS!");
    ESP.restart();
  }

  server.begin();

  // Set up FreeRTOS tasks and mutexes
  wmutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(task_wifi_client, "task_wifi_client", 10000, NULL, 1, &task_handle_wifi_client, 0);
}

void loop() {
  vTaskDelay(60000 / portTICK_PERIOD_MS);
  Serial.println("Ping!");
}
