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
| USB-сеть (TinyUSB + RNDIS) | **поднялась**: Windows перечисляет прибор как `Remote NDIS based Internet Sharing Device`, `USB\VID_303A&PID_4020`. Причина прошлых отказов — описатели, см. ниже |
| Адрес по DHCP у хоста | **нет**: адаптер `Up`, DHCP включён, IPv4 не получен вообще (даже APIPA) |
| Перезагрузки прибора | **есть**: чип перезагружается каждые ~15–25 с (на хосте RNDIS-адаптер появляется и пропадает). Причина не найдена |
| Состояние платы на конец дня 2026-09-16 | **с шины пропала**: все `VID_303A` — `Present: False`, COM-портов нет (только COM1 хоста) |
| Наш код: сеть + DHCP + HTTP | поднимается (проверено в диагностической сборке, лог `работаю: heap 364596, up 5 с`) |
| «Чёрный ящик» логов | **работает**: логи, метки стадий и регистры читаются `tools/idf_diag.py` (см. `firmware/backup/diag-20260916-rndis.log`) |
| Сборка со счётчиками кадров и событиями USB | **собрана, на железо не заливалась** (`idf/build`) |

**Что именно починило USB-сеть.** `esp_tinyusb` 2.3.0 подставляет описатели по умолчанию
только для классов CDC, MSC, MTP и NCM (`descriptors_control.c`). Для ECM/RNDIS
конфигурационный описатель обязан задать пользователь — иначе задача стека умирает с
`Full-speed configuration descriptor must be provided for this device`, а наружу это видно
**только как `ESP_ERR_TIMEOUT`** из `tinyusb_driver_install` (никаких сообщений: консоли у
платы нет). Решение — `idf/main/usb_desc.c`: свой описатель устройства (VID `0x303A`,
PID `0x4020`) и одна конфигурация **RNDIS** (`TUD_CONFIG_DESCRIPTOR` +
`TUD_RNDIS_DESCRIPTOR`, как в примере TinyUSB `net_lwip_webserver`) плюс строки. RNDIS, а не
ECM — потому что Windows понимает RNDIS своим драйвером; две конфигурации (RNDIS + ECM, как
в примере TinyUSB) через `esp_tinyusb` не выставить: он поддерживает ровно одну.

Заодно: PHY поднимаем сами (`usb_phy_setup()`) и оставляем себе, `esp_tinyusb` — с
`.phy.skip_setup = true`. Создание и удаление PHY по кругу роняло плату в `interrupt-wdt`
и обрывало лог ровно на `usb_del_phy()`. Регистры при этом показали, что PHY и часы в
порядке: до PHY `GSNPSID=0x00000000` (часы выключены — так и должно быть), после
`usb_new_phy` — `GSNPSID=0x4f54400a` (валидный ID ядра DWC2), `USB_WRAP.otg_conf=0x001c0000`.

### С ЧЕГО НАЧАТЬ ЗАВТРА (2026-09-17)

Прибор в конце дня пропал с USB. Порядок:

1. **Вернуть прибор на связь.** Полностью снять питание (отключить USB на ~10 с), затем
   зажать `BOOT` и, не отпуская, подключить USB, подержать ~2 с. Проверить:
   ```bash
   /c/Python314/python.exe -m serial.tools.list_ports -v
   ```
   Ждём `VID:PID=303A:1001` (порт ROM, COM3). Если порта нет — попробовать другой USB-кабель
   и/или другой порт (в конце дня признаки похожи на «устройства нет физически»: все
   `VID_303A` в диспетчере `Present: False`), затем проверить диспетчер устройств на
   «Неизвестное USB-устройство».
2. **Снять «чёрный ящик»** (плата уже в загрузчике, порт присутствует):
   ```bash
   /c/Python314/python.exe D:/inkmetrics/tools/idf_diag.py
   ```
   В заголовке смотрим **счётчик загрузок** и **причину сброса** (`4` — паника,
   `5` — interrupt-wdt, `6` — task-wdt, `1` — power-on): это ответ на вопрос, почему
   прибор перезагружается каждые ~20 с. Если причина `4` — снять дамп паники из раздела
   `coredump` (0x7F0000) и расшифровать **тем ELF, который прошит** (`idf/build/inkmetrics_idf.elf`):
   ```bash
   /c/Python314/python.exe -m esptool --chip esp32s3 --port COM3 --before no-reset --after no-reset \
       read-flash 0x7F0000 0x10000 firmware/backup/core-next.bin
   "C:/Users/user/.espressif/python_env/idf5.5_py3.14_env/Scripts/python.exe" -m esp_coredump info_corefile \
       --gdb "C:/Users/user/.espressif/tools/xtensa-esp-elf-gdb/17.1_20260402/xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb.exe" \
       -t raw -c firmware/backup/core-next.bin idf/build/inkmetrics_idf.elf | head -40
   ```
