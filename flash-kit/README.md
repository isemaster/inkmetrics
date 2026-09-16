# Заливка прошивки inkmetrics с другого компьютера

Папку `flash-kit` скопируй целиком (флешка, сеть) — больше ничего не нужно, кроме Python
с `esptool` (см. «Если Python ставить не хочется» — там варианты без него).

| Папка | Что внутри | Когда заливать |
|---|---|---|
| `arduino/` | рабочая прошивка прибора (Arduino-версия): отдаёт диск `E:` со страницей `INKMETRICS.HTM` + COM-порт приложения | **чтобы вернуть прибор в работу** |
| `idf/` | экспериментальная прошивка на ESP-IDF (USB-сеть, прибор как сетевая карта) | только для отладки USB-сети: она сейчас перезагружается каждые ~20 с, адрес по DHCP хост не получает |

## Почему прибор «то определяется, то отсоединяется» раз в секунду

Это **не** режим загрузчика. Так выглядит приложение: чип перезапускается в цикле, и Windows
успевает увидеть USB-устройство и снова потерять его. В режиме загрузчика (ROM) порт
появляется один раз и **не пропадает** — именно в нём и можно шить.

Две типичные причины такого цикла, различаются просто:
- **Питание/кабель/порт** — хаб, слабый порт или плохой кабель: плата проседает по питанию и
  сбрасывается. Признак: мигает даже порт загрузчика (с зажатым BOOT), либо цикл ~1 раз в секунду.
  Лечение: подключить напрямую к порту ПК (без хаба), другой кабель, другой порт (лучше USB 2.0).
- **Прошивка** — приложение само сбрасывается. Признак: порт загрузчика стоит ровно, а вот
  устройство приложения мигает. Лечение: залить `arduino/` (рабочая версия).

## Шаг 1. Войти в режим загрузчика

1. Отключи прибор от USB.
2. Зажми кнопку **BOOT** и, не отпуская её, подключи USB.
3. Подержи ~2 секунды и отпусти BOOT.
4. Проверь, что порт появился и стабилен:

   ```powershell
   Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like '*303A*' } |
       Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize
   ```

   Нужно увидеть `USB JTAG/serial debug unit` (VID `303A`, PID `1001`) и/или
   «Последовательный интерфейс USB (COMx)». Обычно это `COM5` и выше.

5. Запомни номер порта (`COMx`). Список портов с описанием: `python -m serial.tools.list_ports -v`

Если порт не появился или мигает — снова BOOT + подключение USB; убери хаб; возьми другой
кабель; если на плате есть кнопка RESET — зажми BOOT и нажми RESET.

## Шаг 2. Поставить esptool (один раз)

Нужен Python 3 с python.org, затем:

```
pip install esptool
```

## Шаг 3. Залить прошивку

В папке нужного варианта:

```
flash-merged.bat COM5      <- проще всего: один файл, один адрес (0x0)
flash.bat COM5             <- то же самое, но четырьмя файлами с явными адресами
```

(подставь свой порт; скрипт сам найдёт интерпретатор с esptool — `python` или `py -3`).

Вручную, если скрипты не годятся.

**Arduino-версия** — одним файлом (из папки `arduino`):

```
python -m esptool --chip esp32s3 --port COM5 --baud 921600 write-flash -z ^
  0x0 inkmetrics-arduino-0x0.bin
```

**Arduino-версия** — четырьмя файлами (то же содержимое, явные адреса):

```
python -m esptool --chip esp32s3 --port COM5 --baud 921600 write-flash -z ^
  0x0     inkmetrics.ino.bootloader.bin ^
  0x8000  inkmetrics.ino.partitions.bin ^
  0xe000  boot_app0.bin ^
  0x10000 inkmetrics.ino.bin
```

**IDF-версия** — одним файлом (из папки `idf`):

```
python -m esptool --chip esp32s3 --port COM5 --baud 921600 write-flash -z ^
  0x0 inkmetrics-idf-0x0.bin
```

**IDF-версия** — четырьмя файлами:

```
python -m esptool --chip esp32s3 --port COM5 --baud 921600 write-flash -z ^
  0x0     bootloader.bin ^
  0x8000  partition-table.bin ^
  0xe000  ota_data_initial.bin ^
  0x20000 inkmetrics_idf.bin
```

Адреса важны: у IDF-версии приложение лежит на `0x20000` (две копии под OTA), у Arduino —
на `0x10000`. Поэтому объединённые образы удобнее: там адреса уже проставлены, ошибиться
нельзя. Объединённый образ пишется строго с `0x0`.

## Если Python ставить не хочется

- **Flash Download Tool** (Espressif, Windows, без Python): вкладка `ESP32-S3`, режим `DIO`,
  80 МГц, четыре файла с адресами из команды выше, кнопка START.
- **Веб-прошивальщик** `esptool-js` в Chrome: <https://espressif.github.io/esptool-js/> —
  в нём нужно указать адрес и файл для каждой из четырёх позиций, порт тот же (ROM).

Оба варианта требуют того же: прибор в режиме загрузчика (BOOT + подключение USB).

## Шаг 4. Проверка

esptool сам сбросит чип после записи.

- После **Arduino-версии**: появится съёмный диск `E:` с `INKMETRICS.HTM` и COM-порт приложения;
  страница открывается в Chrome/Edge и работает через Web Serial.
- После **IDF-версии**: Windows покажет сетевую карту `Remote NDIS based Internet Sharing
  Device` (`VID_303A&PID_4020`); адрес по DHCP сейчас не выдаётся — это незакрытый вопрос,
  см. `idf/README.md` в проекте.

Если после заливки Arduino-версии прибор всё равно мигает — дело не в прошивке, а в
питании/кабеле/порте (либо плата повреждена).

## Как обновить комплект после новой сборки

Образы (`*.bin`) в git не хранятся — их кладут заново из сборок проекта:

```bash
cd /d/inkmetrics
# Arduino-версия: bash tools/build.sh   (собирает firmware/build/*.bin)
cp firmware/build/inkmetrics.ino.bootloader.bin firmware/build/inkmetrics.ino.partitions.bin \
   firmware/build/inkmetrics.ino.bin flash-kit/arduino/
cp "$LOCALAPPDATA/Arduino15/packages/esp32/hardware/esp32/2.0.9/tools/partitions/boot_app0.bin" flash-kit/arduino/
# IDF-версия: python C:/esp/idf_build.py -C D:/inkmetrics/idf -B D:/inkmetrics/idf/build build
cp idf/build/bootloader/bootloader.bin idf/build/partition_table/partition-table.bin \
   idf/build/ota_data_initial.bin idf/build/inkmetrics_idf.bin flash-kit/idf/
# объединённые образы (пишутся с 0x0)
cd flash-kit/arduino && /c/Python314/python.exe -m esptool --chip esp32s3 merge-bin \
   -o inkmetrics-arduino-0x0.bin 0x0 inkmetrics.ino.bootloader.bin 0x8000 inkmetrics.ino.partitions.bin \
   0xe000 boot_app0.bin 0x10000 inkmetrics.ino.bin
cd ../idf && /c/Python314/python.exe -m esptool --chip esp32s3 merge-bin \
   -o inkmetrics-idf-0x0.bin 0x0 bootloader.bin 0x8000 partition-table.bin \
   0xe000 ota_data_initial.bin 0x20000 inkmetrics_idf.bin
```
