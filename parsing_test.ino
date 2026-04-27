#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <ETH.h>
#include <WebServer.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <esp_task_wdt.h>
#include <esp_system.h>


// Рекомендуемые пины для ESP32 (избегаем конфликта с флеш-памятью -  Важно: НЕ использовать GPIO5!)
#define ETH_CS 17
#define ETH_RST 4

// Wifi настройки
const char* ssid = "levelmeter";
const char* password = "2222111007";
const char* serverUrl = "http://192.168.4.1";

// ========================ИНТЕРВАЛЫ==============================

// Интервал считывания данных криохранилища
const unsigned long READ_INTERVAL = 5000;
// интервал обновления дисплея
const unsigned long LCD_UPDATE_INTERVAL = 1000;
// сетевые переподключения
const unsigned long WIFI_RECONNECT_DELAY = 30000;
const unsigned long LAN_RECONNECT_DELAY = 30000;


// =================СТАТУСЫ СЕТИ==================================
static bool wifiConnected = false;
static bool lanConnected = false;
static bool eth_started = false;
static bool cable_connected = false;


// ===================СЧЕТЧИКИ====================================

// переменные для фиксации времени считывания данных и обновления дисплея
unsigned long lastReadTime = 0;
unsigned long lastLcdUpdateTime = 0;


// переменные для фиксации времени проверки/переподключения сети
unsigned long lastWifiReconnect = 0;
unsigned long lastLanReconnect = 0;

// сервер на порту 502
WebServer server(502);


// дисплей
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Строка для передачи на Web
String main_params = "0 0";

// ========== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ДЛЯ ETH v3.x ==========

SPIClass ethSPI(HSPI);  // Отдельный SPI для Ethernet W5500

// ========== ОБРАБОТЧИК СОБЫТИЙ (ОБЯЗАТЕЛЬНО ДЛЯ v3.x) ==========

// События Ethernet
void onEthEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("🔌 ETH Started");
      ETH.setHostname("wendum_esp");
      eth_started = true;
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("🔗 Cable connected");
      cable_connected = true;
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.print("🌐 IP: ");
      Serial.println(ETH.localIP());
      lanConnected = true;  // ✅ Устанавливаем флаг
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println("ETH lost IP");
      lanConnected = false;  // ✅ Сбрасываем флаг
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("🔗 Cable disconnected!!");
      cable_connected = false;
      lanConnected = false;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("🔌 ETH stopped");
      lanConnected = false;  // ✅ Сбрасываем флаг
      break;
    default: break;
  }
}

// События WiFi
void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiConnected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiConnected = false;
      break;
    default:
      break;
  }
}


// ============ Обновление дисплея и сетевых подключений =========================

void startLAN() {
  // LAN start
  pinMode(ETH_RST, OUTPUT);
  digitalWrite(ETH_RST, LOW);
  delay(50);
  digitalWrite(ETH_RST, HIGH);
  delay(200);

  // ✅ Инициализация ОТДЕЛЬНОГО SPI для Ethernet (ваши пины)
  ethSPI.begin(18, 19, 23, ETH_CS);  // SCK=18, MISO=19, MOSI=23, CS=17
  pinMode(ETH_CS, OUTPUT);
  digitalWrite(ETH_CS, HIGH);

  // Получение IP-адреса
  Serial.println("\nПолучение IP по DHCP...");
  lcd.setCursor(0, 0);
  lcd.print("Get LAN IP DHCP ");

  // ✅ Регистрация обработчика событий (ОБЯЗАТЕЛЬНО до ETH.begin)
  Network.onEvent(onEthEvent);

  // ✅ ПРАВИЛЬНЫЙ ВЫЗОВ ETH.begin() для W5500 в v3.x:
  eth_started = ETH.begin(
    ETH_PHY_W5500,  // тип PHY
    1,              // адрес PHY (не критично для W5500)
    ETH_CS,         // CS = 17
    -1,             // INT не подключён
    ETH_RST,        // RST = 4
    ethSPI,         // объект SPIClass
    20              // частота SPI в МГц
  );

  if (!eth_started) {
    lanConnected = false;
  }
}

void startWifi() {
  // Start Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
}


