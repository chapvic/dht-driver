# Драйвер датчиков температуры и влажности DHT11/DHT22/AM2302

**Версия:** 2.8.5  
**Автор:** (c) 2026, Chapvic  
**Лицензия:** GNU General Public License v3

Модуль ядра Linux для чтения данных о температуре и влажности с датчиков DHT11, DHT22 и AM2302, подключённых к контактам GPIO Raspberry Pi. Драйвер создаёт интерфейс procfs в `/proc/sensors/dht/` для управления регистрацией датчиков, настройкой и получением данных.

---

## Оглавление

1. [Обзор](#обзор)
2. [Архитектура протокола DHT](#архитектура-протокола-dht)
3. [Поддерживаемое оборудование](#поддерживаемое-оборудование)
4. [Подключение](#подключение)
5. [Быстрый старт](#быстрый-старт)
6. [Сборка и установка](#сборка-и-установка)
7. [Установка через DKMS](#установка-через-dkms)
8. [Совместимость с ядрами](#совместимость-с-ядрами)
9. [Загрузка модуля](#загрузка-модуля)
10. [Файл конфигурации](#файл-конфигурации)
11. [Интерфейс procfs](#интерфейс-procfs)
12. [Регистрация датчиков](#регистрация-датчиков)
13. [Чтение данных](#чтение-данных)
14. [Автоматический опрос](#автоматический-опрос)
15. [Ручное измерение](#ручное-измерение)
16. [Определение типа датчика](#определение-типа-датчика)
17. [Сервис systemd](#сервис-systemd)
18. [Примеры на Bash](#примеры-на-bash)
19. [Примеры на Python](#примеры-на-python)
20. [Коды ошибок](#коды-ошибок)
21. [Параметры конфигурации](#параметры-конфигурации)
22. [Режим отладки](#режим-отладки)
23. [Архитектура](#архитектура)
24. [История версий](#история-версий)
25. [Известные ограничения](#известные-ограничения)
26. [Устранение неполадок](#устранение-неполадок)
27. [Файлы в этом репозитории](#файлы-в-этом-репозитории)
28. [Лицензия](#лицензия)

---

## Обзор

Драйвер DHT предоставляет интерфейс на основе procfs для чтения температуры и влажности с датчиков DHT11, DHT22 и AM2302, подключённых к контактам GPIO Raspberry Pi.

Ключевые возможности:

- Динамическая регистрация датчиков через procfs (`export`/`unexport`)
- Индивидуальные записи procfs для температуры, влажности, статуса и конфигурации
- Фоновый поток опроса с настраиваемым интервалом (индивидуальным или глобальным)
- Ручной запуск измерения через procfs
- Наносекундная точность измерения импульсов для надёжного чтения на всех моделях Pi
- Кэширование базы GPIO-чипа для быстрой регистрации нескольких датчиков на Pi 3/4/5
- Ограничение частоты измерений (ручных и автоматических)
- Безопасная выгрузка модуля со счётчиком ссылок (`kref`)
- Разделяемый каталог `/proc/sensors`: сосуществует с другими драйверами датчиков
- Файл конфигурации: опциональный `/etc/default/dht` для авторегистрации при загрузке
- Совместимость с ядрами: 5.0 — 6.18+ (с макросами совместимости во время компиляции)
- Поддержка DKMS: автоматическая пересборка при обновлении ядра

---

## Архитектура протокола DHT

Датчики DHT11/DHT22/AM2302 используют проприетарный однопроводной двунаправленный протокол. Микроконтроллер (хост) инициирует обмен, и датчик отвечает 40 битами данных (5 байт).

### Последовательность обмена

1. **Старт-сигнал хоста**: МК опускает линию данных в низ на минимум 18 мс (драйвер использует 20 мс)
2. **Освобождение хоста**: МК переключает линию на вход; подтягивающий резистор поднимает линию в высокое состояние
3. **Ответ датчика** (20–40 мкс после освобождения):
   - Низкий импульс ~80 мкс
   - Высокий импульс ~80 мкс
4. **Передача данных** (40 бит):
   - Каждый бит: низкий импульс ~50 мкс, затем высокий импульс, длительность которого кодирует значение
   - **0**: высокий импульс ~26 мкс
   - **1**: высокий импульс ~70 мкс
5. **Конец кадра**: линия возвращается в состояние логической единицы

### Кодирование битов

| Значение бита | Низкий импульс | Высокий импульс |
|---------------|----------------|-----------------|
| 0             | ~50 мкс        | ~26 мкс         |
| 1             | ~50 мкс        | ~70 мкс         |

Драйвер использует порог 40 мкс (`BIT_THRESHOLD = 40000 нс`) для различения 0 и 1.

### 40-битный кадр данных

| Байт | Поле                | Формат DHT11         | Формат DHT22         |
|------|---------------------|----------------------|----------------------|
| 0    | Влажность, целая    | 0–100 (целое)        | Старший байт RH      |
| 1    | Влажность, дробная | 0 (всегда ноль)      | Младший байт RH      |
| 2    | Температура, целая  | 0–50 (целое)         | Старший байт T (бит 7 = знак) |
| 3    | Температура, дробная| 0 (всегда ноль)      | Младший байт T       |
| 4    | Контрольная сумма   | (байт0+байт1+байт2+байт3) & 0xFF | То же |

**DHT11** передаёт только целые значения (дробные байты всегда 0). Значения масштабируются x10 в драйвере для единообразия.

**DHT22/AM2302** передаёт 16-битные значения, масштабированные x10: влажность как `(байт0 << 8) | байт1`, температура как `((байт2 & 0x7F) << 8) | байт3`. Бит 7 байта 2 — знаковый бит для отрицательных температур.

### Временные параметры

| Параметр                | Значение | Описание                                     |
|-------------------------|----------|----------------------------------------------|
| Старт-сигнал (низкий)    | 20 мс    | Хост опускает линию                           |
| Задержка ответа датчика | 20–40 мкс| После освобождения линии хостом             |
| Низкий импульс бита     | ~50 мкс  | Фиксированный низкий перед каждым битом       |
| Высокий импульс (0)     | ~26 мкс  | Ниже `BIT_THRESHOLD` (40 мкс)                |
| Высокий импульс (1)     | ~70 мкс  | Выше `BIT_THRESHOLD` (40 мкс)                |
| Тайм-аут импульса       | 200 мкс  | `PULSE_TIMEOUT_NS` — отмена при превышении    |
| Общая длительность кадра| ~4 мс    | 40 бит + рукопожатие                         |
| Минимум между чтениями  | 2 с      | `MEAS_MIN_GAP` — время восстановления датчика |

Драйвер отключает вытеснение (`preempt_disable`/`preempt_enable`) во время bit-bang чтения, чтобы предотвратить искажение тайминга из-за переключений контекста. Прерывания остаются включёнными, чтобы избежать влияния на задержки системы.

---

## Поддерживаемое оборудование

### Датчики

| Датчик  | Диапазон температуры | Диапазон влажности | Точность (T) | Точность (H) | Тип      |
|---------|----------------------|--------------------|--------------|--------------|----------|
| DHT11   | 0–50 °C              | 20–90% RH          | ±2 °C        | ±5% RH       | Целый    |
| DHT22   | -40…80 °C           | 0–100% RH          | ±0,5 °C      | ±2% RH       | Дробный  |
| AM2302  | -40…80 °C           | 0–100% RH          | ±0,5 °C      | ±2% RH       | Дробный  |

AM2302 — выносная версия DHT22 с тем же протоколом.

### Модели Raspberry Pi

| Модель    | Метка GPIO-чипа       | База GPIO | Макс. BCM | Примечания                      |
|-----------|----------------------|-----------|-----------|---------------------------------|
| Pi 5      | `pinctrl-rp1`        | 512+      | 0–27      | RP1 south bridge, большой сдвиг |
| Pi 4      | `pinctrl-bcm2711`    | 0         | 0–27      | BCM = глобальный GPIO            |
| Pi 3/Zero | `pinctrl-bcm2835`    | 0         | 0–27      | BCM = глобальный GPIO            |
| Pi 1/2    | `pinctrl-bcm2835`    | 0         | 0–27      | BCM = глобальный GPIO            |

### Требования к ядру

- **Минимальная версия ядра**: 5.0
- **Требуемые опции конфигурации**: `CONFIG_GPIOLIB`, `CONFIG_PROC_FS`, `CONFIG_MODULES`
- **Протестировано на**: 5.0 — 6.18+ (Raspberry Pi OS, Ubuntu, Debian)

---

## Подключение

### Разъём GPIO Raspberry Pi

Подключите вывод данных датчика к любому доступному контакту GPIO (нумерация BCM). Драйвер поддерживает контакты BCM 0–27.

```
Датчик DHT       Raspberry Pi
---------        ------------
VCC (пин 1)  →   3.3V (пин 1 или 17)
DATA         →   GPIO на выбор (например, GPIO4 = пин 7)
GND (пин 4)  →   GND (пин 6 или 9)
```

### Подтягивающий резистор

Большинство модулей DHT имеют встроенный подтягивающий резистор (4.7k–10k). При использовании «голого» датчика:

- Подключите резистор 4.7кОм–10кОм между DATA и VCC (3.3В)
- Без подтяжки показания будут нестабильными или чтение не удастся

### Выбор контакта

- Поддерживаются любые BCM GPIO 0–27
- Избегайте контактов со специальными функциями (GPIO14/15 = UART, GPIO3 = I2C SDA), если эти интерфейсы используются
- GPIO4 — распространённый выбор для DHT на Raspberry Pi

---

## Быстрый старт

```bash
# 1. Склонируйте или скопируйте файлы драйвера в каталог
cd dht-driver

# 2. Соберите
make

# 3. Загрузите модуль
sudo insmod dht.ko

# 4. Зарегистрируйте датчик на GPIO4
echo 4 | sudo tee /proc/sensors/dht/export

# 5. Считайте температуру и влажность
cat /proc/sensors/dht/gpio4/value

# 6. Включите автоопрос каждые 5 секунд
echo 5 | sudo tee /proc/sensors/dht/gpio4/interval

# 7. Или включите глобальный автоопрос для всех датчиков
echo 10 | sudo tee /proc/sensors/dht/auto_interval

# 8. Проверьте версию драйвера
cat /proc/sensors/dht/version

# 9. Выгрузите по окончании
sudo rmmod dht
```

---

## Сборка и установка

### Зависимости

```bash
# Raspberry Pi OS / Debian / Ubuntu
sudo apt install build-essential linux-headers-$(uname -r)
```

### Нативная сборка

```bash
make            # проверка + сборка dht.ko
make check      # только проверка
make modules    # сборка без проверки
make clean      # удалить артефакты сборки
```

### Установка (нативная)

```bash
make install    # сборка, установка в /lib/modules/$(uname -r)/, depmod
sudo modprobe dht
```

Модуль устанавливается в `/lib/modules/$(uname -r)/updates/` с помощью Kbuild. Это стандартное расположение для модулей вне дерева исходников, имеющее приоритет над `/kernel/` при поиске `modprobe`.

### Удаление

```bash
make uninstall   # ищет dht.ko во всех каталогах модулей и удаляет
sudo modprobe -r dht
```

Цель `uninstall` использует `find` для поиска по всем подкаталогам `/lib/modules/$(uname -r)/` (включая `updates/`, `kernel/`, `extra/`), корректно удаляя модуль независимо от места установки.

### Кросс-компиляция

```bash
# Пример: сборка на x86 для Raspberry Pi 5 (arm64)
make ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu- \
     KDIR=/path/to/rpi-kernel/build

# Установка в примонтированную целевую ФС
make ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu- \
     KDIR=/path/to/rpi-kernel/build \
     INSTALL_MOD_PATH=/mnt/rpi-root \
     install
```

### Цели Make

| Цель           | Описание                                              |
|----------------|-------------------------------------------------------|
| `make`         | Проверка, затем сборка модуля                         |
| `make check`   | Только проверка (заголовки, версия, конфигурация)    |
| `make modules` | Сборка `dht.ko` без проверки                          |
| `make clean`   | Удаление артефактов сборки                             |
| `make install` | Сборка, установка в `/lib/modules/...`, запуск `depmod`|
| `make uninstall`| Поиск и удаление `dht.ko` из всех каталогов           |
| `make help`    | Показать доступные цели и переменные                  |

---

## Установка через DKMS

DKMS (Dynamic Kernel Module Support) автоматически пересобирает модуль при обновлении ядра. Это избавляет от ручной пересборки после каждого `apt upgrade`.

### Зависимости

```bash
sudo apt install dkms
```

### Установка

```bash
# 1. Скопируйте файлы драйвера в дерево исходников DKMS
sudo mkdir -p /usr/src/dht-2.8.5
sudo cp dht.c Makefile dkms.conf /usr/src/dht-2.8.5/

# 2. Зарегистрируйте модуль в DKMS
sudo dkms add dht/2.8.5

# 3. Соберите и установите
sudo dkms install dht/2.8.5

# 4. Загрузите модуль
sudo modprobe dht
```

### Проверка

```bash
# Проверьте статус DKMS
sudo dkms status
# Ожидаемый вывод: dht/2.8.5: installed

# Проверьте, что модуль загружен
lsmod | grep dht
cat /proc/sensors/dht/version
```

### Автоматическая пересборка

С `AUTOINSTALL="yes"` в `dkms.conf` модуль автоматически пересобирается при установке нового ядра. Ручное вмешательство не требуется.

### Обновление до новой версии драйвера

```bash
# 1. Удалите старую версию
sudo dkms remove dht/2.8.5 --all

# 2. Скопируйте новые файлы
sudo cp dht.c Makefile dkms.conf /usr/src/dht-2.8.5/

# 3. Переустановите
sudo dkms install dht/2.8.5
```

### Полное удаление

```bash
sudo dkms remove dht/2.8.5 --all
sudo rm -rf /usr/src/dht-2.8.5
```

### dkms.conf

```ini
PACKAGE_NAME="dht"
PACKAGE_VERSION="2.8.5"
BUILT_MODULE_NAME[0]="dht"
DEST_MODULE_LOCATION[0]="/updates"
AUTOINSTALL="yes"
MAKE[0]="make -C ${kernel_source_dir} M=${dkms_tree}/${PACKAGE_NAME}/${PACKAGE_VERSION}/build modules"
CLEAN="make -C ${kernel_source_dir} M=${dkms_tree}/${PACKAGE_NAME}/${PACKAGE_VERSION}/build clean"
```

---

## Совместимость с ядрами

Драйвер поддерживает ядра Linux от 5.0 до 6.18+. Различия в API между версиями ядра обрабатываются во время компиляции с помощью препроцессорных макросов — скрипт `configure` не нужен.

### Макросы совместимости

#### proc_ops / file_operations (ядро 5.6)

Начиная с ядра 5.6, `proc_create()` принимает `const struct proc_ops *` вместо `const struct file_operations *`. Имена полей также изменились: `.read` -> `.proc_read` и т. д.

```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
  #define DHT_PROC_OPS     struct proc_ops
  #define DHT_PROC_READ    .proc_read
  #define DHT_PROC_WRITE   .proc_write
  #define DHT_PROC_OPEN    .proc_open
  #define DHT_PROC_RELEASE .proc_release
#else
  #define DHT_PROC_OPS     struct file_operations
  #define DHT_PROC_READ    .read
  #define DHT_PROC_WRITE   .write
  #define DHT_PROC_OPEN    .open
  #define DHT_PROC_RELEASE .release
#endif
```

Все структуры `proc_ops`/`file_operations` в драйвере используют `DHT_PROC_OPS` вместо прямого имени типа.

#### pde_data / PDE_DATA (ядро 5.17)

Начиная с ядра 5.17, `PDE_DATA()` переименована в `pde_data()`.

```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
  #define DHT_PDE_DATA(inode)  pde_data(inode)
#else
  #define DHT_PDE_DATA(inode)  PDE_DATA(inode)
#endif
```

### Таблица стабильности API

| API                           | Назначение           | Стабильно с | Примечания                    |
|-------------------------------|---------------------|-------------|-------------------------------|
| `proc_ops` / `file_operations`| Операции procfs      | 5.6 / 5.0   | Макрос `DHT_PROC_OPS`        |
| `pde_data()` / `PDE_DATA()`   | Данные датчика       | 5.17 / 5.0  | Макрос `DHT_PDE_DATA`         |
| `gpio_to_desc()`              | Поиск BCM-пина      | 3.x         | Стабильно, не удалено         |
| `gpiod_to_chip()`             | Определение GPIO-чипа| 3.x        | Стабильный API                |
| `gpiod_direction_output/input`| Протокол DHT        | 3.x         | Стабильно                     |
| `gpiod_get_value()`           | Bit-bang чтение     | 3.x         | Стабильно                     |
| `gpiod_cansleep()`            | Проверка сна        | 3.x         | Стабильно                     |
| `ktime_get_ns()`              | Измерение импульсов | 4.x         | Стабильно                     |
| `ktime_get_real_seconds()`    | Метка времени       | 4.x         | Стабильно                     |
| `iterate_dir()`               | Проверка пустоты procfs| 3.11     | Стабильно                     |
| `kref` / `kref_put`           | Подсчёт ссылок      | 2.6.x       | Стабильно                     |
| `atomic_cmpxchg`              | Флаг измерения      | 2.6.x       | Стабильно                     |
| `preempt_disable/enable`      | Защита bit-bang     | 2.6.x       | Стабильно                     |
| `filp_open` / `kernel_read`    | Чтение файла конфига| 2.6.x      | Стабильно                     |

### Предварительные проверки

Цель `check` в Makefile проверяет перед компиляцией:

- Каталог сборки ядра существует и содержит корректное дерево Kbuild
- Версия ядра >= 5.0
- `CONFIG_GPIOLIB=y` в `.config` ядра
- `CONFIG_PROC_FS=y` в `.config` ядра
- `CONFIG_MODULES=y` в `.config` ядра
- Предупреждает, если задан `ARCH=` без `CROSS_COMPILE=` (или наоборот)

---

## Загрузка модуля

### insmod (напрямую)

```bash
sudo insmod dht.ko
sudo insmod dht.ko dht_debug=1
```

### modprobe (после установки)

```bash
sudo modprobe dht
sudo modprobe dht dht_debug=1
```

### Проверка в dmesg

```bash
dmesg | grep DHT
# Ожидается:
# [DHT]: DHT11/DHT22/AM2302 Temperature and Humidity Sensor Driver (v2.8.5)
```

---

## Файл конфигурации

Драйвер читает `/etc/default/dht` при загрузке. Файл опционален — при отсутствии драйвер загружается с настройками по умолчанию.

### Формат

Строки, начинающиеся с `#`, и пустые строки игнорируются. Неизвестные опции и некорректные значения вызывают предупреждения в dmesg.

### Глобальные опции

```
# /etc/default/dht

# Включить отладку при загрузке
DEBUG=1

# Глобальный интервал автоопроса в секундах (2-60)
# Запускает опрос для всех зарегистрированных датчиков
AUTO_INTERVAL=10
```

### Регистрация датчиков

```
# Регистрация датчика на BCM-пине (без автоопроса)
SENSOR=4

# Регистрация с индивидуальным интервалом автоопроса
SENSOR=17,5

# Регистрация с отключённым автоопросом
SENSOR=22
```

### Полный пример

```
# /etc/default/dht
DEBUG=1
AUTO_INTERVAL=10
SENSOR=4             # DHT22 на GPIO4, использует глобальный интервал
SENSOR=17,5          # DHT11 на GPIO17, опрос каждые 5 секунд
SENSOR=22            # Датчик на GPIO22, без автоопроса
```

### Порядок обработки

1. Сначала обрабатываются глобальные опции (`DEBUG`, `AUTO_INTERVAL`)
2. Затем регистрируются датчики (`SENSOR=...`)
3. Если `AUTO_INTERVAL` активен, зарегистрированные датчики автоматически начинают опрос

---

## Интерфейс procfs

### Глобальные записи

```
/proc/sensors/dht/
  debug          (rw) - отладка: 0 = выкл (по умолчанию), 1 = вкл
  version        (r)  - версия драйвера
  export         (w)  - запись номера BCM-пина для регистрации датчика
  unexport       (w)  - запись номера BCM-пина для удаления датчика
  auto_interval  (rw) - глобальный интервал автоопроса (2-60, -1 = выкл)
```

### Записи датчика

```
/proc/sensors/dht/gpio<pin>/
  pin           (r)  - номер BCM GPIO
  interval      (rw) - интервал автоопроса в секундах (2-60, -1 = выкл)
  measure       (w)  - запись "1" для запуска измерения
                      (игнорируется, если interval != -1 или активен глобальный автоопрос)
  status_code   (r)  - код ошибки (0 = успех)
  status_text   (r)  - описание ошибки
  value         (r)  - "H=<влажность>\nT=<температура>\n"
  info          (r)  - тип датчика + время регистрации
  timestamp     (r)  - Unix-метка времени последнего измерения
```

### Форматы файлов

| Файл          | Права | Формат                                    | Описание                          |
|---------------|-------|-------------------------------------------|-----------------------------------|
| `pin`         | r     | `<pin>`                                   | Номер BCM GPIO                    |
| `interval`    | rw    | `<n>` (2-60) или `-1`                     | Интервал автоопроса датчика       |
| `measure`     | w     | запись `1`                                | Запуск ручного измерения          |
| `status_code` | r     | `0`–`5`                                   | Код ошибки (0 = успех)            |
| `status_text` | r     | `SUCCESS` / текст ошибки                  | Человекочитаемый статус           |
| `value`       | r     | `H=<влажность>\nT=<температура>\n`       | Последнее чтение (масштаб x10)    |
| `info`        | r     | `Sensor type: DHT11\nRegister time: <ISO>`| Тип датчика + время регистрации   |
| `timestamp`   | r     | Unix timestamp                            | Время последнего успешного измерения|

### Детали форматов

**`value`**: `H=<h>.<d>\nT=<t>.<d>\n`, значения масштабированы x10 (например, `H=45.2` = 45.2% RH). Отрицательные температуры: `T=-23.1`.

**`info`**: `Sensor type: DHT11\nRegister time: 2026-01-15T14:30:00Z\n` или `Sensor type: DHT22\n...`. Пустой вывод (EOF), если тип датчика ещё не определён (нет успешного измерения).

**`timestamp`**: Unix-метка (секунды с эпохи) последнего **успешного** измерения. `0`, если успешных измерений не было.

**`status_code`**: `0` = успех, `1` = неверный пин, `2` = ошибка GPIO, `3` = ошибка чтения, `4` = активен авто-режим, `5` = слишком рано.

---

## Регистрация датчиков

### Регистрация датчика

```bash
echo 4 | sudo tee /proc/sensors/dht/export
```

Это:
1. Проверяет номер пина (0–27)
2. Проверяет дубликат регистрации
3. Находит GPIO-дескриптор (с кэшированием базы чипа)
4. Запрашивает линию GPIO
5. Создаёт записи procfs в `/proc/sensors/dht/gpio4/`
6. Выполняет начальное измерение

### Удаление датчика

```bash
echo 4 | sudo tee /proc/sensors/dht/unexport
```

Это:
1. Находит датчик по номеру пина
2. Останавливает поток опроса (если запущен)
3. Удаляет записи procfs
4. Освобождает линию GPIO
5. Освобождает структуру датчика (после закрытия всех открытых файлов)

### Несколько датчиков

```bash
echo 4  | sudo tee /proc/sensors/dht/export
echo 17 | sudo tee /proc/sensors/dht/export
echo 22 | sudo tee /proc/sensors/dht/export
cat /proc/sensors/dht/gpio4/value
cat /proc/sensors/dht/gpio17/value
cat /proc/sensors/dht/gpio22/value
```

Максимум: 32 датчика (`MAX_SENSORS`).

---

## Чтение данных

### Кэшированные значения

Записи `value`, `status_code`, `status_text` и `timestamp` возвращают кэшированные данные последнего измерения. Они не запускают новое чтение. Драйвер выполняет начальное измерение при регистрации.

### Чтение в shell

```bash
cat /proc/sensors/dht/gpio4/value
# Вывод: H=45.2
#         T=23.1

cat /proc/sensors/dht/gpio4/status_code
# Вывод: 0

cat /proc/sensors/dht/gpio4/status_text
# Вывод: SUCCESS

cat /proc/sensors/dht/gpio4/timestamp
# Вывод: 1736897400
```

### Разбор значений

Значения в `value` масштабированы x10. Например, `H=45.2` означает 45.2% RH, `T=23.1` — 23.1 °C. Отрицательные температуры отображаются как `T=-5.3`.

---

## Автоматический опрос

### Индивидуальный интервал датчика

```bash
# Опрос каждые 5 секунд
echo 5 | sudo tee /proc/sensors/dht/gpio4/interval

# Отключить автоопрос для этого датчика
echo -1 | sudo tee /proc/sensors/dht/gpio4/interval
```

### Глобальный интервал

```bash
# Опрос ВСЕХ зарегистрированных датчиков каждые 10 секунд
echo 10 | sudo tee /proc/sensors/dht/auto_interval

# Отключить глобальный опрос (индивидуальные интервалы вступают в силу)
echo -1 | sudo tee /proc/sensors/dht/auto_interval
```

### Как это работает

- Каждый датчик с активным интервалом получает собственный поток ядра (`dht_poll_<pin>`)
- Глобальный интервал имеет приоритет над индивидуальным
- Поток спит с шагом в 1 секунду для быстрого завершения
- Ограничение частоты (минимум 2 с) применяется ко всем измерениям
- Если и глобальный, и индивидуальный интервалы равны -1, поток простаивает до повторного включения или остановки

---

## Ручное измерение

### Запуск измерения

```bash
echo 1 | sudo tee /proc/sensors/dht/gpio4/measure
```

### Ограничения

- Ручное измерение недоступно, если активен автоопрос (индивидуальный или глобальный)
- Минимум 2 секунды между измерениями (ограничение частоты)
- Если занято другое измерение, возвращается ошибка

### Проверка результата

```bash
echo 1 | sudo tee /proc/sensors/dht/gpio4/measure
cat /proc/sensors/dht/gpio4/status_code   # 0 = успех
cat /proc/sensors/dht/gpio4/value        # H=45.2 T=23.1
```

---

## Определение типа датчика

Драйвер автоматически определяет тип датчика (DHT11 или DHT22) по результатам первого успешного измерения:

- **DHT11**: дробные байты (байты 1 и 3) всегда равны 0
- **DHT22**: дробные байты содержат ненулевые значения или температура имеет знак минус

Тип отображается в записи `info`:

```bash
cat /proc/sensors/dht/gpio4/info
# Вывод: Sensor type: DHT22
#        Register time: 2026-01-15T14:30:00Z
```

Если измерение ещё не выполнено, тип `UNKNOWN` и `info` возвращает пустой вывод.

---

## Сервис systemd

### Способ 1: Простая загрузка модуля

Создайте `/etc/modules-load.d/dht.conf`:

```
dht
```

Создайте `/etc/modprobe.d/dht.conf` (опционально):

```
options dht dht_debug=0
```

### Способ 2: Модуль с параметрами через systemd

Создайте `/etc/modprobe.d/dht.conf`:

```
options dht dht_debug=0
```

Создайте `/etc/systemd/system/dht-driver.service`:

```ini
[Unit]
Description=DHT sensor driver
After=systemd-modules-load.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/sbin/modprobe dht
ExecStop=/sbin/rmmod dht

[Install]
WantedBy=multi-user.target
```

### Способ 3: Полный сервис с файлом конфигурации

Создайте `/etc/systemd/system/dht-sensors.service`:

```ini
[Unit]
Description=DHT Temperature and Humidity Sensor Driver
After=systemd-modules-load.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/sbin/modprobe dht
ExecStart=/bin/sh -c 'for pin in 4 17 22; do echo $$pin > /proc/sensors/dht/export; done'
ExecStart=/bin/sh -c 'echo 10 > /proc/sensors/dht/auto_interval'
ExecStop=/bin/sh -c 'for pin in 4 17 22; do echo $$pin > /proc/sensors/dht/unexport; done'
ExecStop=/sbin/rmmod dht

[Install]
WantedBy=multi-user.target
```

Включите и запустите:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now dht-sensors.service
```

---

## Примеры на Bash

### 1. Базовое чтение

```bash
#!/bin/bash
PIN=4
data=$(cat /proc/sensors/dht/gpio$PIN/value)
echo "GPIO$PIN: $data"
```

### 2. Непрерывный мониторинг

```bash
#!/bin/bash
PIN=4
while true; do
    data=$(cat /proc/sensors/dht/gpio$PIN/value)
    echo "$(date '+%Y-%m-%d %H:%M:%S') - $data"
    sleep 10
done
```

### 3. Логирование в CSV

```bash
#!/bin/bash
PIN=4
INTERVAL=10
LOGFILE=/tmp/dht_log.csv
echo "timestamp,humidity,temperature" > "$LOGFILE"
while true; do
    data=$(cat /proc/sensors/dht/gpio$PIN/value)
    ts=$(cat /proc/sensors/dht/gpio$PIN/timestamp)
    h=$(echo "$data" | grep '^H=' | cut -d= -f2)
    t=$(echo "$data" | grep '^T=' | cut -d= -f2)
    echo "$ts,$h,$t" >> "$LOGFILE"
    sleep "$INTERVAL"
done
```

### 4. Чтение всех датчиков

```bash
#!/bin/bash
for dir in /proc/sensors/dht/gpio*/; do
    [ -d "$dir" ] || continue
    pin=$(cat "${dir}pin")
    data=$(cat "${dir}value")
    status=$(cat "${dir}status_code")
    echo "GPIO$pin (status=$status): $data"
done
```

### 5. Мониторинг порога

```bash
#!/bin/bash
PIN=4
THRESHOLD=30.0
while true; do
    data=$(cat /proc/sensors/dht/gpio$PIN/value)
    temp=$(echo "$data" | grep '^T=' | cut -d= -f2)
    if (( $(echo "$temp > $THRESHOLD" | bc -l) )); then
        echo "ALERT: Temperature $temp C exceeds $THRESHOLD C"
    fi
    sleep 10
done
```

### 6. Регистрация из файла конфигурации

```bash
#!/bin/bash
# Регистрация датчиков из формата /etc/default/dht
CONFIG=/etc/default/dht
if [ ! -f "$CONFIG" ]; then
    echo "Файл конфигурации не найден: $CONFIG"
    exit 1
fi
while IFS= read -r line; do
    case "$line" in
        \#*|"") continue ;;
        SENSOR=*)
            spec="${line#SENSOR=}"
            pin="${spec%%,*}"
            echo "Регистрация датчика на GPIO$pin..."
            echo "$pin" | sudo tee /proc/sensors/dht/export > /dev/null
            ;;
    esac
done < "$CONFIG"
```

---

## Примеры на Python

### 1. Базовое чтение

```python
#!/usr/bin/env python3
PIN = 4
with open(f"/proc/sensors/dht/gpio{PIN}/value") as f:
    data = f.read().strip()
values = dict(line.split("=") for line in data.splitlines())
print(f"GPIO{PIN}: H={values['H']}%  T={values['T']}C")
```

### 2. Непрерывный мониторинг с CSV

```python
#!/usr/bin/env python3
import time, csv, sys
PIN = 4
INTERVAL = 10
with open(f"/proc/sensors/dht/gpio{PIN}/value") as devnull:
    pass
writer = csv.writer(sys.stdout)
writer.writerow(["timestamp", "humidity", "temperature", "status"])
try:
    while True:
        with open(f"/proc/sensors/dht/gpio{PIN}/value") as f:
            data = dict(l.split("=") for l in f.read().strip().splitlines())
        with open(f"/proc/sensors/dht/gpio{PIN}/status_code") as f:
            status = f.read().strip()
        with open(f"/proc/sensors/dht/gpio{PIN}/timestamp") as f:
            ts = f.read().strip()
        writer.writerow([ts, data["H"], data["T"], status])
        sys.stdout.flush()
        time.sleep(INTERVAL)
except KeyboardInterrupt:
    pass
```

### 3. Ручное измерение

```python
#!/usr/bin/env python3
import time, subprocess
PIN = 4
subprocess.run(f"echo 1 > /proc/sensors/dht/gpio{PIN}/measure", shell=True)
time.sleep(1)
with open(f"/proc/sensors/dht/gpio{PIN}/value") as f:
    print(f.read())
with open(f"/proc/sensors/dht/gpio{PIN}/status_code") as f:
    print(f"Status: {f.read().strip()}")
```

### 4. Чтение всех датчиков

```python
#!/usr/bin/env python3
import os, glob
for d in sorted(glob.glob("/proc/sensors/dht/gpio*/")):
    pin = open(os.path.join(d, "pin")).read().strip()
    value = open(os.path.join(d, "value")).read().strip()
    status = open(os.path.join(d, "status_code")).read().strip()
    print(f"GPIO{pin} (status={status}): {value}")
```

### 5. Демон systemd с syslog

```python
#!/usr/bin/env python3
# dht-monitor.py -- демон systemd для логирования показаний DHT в syslog.
import time, syslog, glob, os, signal, sys

INTERVAL = 60
syslog.openlog("dht-monitor", syslog.LOG_PID | syslog.LOG_NDELAY, syslog.LOG_DAEMON)
running = True

def handle_sigterm(signum, frame):
    global running
    running = False
signal.signal(signal.SIGTERM, handle_sigterm)

while running:
    for d in sorted(glob.glob("/proc/sensors/dht/gpio*/")):
        pin = open(os.path.join(d, "pin")).read().strip()
        data = dict(l.split("=") for l in open(os.path.join(d, "value")).read().strip().splitlines())
        msg = f"GPIO{pin}: H={data['H']}% T={data['T']}C"
        syslog.syslog(syslog.LOG_INFO, msg)
    time.sleep(INTERVAL)
syslog.syslog(syslog.LOG_INFO, "dht-monitor stopping")
```

Unit-файл `/etc/systemd/system/dht-monitor.service`:

```ini
[Unit]
Description=DHT Sensor Monitor
After=systemd-modules-load.service

[Service]
Type=simple
ExecStart=/usr/local/bin/dht-monitor.py
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

---

## Коды ошибок

### Коды статуса

| Код | Константа           | Описание                                    |
|-----|---------------------|---------------------------------------------|
| 0   | `ERR_SUCCESS`       | Операция выполнена успешно                   |
| 1   | `ERR_PIN_INVALID`   | Неверный номер GPIO (должен быть 0–27)      |
| 2   | `ERR_GPIO_REQUEST`  | Ошибка запроса/поиска GPIO                  |
| 3   | `ERR_READ_FAILED`   | Ошибка чтения данных (контрольная сумма, тайм-аут) |
| 4   | `ERR_AUTO_MODE`     | Ручное измерение недоступно в режиме автоопроса |
| 5   | `ERR_TOO_SOON`      | Слишком мало времени после последнего измерения (мин. 2 с) |

### Коды errno

| errno   | Значение           | Когда                                     |
|---------|--------------------|-------------------------------------------|
| `-EINVAL` | Неверный аргумент | Неверный пин, неверный интервал           |
| `-ENODEV` | Нет устройства    | Выгрузка драйвера, датчик не найден      |
| `-EFAULT` | Неверный адрес    | Ошибка `copy_from_user`/`copy_to_user`   |
| `-EBUSY`  | Устройство занято  | Пин уже зарегистрирован                   |
| `-ENOMEM` | Нет памяти        | Ошибка `kzalloc` для структуры датчика   |

---

## Параметры конфигурации

### Параметры модуля

| Параметр     | Тип  | По умолчанию | Диапазон | Описание                       |
|--------------|------|-------------|----------|--------------------------------|
| `dht_debug`  | int  | 0           | 0–1      | Отладка через insmod/procfs    |

### Константы времени компиляции

| Константа           | Значение            | Описание                                |
|---------------------|---------------------|-----------------------------------------|
| `MAX_SENSORS`      | 32                  | Максимум одновременно зарегистрированных датчиков |
| `MAX_PIN_NUM`      | 27                  | Максимальный номер BCM GPIO              |
| `MIN_INTERVAL`     | 2                   | Минимальный интервал автоопроса (секунды) |
| `MAX_INTERVAL`     | 60                  | Максимальный интервал автоопроса (секунды)|
| `MEAS_MIN_GAP`     | 2                   | Минимум секунд между измерениями         |
| `MAX_RETRIES`      | 3                   | Попыток чтения до отказа                |
| `RETRY_DELAY_MS`   | 100                 | Задержка между попытками (мс)           |
| `BIT_THRESHOLD`    | 40000               | Порог 0/1 в наносекундах (40 мкс)       |
| `PULSE_TIMEOUT_NS` | 200000              | Макс. наносекунд на импульс (200 мкс)   |
| `CONFIG_PATH`      | `/etc/default/dht` | Путь к файлу конфигурации               |

---

## Режим отладки

### Включение отладки

**Способ 1: Параметр модуля**

```bash
sudo insmod dht.ko dht_debug=1
# или
sudo modprobe dht dht_debug=1
```

**Способ 2: procfs во время выполнения**

```bash
echo 1 | sudo tee /proc/sensors/dht/debug
```

**Способ 3: Файл конфигурации**

```
# /etc/default/dht
DEBUG=1
```

### Отключение отладки

```bash
echo 0 | sudo tee /proc/sensors/dht/debug
```

### Пример вывода отладки

```
[DHT]: debug enabled
[dht_gpio_4]: poll thread started
[dht_gpio_4]: measurement OK - H=45.2% T=23.1 C
[dht_gpio_4]: read attempt 1 failed - j=38, data=[45,0,23,0,68]
[dht_gpio_4]: measurement OK - H=45.2% T=23.1 C
[DHT]: debug disabled
```

---

## Архитектура

### Подсчёт ссылок (`kref`)

Каждая структура датчика имеет `kref` — счётчик ссылок. Начальная ссылка создаётся при регистрации. Дополнительные ссылки берутся в `dht_proc_open()` для каждого открытого файла procfs. Это гарантирует, что структура датчика не будет освобождена, пока у пользователя открыт файл procfs, даже если датчик удалён через `unexport`.

### Контроль вытеснения

Драйвер отключает вытеснение (`preempt_disable`) во время bit-bang чтения для предотвращения искажения тайминга. Прерывания остаются включёнными, чтобы минимизировать влияние на задержки системы. Чтение занимает ~4 мс.

### Поток опроса

Каждый датчик с активным интервалом создаёт собственный поток ядра (`kthread`). Поток:

1. Вычисляет эффективный интервал (глобальный > индивидуальный)
2. Спит с шагом в 1 секунду для отзывчивого завершения
3. Вызывает `dht_do_measurement()` по истечении интервала
4. Применяет ограничение частоты (минимум 2 с)
5. Завершается при `kthread_should_stop()`

### Безопасная выгрузка модуля

Выгрузка модуля (`rmmod`) выполняется в два этапа:

1. Устанавливается флаг `dht_exiting`, отклоняющий новые операции procfs
2. Удаляются все записи procfs (предотвращает новые открытия)
3. Останавливаются все потоки опроса (`kthread_stop`)
4. Освобождаются все датчики и GPIO

Счётчик ссылок `kref` гарантирует, что структуры датчиков освобождаются только после закрытия всех открытых файлов.

### Диаграмма потока измерений

```
Пользователь записывает "1" в /proc/.../measure
        |
        v
+-------------------+
| dht_do_measurement|
| (manual=true)     |
+-------------------+
        |
        v
+-------------------+     +-------------------+
| Проверка частоты  |---->| ERR_TOO_SOON (5)  |
| (мин. 2 с)        |     +-------------------+
+-------------------+
        |
        v
+-------------------+     +-------------------+
| atomic_cmpxchg    |---->| ERR_READ_FAILED   |
| (measuring 0->1)  |     | (уже выполняется) |
+-------------------+     +-------------------+
        |
        v
+-------------------+
| last_attempt_time |
| = now             |
+-------------------+
        |
        v
+-------------------+
| Освобождение lock |
+-------------------+
        |
        v
+-------------------+
| dht_read_sensor   |
| (20ms + 4ms)      |
+-------------------+
        |
        v
+-------------------+
| measuring = 0     |
+-------------------+
        |
        v
+-------------------+     +-------------------+
| Сохранение        |---->| ERR_READ_FAILED   |
| результатов      |     | (3) при ошибке    |
+-------------------+     +-------------------+
        |
        v
  Обновление: humidity, temperature, type (если первое), status, last_meas_time
```

---

## История версий

| Версия | Дата       | Изменения                                                              |
|--------|------------|-----------------------------------------------------------------------|
| 2.8.5  | 2026-01-15 | Патч #16: `iterate_dir` для проверки пустоты procfs                  |
|        |            | Патч #15: `last_attempt_time` обновляется после успешного `cmpxchg`|
|        |            | Патч #13: `atomic_cmpxchg` без двойного отрицания                    |
|        |            | Совместимость с ядром 5.0+ (макросы proc_ops, pde_data)              |
|        |            | Поддержка DKMS (`dkms.conf`)                                          |
|        |            | Makefile: `check`, `uninstall`, кросс-компиляция                     |
|        |            | README: 28 разделов, протокол DHT, таблица совместимости ядер        |
| 2.8    | 2026-01-10 | Начальный публичный релиз                                             |

---

## Известные ограничения

- Минимальный интервал между измерениями: 2 секунды (ограничение датчика)
- Максимум 32 одновременно зарегистрированных датчика
- Поддерживаются только BCM GPIO 0–27
- Чтение занимает ~4 мс с отключённым вытеснением — может влиять на задержки реального времени
- Датчики DHT иногда не отвечают; драйвер делает 3 попытки с задержкой 100 мс
- Автоопрос и ручное измерение не могут работать одновременно
- Нет поддержки прерываний GPIO — только bit-bang чтение

---

## Устранение неполадок

| Симптом                              | Причина                       | Решение                                    |
|--------------------------------------|-------------------------------|---------------------------------------------|
| `make` не удаётся: заголовки не найдены | Отсутствуют `linux-headers` | `sudo apt install linux-headers-$(uname -r)`|
| `make` не удаётся: ядро слишком старое | Ядро < 5.0                  | Обновите ядро или используйте старую версию  |
| `implicit declaration of 'proc_read'` | Ядро < 5.6, нет макроса      | Убедитесь, что в `dht.c` есть `DHT_PROC_OPS` |
| `make` не создаёт `dht.ko`           | `obj-m` не на верхнем уровне  | Убедитесь, что `obj-m += dht.o` перед `ifdef`|
| `WARNING: ARCH= but CROSS_COMPILE=`    | Несоответствие кросс-компиляции | Укажите оба `ARCH` и `CROSS_COMPILE`      |
| Сборка DKMS не удаётся               | Нет `dkms.conf` или заголовков | `sudo apt install dkms linux-headers-...`   |

### Частые проблемы

**`modprobe: Module dht not found`**

Модуль не установлен в дерево ядра. Выполните:

```bash
make install
sudo modprobe dht
```

Если `depmod` не запустился автоматически (нет `System.map`):

```bash
sudo depmod -a
sudo modprobe dht
```

**`dht.ko.xz остаётся после make uninstall`**

Современные ядра сжимают модули (`.ko.xz`, `.ko.zst`). Цель `uninstall` ищет все варианты сжатия. Убедитесь, что используете актуальную версию Makefile.

**Чтение возвращает `ERR_READ_FAILED` (код 3)**

- Проверьте подключение датчика (VCC, DATA, GND)
- Убедитесь в наличии подтягивающего резистора 4.7кОм–10кОм
- Проверьте номер GPIO (BCM, не физический пин)
- Включите отладку: `echo 1 | sudo tee /proc/sensors/dht/debug`
- Проверьте dmesg на наличие диагностических сообщений

**`ERR_TOO_SOON` (код 5)**

Слишком мало времени с последнего измерения. Минимум 2 секунды. Дождитесь истечения интервала или используйте автоопрос.

---

## Файлы в этом репозитории

| Файл            | Описание                                                       |
|-----------------|----------------------------------------------------------------|
| `dht.c`         | Исходный код драйвера (один файл, ~2400 строк)                  |
| `Makefile`      | Сборка с проверками, install/uninstall, кросс-компиляция      |
| `dkms.conf`     | Конфигурация DKMS для автоматической пересборки при обновлении ядра |
| `LICENSE`       | Текст лицензии GNU General Public License v3                  |
| `README.md`     | Этот файл (английская версия)                                  |
| `README_RU.md`  | Русская версия документации                                     |
| `changelog.txt`  | Журнал изменений (EN)                                          |
| `changelog_ru.txt` | Журнал изменений (RU)                                       |
| `ReleaseNotes.md` | Примечания к релизу для GitHub                                 |

---

## Лицензия

Эта программа является свободным программным обеспечением; вы можете распространять и/или изменять её на условиях GNU General Public License версии 3, опубликованной Free Software Foundation.

Copyright (c) 2026, Chapvic

### Благодарности

Реализация протокола DHT основана на принципах, описанных в библиотеке Adafruit DHT и различных драйверах GPIO ядра Linux. Подход bit-bang с `preempt_disable` следует сложившимся практикам в сообществе ядра Linux.
