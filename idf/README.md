# idf/ — прошивка inkmetrics на ESP-IDF (USB-сеть + сервер в приборе)

Цель этого проекта: прибор представляется хосту обычной **сетевой картой** (USB NCM/ECM-RNDIS),
сам выдаёт адрес по DHCP и отдаёт веб-страницу. На хосте не ставится ничего: с рабочего
компьютера открывается `http://192.168.7.1/` как обычный сайт.

Это **не** замена Arduino-версии из `firmware/` — та остаётся рабочим прибором (диск со
страницей, Web Serial). Здесь идёт вторая ветка, где сервер живёт в приборе полностью.

## Статус (2026-09-16, конец дня)

| Что | Состояние |
|---|---|
| IDF v5.5.5 + тулчейны | **готово** (`C:\esp\esp-idf`, `C:\Users\user\.espressif`) |
| Сборка/прошивка/логи из git-bash | **готово** (обёртки и инструменты ниже) |
| Наш код: сеть + DHCP + HTTP | **работает** — проверено в диагностической сборке (лог `работаю: heap 364596, up 5 с`) |
| USB-сеть (TinyUSB) | **падает**: `tinyusb_driver_install` → `ESP_ERR_TIMEOUT` (`idf/main/main.c:276`), устройство на хосте не появляется |

Точное место падения получено из дампа паники (расшифровка `esp-coredump`):

```
Panic reason: abort() was called at PC 0x40379503, задача 'main'
#3 _esp_error_check_failed (rc=263 = 0x107 ESP_ERR_TIMEOUT, main.c:276,
    function="start_usb_net", expression="tinyusb_driver_install(&tusb_cfg)")
```

### СЛЕДУЮЩИЙ ШАГ (с чего начинать завтра)

1. Зажать `BOOT`, не отпуская передёрнуть USB, держать ~2 с, отпустить — плата в загрузчике.
2. Прочитать свежий дамп паники и расшифровать его:
   ```bash
   cd /d/inkmetrics
   PY=/c/Python314/python.exe
   CPY="C:/Users/user/.espressif/python_env/idf5.5_py3.14_env/Scripts/python.exe"
   GDB="C:/Users/user/.espressif/tools/xtensa-esp-elf-gdb/17.1_20260402/xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb.exe"
   $PY -m esptool --chip esp32s3 --port COM3 --before no-reset --after no-reset \
       read-flash 0x7F0000 0x10000 firmware/backup/core-new.bin
   "$CPY" -m esp_coredump info_corefile --gdb "$GDB" -t raw \
       -c firmware/backup/core-new.bin idf/build/inkmetrics_idf.elf | grep -A6 "Backtrace\|#3"
   ```
   Дамп покажет **новую** строку причины (прошлый раз так нашли `tinyusb_driver_install`).
   Расшифровывать нужно тем ELF, который **прошит** — если пересобирали, SHA не совпадёт.
3. Гипотезы, если снова `ESP_ERR_TIMEOUT` в `tinyusb_driver_install`:
   - стек задачи TinyUSB 4 КБ (`TINYUSB_DEFAULT_TASK_SIZE`) — задать свой через
     `TINYUSB_TASK_CUSTOM(size, prio, core)` в `tinyusb_config_t`;
   - сравнить с **NCM**-сборкой: она перечислялась устройством (`VID_303A&PID_4000`) —
     значит TinyUSB тогда поднимался; разница только в режиме сети и конфиге.
   - временно добавить в композит класс **CDC** (это класс TinyUSB, не консоль IDF —
     конфликта нет) и писать логи туда: получим живые логи при работающем USB.

## Адреса и раскладка

| Что | Значение |
|---|---|
| Прибор | `192.168.7.1` (HTTP: `/`, `/api/state`, `/api/boot` — уход в загрузчик) |
| Хост | адрес выдаёт DHCP самого прибора (обычно `192.168.7.2`) |
| Режим USB-сети | ECM/RNDIS (NCM Windows не приняла: `CM_PROB_FAILED_INSTALL`) |
| Разделы | `idf/partitions.csv`: app0/app1 под OTA, `msc` 1,5 МБ (диск хоста) — на том же смещении, что в Arduino-версии, поэтому файлы хоста не теряются |
| Загрузчик | `BOOT` при включении 3 с, либо `GET /api/boot` |