// регулярная проверка Wifi для loop
void renewWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    return;
  }

  wifiConnected = false;

  if (millis() - lastWifiReconnect < WIFI_RECONNECT_DELAY) return;

  lastWifiReconnect = millis();
  Serial.println("WiFi reconnect..");
  lcd.setCursor(0, 0);
  lcd.print("WiFi reconnect..");

  WiFi.reconnect();
}


// Обновление статуса LAN, переподключение
// Обновленная функция renewLAN() для v3.x
void renewLAN() {

  // Быстрый чек
  if (ETH.linkUp() && ETH.localIP() != IPAddress(0, 0, 0, 0)) {
    lanConnected = true;
    return;
  }

  // Чек не прошел, меняем флаг
  lanConnected = false;


  // Проверка таймера
  if (millis() - lastLanReconnect < LAN_RECONNECT_DELAY) return;
  lastLanReconnect = millis();

  
  // Обновляем дисплей
  Serial.println("ETH restart...");
  lcd.setCursor(0, 0);
  lcd.print("Restart LAN...  ");

  // Перезапуск LAN
  ETH.end();
  delay(100);

  pinMode(ETH_RST, OUTPUT);
  digitalWrite(ETH_RST, LOW);
  delay(50);
  digitalWrite(ETH_RST, HIGH);
  delay(100);

  eth_started = ETH.begin(ETH_PHY_W5500, 1, ETH_CS, -1, ETH_RST, ethSPI, 20);
}



// Обновление LCD дисплея для loop
void updateLcdDisplay() {
  if (millis() - lastLcdUpdateTime < LCD_UPDATE_INTERVAL) return;

  lcd.setCursor(0, 0);

  // ✅ Правильная проверка: ETH.localIP() == IPAddress(0,0,0,0)
  if (ETH.localIP() == IPAddress(0, 0, 0, 0)) {
    lcd.print("NO LAN ADDR     ");
  } else {
    lcd.print("                ");
    lcd.setCursor(0, 0);
    lcd.print(ETH.localIP());
  }

  // Отображение результирующей строки
  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  lcd.print(main_params);

  //Отображение активности WiFi (W/w) и активности LAN (L/l)
  lcd.setCursor(13, 1);
  lcd.print(lanConnected ? "L" : "l");
  lcd.setCursor(15, 1);
  lcd.print(wifiConnected ? "W" : "w");

  //Обновляем счетчик
  lastLcdUpdateTime = millis();
}

// ===============================================================================


// ==================== <<< РАБОТА С HTML страницей криохранилища >>> =============================================

// Функция удаляет все кавычки из строки: " ' и \"
String removeQuotes(String input) {
  String output = "";

  for (int i = 0; i < input.length(); i++) {
    char c = input.charAt(i);

    // Пропускаем двойные и одинарные кавычки
    if (c == '"' || c == '\'') {
      continue;
    }

    // Добавляем символ к результату
    output += c;
  }

  return output;
}

// Функция проверки: содержит ли строка хотя бы одну цифру
bool hasDigits(String str) {
  for (int i = 0; i < str.length(); i++) {
    if (isDigit(str.charAt(i))) {
      return true;
    }
  }
  return false;
}


// Функция подключения к странице, получение кода страницы
String getHtmlPage() {

  String payload = "";

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(serverUrl);  // или http.begin(wifiClient, url) для HTTPS
    int httpResponseCode = http.GET();

    if (httpResponseCode == 200) {
      payload = http.getString();  // Получаем весь HTML в одну строку
      Serial.println("Web cтраница загружена. ");

      // Убираем кавычки из текста
      payload = removeQuotes(payload);

    } else {
      Serial.print("Ошибка запроса: ");
      Serial.println(httpResponseCode);
    }
    http.end();
  } else {
    Serial.println("WiFi не подключен!");
  }


  return payload;
}