3. **Залить готовую сборку** со счётчиками кадров, событиями USB (`прицепился/отцепился`)
   и строкой раз в 10 с (`ip`, `link`, кадры приёма/отдачи):
   ```bash
   /c/Python314/python.exe D:/inkmetrics/tools/try_flash.py D:/inkmetrics/idf/build
   ```
   Затем `BOOT` + передёрнуть USB и снять лог: счётчики покажут, доходят ли DHCP-запросы
   хоста до прибора (если `приём 0` — вопрос в приёме кадров, если растут, а адреса нет —
   в DHCP-сервере).
4. **Проверить хост-сторону** (после того как прибор снова появится как сетевая карта):
   ```bash
   powershell -NoProfile -ExecutionPolicy Bypass -File tools/host_net_check.ps1
   ```
5. **Если ИДФ-ветка упёрлась** — вернуть рабочий прибор (Arduino-версия):
   ```bash
   bash tools/build.sh && bash tools/flash.sh          # нужен COM3 в загрузчике
   # либо залить сохранённый дамп:
   /c/Python314/python.exe -m esptool --chip esp32s3 --port COM3 write-flash 0x0 firmware/backup/flash-20260916-131245.bin
   ```



## Адреса и раскладка

| Что | Значение |
|---|---|
| Прибор | `192.168.7.1` (HTTP: `/`, `/api/state`, `/api/boot` — уход в загрузчик) |
| Хост | адрес выдаёт DHCP самого прибора (обычно `192.168.7.2`) |
| Режим USB-сети | ECM/RNDIS (NCM Windows не приняла: `CM_PROB_FAILED_INSTALL`) |
| Разделы | `idf/partitions.csv`: app0/app1 под OTA, `msc` 1,44 МБ (диск хоста) — на том же смещении, что в Arduino-версии, поэтому файлы хоста не теряются; `diag` 64 КБ — «чёрный ящик» логов (пишется по адресу 0x7E0000, занято 8 КБ) |
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

# «чёрный ящик» IDF-прошивки: логи из раздела diag (плата в загрузчике!)
/c/Python314/python.exe D:/inkmetrics/tools/idf_diag.py
/c/Python314/python.exe D:/inkmetrics/tools/idf_diag.py --file tools/diag-raw.bin   # разобрать снятый дамп

# весь цикл одной командой: залить → дать отработать → снять «чёрный ящик»
# (сам говорит, когда зажать BOOT и передёрнуть USB)
/c/Python314/python.exe D:/inkmetrics/tools/idf_cycle.py
/c/Python314/python.exe D:/inkmetrics/tools/idf_cycle.py --build C:/Users/user/AppData/Local/Temp/eink-ncm/build

# состояние USB-сети на хосте: RNDIS-адаптер, адрес, DHCP, маршруты, страница прибора
powershell -NoProfile -ExecutionPolicy Bypass -File D:/inkmetrics/tools/host_net_check.ps1

# проверка читалки «чёрного ящика» без платы (синтетический дамп)
/c/Python314/python.exe D:/inkmetrics/tools/diag_test_make.py
/c/Python314/python.exe D:/inkmetrics/tools/idf_diag.py --file D:/inkmetrics/tools/diag-test.bin

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
10. **В IDF-прошивке у платы нет ни одного канала логов**: UART наружу не выведен, USB-CDC
   несовместим с TinyUSB, вторичная консоль на USB-Serial/JTAG занимает тот же PHY — и
   именно тогда, когда USB и падает, смотреть некуда. Решение — «чёрный ящик»: хук
   `esp_log_set_vprintf` копит логи в ОЗУ (8 КБ) и пишет их в раздел `diag` по ERROR и
   явным вызовам `diag_flush()`; читается с хоста `tools/idf_diag.py` в режиме загрузчика.
   Дамп паники (`coredump`) этого не заменяет: в нём нет строк ESP_LOGE и кодов `esp_err`.