## Команды

```bash
cd /d/inkmetrics

# сборка обычной прошивки (USB-сеть + HTTP)
/c/Python314/python.exe C:/esp/idf_build.py -C D:/inkmetrics/idf -B D:/inkmetrics/idf/build build

# сборка диагностической (логи в USB-Serial/JTAG, USB-сеть выключена)
mkdir -p /d/inkmetrics/build-diag
/c/Python314/python.exe C:/esp/idf_build.py -C D:/inkmetrics/idf -B D:/inkmetrics/build-diag \
    -D 'SDKCONFIG=build-diag/sdkconfig' -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.diag' build

# прошивка (сам ждёт порт; после записи делает watchdog-сброс — чип уходит в приложение)
/c/Python314/python.exe D:/inkmetrics/tools/try_flash.py D:/inkmetrics/idf/build

# логи платы (ВАЖНО: DTR/RTS=true — см. грабли ниже)
/c/Python314/python.exe D:/inkmetrics/tools/read_log.py COM3 20

# сброс чипа в приложение, не трогая USB-линии
/c/Python314/python.exe -m esptool --chip esp32s3 --port COM3 --before no-reset --after watchdog-reset chip-id
```

Обёртки `C:/esp/idf_run.py` и `C:/esp/idf_build.py` нужны, потому что `idf_tools.py`/`idf.py`
отказываются работать при `MSYSTEM` в окружении (метка git-bash) и спотыкаются о SOCKS-прокси.

## Грабли, которые уже стоило времени (не наступать снова)

1. **Порт USB-Serial/JTAG: `DTR/RTS` — это кнопки.** `DTR=0/RTS=0` при открытии = «зажми BOOT
   и дёрни сброс», то есть команда «уйди в загрузчик». Логи читать только с `DTR=1/RTS=1`.
2. **RTC-бит `FORCE_DOWNLOAD_BOOT` «липкий»** — после него чип входит в загрузчик при каждом
   сбросе, пока полностью не снимешь питание. Для аварийного выхода его не использовать:
   достаточно сброситься, пока зажат BOOT (ROM сам увидит низкий уровень на GPIO0).
3. **Консоль USB-CDC несовместима с TinyUSB** (`depends on !TINY_USB` в Kconfig IDF) — для
   логов либо UART-адаптер, либо диагностическая сборка с USB-Serial/JTAG.
4. **Вторичная консоль на USB-Serial/JTAG занимает тот же USB-PHY**, что нужен TinyUSB.
   В обычной сборке держать `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` и
   `CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=n`.
5. **`esp_netif`: конфигурацию драйвера нельзя передавать в `esp_netif_new(cfg.driver)`** —
   она теряется. Как в `esp_eth`: `esp_netif_new()` без driver → `esp_netif_attach(base)` →
   `esp_netif_set_driver_config()` внутри `post_attach`.
6. **Дампы паники не работают без `espcoredump` в `REQUIRES`** — проект собирается с
   `MINIMAL_BUILD ON`, иначе компонент не попадает в сборку и символы `CONFIG_ESP_COREDUMP_*`
   «unknown kconfig symbol».
7. **`esp-coredump` требует GDB** — указывать `--gdb …/xtensa-esp32s3-elf-gdb.exe`.
8. **Загрузки тулчейнов блокируются** (github release-assets и зеркало Espressif отдают
   0 байт) — качать через VPS: `tools/idf_get_tools.py`, ставить из локального
   `C:/esp/dist/tools_local.json`.
9. **`pip` ломается из-за SOCKS-прокси** (`HTTPS_PROXY=socks5://…`, нет PySocks) — для pip
   прокси надо снимать (обёртка это делает).

## Что дальше по плану

1. Добить USB-сеть (см. следующий шаг выше) и проверить с хоста: адаптер, адрес `192.168.7.x`,
   открытие `http://192.168.7.1/`.
2. Перенести в IDF экран (компонент Waveshare `epaper_driver_bsp` из
   `02_Example/ESP-IDF/V2`), кнопки и SHTC3.
3. Отдавать с прибора ту же страницу управления (правка кнопок/страниц), что уже работает
   в Arduino-версии.
4. Wi-Fi точка доступа и OTA; диск хоста (`msc`) — перенести из Arduino-версии в IDF.