// Функция парсинга (ищем все вхождения тэга <b>)
String extractBoldTags(String html) {

  Serial.println("============ СТАРТ ПАРСЕРА ============");

  // инит результирующей строки
  String result = "";

  // Приводим к нижнему регистру
  html.toLowerCase();  // ← присваиваем результат

  int startIndex = 0;
  int endIndex = 0;
  String openTag = "<b>";
  String closeTag = "</b>";
  int count = 0;

  while (true) {
    // 1. Ищем открывающий тег <b>
    startIndex = html.indexOf(openTag, startIndex);
    if (startIndex == -1) break;  // Больше нет тегов <b>

    // Сдвигаем индекс сразу после "<b>"
    startIndex += openTag.length();

    // Пробуем найти правильный закрывающий тег </b>
    endIndex = html.indexOf(closeTag, startIndex);

    // Если </b> нет, ищем любой другой тег "<" как границу
    if (endIndex == -1) {
      endIndex = html.indexOf("<", startIndex);
      // Если даже "<" нет (конец строки), берем до конца
      if (endIndex == -1) endIndex = html.length();
    }

    // Вырезаем и выводим текст
    String content = html.substring(startIndex, endIndex);
    content.trim();  // Убираем пробелы по краям

    // Пропускаем пустые результаты
    if (content.length() > 0) {
      count++;
      Serial.print("Найдено #" + String(count) + ": ");
      Serial.println(content);
    }


    // ПРОВЕРКА: есть ли цифры в строке?
    if (hasDigits(content)) {

      result += content;  // прибавляем к результирующей строке новый контент через пробел.

      Serial.print("Найдено число: ");
      Serial.println(content);
    } else {
      Serial.print("Пропущено (нет цифр): ");
      Serial.println(content);
    }

    // Сдвигаем индекс для поиска следующего
    startIndex = endIndex;
  }

  Serial.print("Всего найдено: ");
  Serial.println(count);
  Serial.println("============ ПАРСИНГ ЗАКОНЧЕН ============");

  // очищаем html
  html = "";


  // очищаем результирующую строку от всего лишнего.
  result.replace("%", "");
  result.replace("c", "");
  result.trim();

  return result;
}


// основная функция, возвращает конечный результат
void parseCryoWeb() {

  // Быстрая проверка таймера
  if (millis() - lastReadTime < READ_INTERVAL) return;

  // Получаем страницу
  String web_page = getHtmlPage();

  // Обновляем глобальную строку с данными
  main_params = extractBoldTags(web_page);

  // Очищаем String web_page
  web_page = "";

  // Обновляем счетчик
  lastReadTime = millis();

  Serial.println(main_params);

  return;
}





// +++++++++++++++++++ SETUP +++++++++++++++++++++++++++++++++++++

void setup() {
  // Bluetooth off
  btStop();

  // COM порт
  Serial.begin(115200);

  // Инициализация таймера
  lastReadTime = millis();

  // Задаем конфигурацию watchdog
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = 60000,   // 60 second timeout
    .idle_core_mask = 0,   // Monitor specific cores (0 for none)
    .trigger_panic = true  // System resets on timeout
  };

  esp_task_wdt_reconfigure(&twdt_config);  // Apply config
  esp_task_wdt_add(NULL);                  // Subscribe current task (loop)
  esp_task_wdt_reset();

  // Инициализация дисплея
  Wire.begin(21, 22);     // SDA, SCL (стандарт для ESP32)
  Wire.setClock(400000);  // ускоряем шину ДО инициализации 400 кГц (Fast Mode). PCF8574 тянет стабильно

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Init LCD");

  lcd.setCursor(0, 1);
  lcd.print(main_params);

  delay(500);
  lcd.clear();


  // Старт LAN
  startLAN();


  // Старт сервера
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain", main_params);
  });
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });
  server.begin();

  Serial.print("Server started on port 502 at ");
  lcd.setCursor(0, 0);
  lcd.print("Server started");
  delay(500);
  lcd.clear();
  Serial.println(ETH.localIP());

  // Start Wi-Fi
  startWifi();
}


// ===================ОСНОВНОЙ ЦИКЛ ПРОГРАММЫ======================================================
void loop() {

  // Поддерживаем LAN
  renewLAN();

  // Поддерживаем WiFi
  renewWifi();

  // Обновляем данные
  parseCryoWeb();

  // обновление LCD дисплея
  updateLcdDisplay();

  // обработка запросов клиента
  if (lanConnected) {
    server.handleClient();
  }

  // Feed watchdog
  esp_task_wdt_reset();
}