11. **`ESP_ERROR_CHECK` в IDF-прошивке = перезагрузка по кругу**: плата падает быстрее, чем
   её успеваешь перевести в загрузчик, и «чёрный ящик» не снять. Ошибку инициализации
   разбирать без паники: лог → `diag_flush()` → `return`, жить дальше.
12. **`esp_tinyusb` 2.3.0 не даёт описателей для ECM/RNDIS** — они есть только для CDC, MSC,
   MTP и NCM. Для сетевого режима конфигурационный описатель задаётся в приложении
   (`idf/main/usb_desc.c`), иначе задача стека умирает с «Full-speed configuration descriptor
   must be provided for this device», а снаружи это выглядит **только** как `ESP_ERR_TIMEOUT`
   из `tinyusb_driver_install`. Не искать причину в PHY по этому коду ошибки (потерян полдня).
13. **Создание и удаление PHY по кругу роняет плату** (`usb_new_phy()` → `usb_del_phy()`, а
   затем `esp_tinyusb` поднимает PHY заново): лог обрывается ровно на `usb_del_phy()`,
   причина сброса — `interrupt-wdt`. PHY поднимать самим и оставлять себе, а `esp_tinyusb`
   передавать `.phy.skip_setup = true`.
14. **Многосекторное стирание флеша валит прошивку**: `esp_flash_erase_region` держит кэш и
   прерывания выключенными на весь диапазон и не укладывается в interrupt-watchdog (300 мс
   по умолчанию) — плата уходит в цикл перезагрузок, а лог не появляется. Стирать по одному
   сектору с паузой 10 мс и держать `CONFIG_ESP_INT_WDT_TIMEOUT_MS` с запасом (у нас 3000).
15. **Контракт «чёрного ящика» с читалкой**: хвост каждой записи добивать **пробелами**, а не
   `0xFF`, потому что `0xFF` — признак нестёртой флеша, по нему читалка определяет конец лога.
   С добивкой `0xFF` лог выглядел пустым (66 байт) три сессии подряд.
16. **Порядок аргументов esptool**: опции `write-flash` (`--flash-mode` и т. п.) ставить
   **после** подкоманды, а `flash_args` из сборки IDF читать с заменой подчёркиваний на
   дефисы (`--flash_mode` → `--flash-mode`) — иначе «No such option».
17. **Скрипты для Windows-хоста — в ASCII**: PowerShell 5.1 читает `.ps1` в системной
   кодировке, русский текст ломает разбор (`TerminatorExpectedAtEndOfString`).

## Что лежит вне папки проекта (нужно для сборки)

| Что | Где | Как восстановить |
|---|---|---|
| ESP-IDF v5.5.5 | `C:\esp\esp-idf` | `git clone --depth 1 -b v5.5.5 --recurse-submodules --shallow-submodules https://github.com/espressif/esp-idf.git` |
| Обёртки запуска | `C:\esp\idf_run.py`, `C:\esp\idf_build.py` | 30 строк: снять `MSYSTEM` и SOCKS-прокси, добавить PATH тулчейнов, запустить `idf_tools.py`/`idf.py` (см. `tools/` в проекте — там же лежат копии логики загрузки тулчейнов) |
| Тулчейны + питоновское окружение IDF | `C:\Users\user\.espressif` (~4 ГБ) | `python tools/idf_get_tools.py` (качает через VPS) → `idf_tools.py --tools-json C:/esp/dist/tools_local.json install --targets=esp32s3` → `install-python-env` |
| Архивы тулчейнов | `C:\esp\dist` (1,2 ГБ) + `tools_local.json` | тот же скрипт |
| Ключ к VPS (канал загрузки) | `D:\NewHerm\proxy_key`, хост `84.21.191.180` | — |
| Примеры Waveshare (для экрана) | склонированы в `%TEMP%\eink\ws` | `git clone` репозитория Waveshare ESP32-S3-ePaper-1.54 |

## Что дальше по плану

1. Добить USB-сеть (см. следующий шаг выше) и проверить с хоста: адаптер, адрес `192.168.7.x`,
   открытие `http://192.168.7.1/`.
2. Перенести в IDF экран (компонент Waveshare `epaper_driver_bsp` из
   `02_Example/ESP-IDF/V2`), кнопки и SHTC3.
3. Отдавать с прибора ту же страницу управления (правка кнопок/страниц), что уже работает
   в Arduino-версии.
4. Wi-Fi точка доступа и OTA; диск хоста (`msc`) — перенести из Arduino-версии в IDF.
