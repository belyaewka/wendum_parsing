#include <WiFi.h>
#include <HTTPClient.h>
#include <ModbusIP_ESP8266.h>
#include <SPI.h>
#include <Ethernet.h>


// Wifi настройки
const char* ssid = "levelmeter";
const char* password = "2222111007";
const char* serverUrl = "http://192.168.4.1";

// Адреса регистров (смещение от 40001)cd 
const uint16_t REG_LEVEL = 0;  // 40001
const uint16_t REG_TEMP = 1;   // 40002

// Интервал считывания данных криохранилища
const unsigned long READ_INTERVAL = 5000;

unsigned long lastReadTime = 0;

// Список ТОЛЬКО для чисел
// std::vector<float> numericalValues;
std::vector<int16_t> numericalValues;  // Целочисленные значения в int16_t для удобства записи в Modbus регистры
ModbusIP mb;                           // Экземпляр сервера Modbus

// Значения для регистров Modbus
int16_t levelInt = 0;
int16_t tempInt = 0;



void setup() {
  // COM порт
  Serial.begin(115200);

  // Инициализация таймера
  lastReadTime = millis();

  // Start Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  // Modbus TCP server
  mb.server();

  // Добавляем регистры
  mb.addHreg(REG_LEVEL);
  mb.addHreg(REG_TEMP);


  // Stabilization
  delay(10);
}



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


// Конвертация строки в float (с обработкой запятых)
float stringToFloat(String str) {
  str.trim();
  str.replace(',', '.');  // Заменяем запятую на точку

  String clean = "";
  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isDigit(c) || c == '-' || c == '.') {
      if (c == '-' && clean.length() > 0) continue;       // Минус только в начале
      if (c == '.' && clean.indexOf('.') >= 0) continue;  // Только одна точка
      clean += c;
    }
  }

  if (clean.length() == 0) return 0.0;
  return clean.toFloat();
}


// Конвертация строки в целые числа int (вернее int16_t) c округлением до десятых
int16_t stringToInt16(String str) {
  str.trim();
  str.replace(',', '.');  // Запятая → точка

  // Извлекаем только цифры, точку и минус
  String clean = "";
  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isDigit(c) || c == '-' || c == '.') {
      // Минус только в начале
      if (c == '-' && clean.length() > 0) continue;
      // Только одна точка
      if (c == '.' && clean.indexOf('.') >= 0) continue;
      clean += c;
    }
  }

  if (clean.length() == 0) return 0;

  // Конвертируем в float, умножаем на 10, округляем, приводим к int16
  float val = clean.toFloat();
  val = val * 10.0;
  val = round(val);  // Округление до целого

  // Проверка на выход за диапазон int16
  if (val > 32767) val = 32767;
  if (val < -32768) val = -32768;

  return (int16_t)val;
}



// Функция подключения к странице, получение кода страницы
void parseHtmlPage() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;

    Serial.println("Загрузка веб страницы криохранилища...");
    http.begin(serverUrl);
    http.addHeader("Connection", "close");
    http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");  // Притворяемся браузером
    http.setConnectTimeout(5000);                                               // 5 секунд на подключение
    http.setTimeout(5000);                                                      // 5 секунд на чтение данных

    int httpResponseCode = http.GET();

    if (httpResponseCode == 200) {
      String payload = http.getString();  // Получаем весь HTML в одну строку
      Serial.println(payload);

      Serial.println("Страница загружена. Начинаем парсинг:");
      Serial.println("------------------------------");

      // Убираем кавычки из текста
      payload = removeQuotes(payload);
      Serial.println("CТРОКА ОЧИЩЕННАЯ ОТ КАВЫЧЕК:       ========================");
      Serial.println(payload);

      // Вызываем функцию поиска тегов <b>
      extractBoldTags(payload);

      // Убрать payload из памяти
      payload = "";

      Serial.println("------------------------------");
    } else {
      Serial.print("Ошибка запроса: ");
      Serial.println(httpResponseCode);
    }
    http.end();
  } else {
    Serial.println("WiFi не подключен!");
  }
}


// Функция парсинга (ищем все вхождения тэга <b>)
void extractBoldTags(String html) {

  Serial.println("============ СТАРТ ПАРСЕРА ============");

  // Приводим к нижнему регистру
  html.toLowerCase();


  int startIndex = 0;
  int endIndex = 0;
  String openTag = "<b>";
  // String closeTag = "</b>";
  int count = 0;

  while (true) {
    // 1. Ищем открывающий тег <b>
    startIndex = html.indexOf(openTag, startIndex);
    if (startIndex == -1) break;  // Больше нет тегов <b>

    // Сдвигаем индекс сразу после "<b>"
    startIndex += openTag.length();

    // Пробуем найти правильный закрывающий тег </b>
    endIndex = html.indexOf("</b>", startIndex);

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
      int16_t value = stringToInt16(content);
      numericalValues.push_back(value);

      Serial.print("Найдено число: ");
      Serial.println(value);
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

  // Очистка html
  html = "";
}


// диагностика памяти ESP32
void mem_diag() {
  Serial.println("=== Статистика памяти ===");
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());
  Serial.print("Min free heap: ");
  Serial.println(ESP.getMinFreeHeap());  // Покажет, насколько низко падала память
  Serial.println("=======================");
}


void loop() {
  // Основной цикл программы

  // счетчик времени на millis
  unsigned long now = millis();

  mb.task();  // Обработка Modbus-запросов

  if (now - lastReadTime >= READ_INTERVAL) {
    numericalValues.clear();
    parseHtmlPage();

    // после парсинга обновляем регистры
    if (numericalValues.size() >= 2) {
      levelInt = numericalValues[0];  // Готовое int16, к примеру 1000 (100%)
      tempInt = numericalValues[1];   // Готовое int16, к примеру -1957 (-195.7 deg C)

      // Принудительная запись значения переменной в регистры
      mb.Hreg(REG_LEVEL, levelInt);
      mb.Hreg(REG_TEMP, tempInt);

      Serial.print("Level: ");
      Serial.println(levelInt);
      Serial.print("Temp: ");
      Serial.println(tempInt);

      mem_diag();
    }

    // // Доступ по индексу
    // if (numericalValues.size() >= 2) {
    //   int16_t level = numericalValues[0];  // 1000
    //   int16_t temp = numericalValues[1];   // -1957

    //   Serial.print("Уровень: ");
    //   Serial.println(level);

    //   Serial.print("Температура: ");
    //   Serial.println(temp);
    // }

    lastReadTime = now;
  }
  delay(10);
}
