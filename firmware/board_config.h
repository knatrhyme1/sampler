// Проверка, что прошивка собрана под целевую плату ESP32-S3 N16R8:
// 16 МБ флеша в режиме QIO и 8 МБ octal-PSRAM (docs/known-issues.md, п. 5).
//
// Опции платы закреплены в sketch.yaml (default_fqbn). Голый FQBN
// esp32:esp32:esp32s3 берёт настройки меню по умолчанию — PSRAM выключена,
// библиотеки IDF под quad-PSRAM, — и такая сборка тихо запускалась бы без
// памяти под банк сэмплов. Поэтому она здесь не компилируется вовсе.
//
// Размер флеша на этапе компиляции не виден: он записан в заголовок образа
// при прошивке. Его, как и фактический объём PSRAM, проверяет
// logBoardMemory() при старте.
//
// Исключение — веб-редактор Wokwi. Он собирает скетч на своём сервере с
// голым FQBN, sketch.yaml не читает, и включить PSRAM там нельзя
// (wokwi/wokwi-features#809). Веб-проект несёт файл wokwi_web_build.h (копия
// лежит в firmware/wokwi_web/, в локальную сборку он не попадает): с ним
// сборка без PSRAM разрешена, а logBoardMemory() пишет в лог предупреждение.
// Прошивку с PSRAM в Wokwi проверяют локальной сборкой (wokwi.toml).

#pragma once

#include <Arduino.h>

#if __has_include("wokwi_web_build.h")
#define SAMPLER_WOKWI_WEB_BUILD 1
#elif !defined(BOARD_HAS_PSRAM) || !defined(CONFIG_SPIRAM_MODE_OCT) || \
    !defined(CONFIG_ESPTOOLPY_FLASHMODE_QIO)
#error "Сборка не под ESP32-S3 N16R8: нужны PSRAM=opi и FlashMode=qio. Собирайте без --fqbn (опции берутся из firmware/sketch.yaml) или с полным FQBN оттуда."
#endif

const uint32_t kBoardFlashBytes = 16u * 1024 * 1024;
const uint32_t kBoardPsramBytes = 8u * 1024 * 1024;

// Печатает в лог флеш и PSRAM, которые видит прошивка, и предупреждает, если
// они меньше заявленных для платы. PSRAM 8 МБ — это 8 388 608 байт на кристалле;
// ESP.getPsramSize() возвращает чуть меньше: часть забирает сам IDF.
inline bool logBoardMemory(Print &out) {
  const uint32_t flash = ESP.getFlashChipSize();
  const uint32_t psram = psramFound() ? ESP.getPsramSize() : 0;
  const uint32_t psramFree = psramFound() ? ESP.getFreePsram() : 0;
  out.printf("board flash=%uKB psram=%uKB psram_free=%uKB\n", (unsigned)(flash / 1024),
             (unsigned)(psram / 1024), (unsigned)(psramFree / 1024));
  const bool ok = flash >= kBoardFlashBytes && psram >= kBoardPsramBytes * 9 / 10;
  if (!ok) {
    out.println("board WARNING: память меньше, чем у N16R8 (16 МБ флеш / 8 МБ PSRAM)");
#ifdef SAMPLER_WOKWI_WEB_BUILD
    out.println("board WARNING: сборка веб-редактора Wokwi, PSRAM в ней нет");
#endif
  }
  return ok;
}
