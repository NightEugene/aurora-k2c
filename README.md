# Kingdom: Two Crowns для ОС Аврора

Лаунчер и слой совместимости для запуска Kingdom: Two Crowns на устройствах
с ОС Аврора, aarch64. Версия лаунчера — **1.1.2**.
Поддерживается совместимый APK с Unity **6000.0.61f1**.

## Возможности

- Выбор APK и автоматический запуск ранее выбранного файла.
- Одно окно Qt Quick с альбомным изображением и обложкой приложения.
- Сенсорное управление, мультитач до 4 касаний.
- Звук через AAudio → PulseAudio.
- Вывод изображения через GLES-текстуру.
- Настройки и сохранения в приватном хранилище приложения.
- Удержание экрана включённым во время активной игры.
- Работа на устройствах без системной `libandroid.so`.

## Требования

Для сборки нужны Aurora SDK 5.2.1.200 с Docker build tools, Python 3.9+
и совместимый APK. На устройстве необходимы libhybris, libgralloc,
libhardware, libui и libmediandk. При отсутствии libandroid графические
буферы используются через системную libnativewindow.

APK, извлечённые библиотеки движка, игровые данные и ключи подписи
предоставляются отдельно.

## Сборка

В корне проекта выполните:

```sh
python3 launcher/prepare-runtime.py /path/to/compatible-game.apk
sh launcher/build-rpm.sh
```

Каталог SDK задаётся переменной `AURORA_SDK_ROOT`. По умолчанию используется
`~/.local/share/aurora-sdk/sdk/5.2.1.200`.
Если SDK доступен как готовый Docker-образ, задайте `AURORA_BUILD_IMAGE`:

```sh
AURORA_BUILD_IMAGE=your-aurora-sdk-image:5.2.1.200 sh launcher/build-rpm.sh
```

Эта переменная также поддерживается сборкой встроенного варианта и скриптом
`launcher/deploy.sh`, принимающим SSH-профиль устройства первым аргументом.
Сборка подписывает RPM и запускает валидатор SDK с профилем сертификата.
Результат:

```text
launcher/build-rpm/RPMS/ru.nighteugene.k2clauncher-1.1.2-1.aarch64.rpm
```

Подготовка runtime проверяет SHA-256 исходного `libunity.so`:
`2de032fd5d52b0a6cd684c1b13b5dc8739b0035bc9e07baeb36e16173a923292`.
После адаптации тайминга кадров ожидается SHA-256:
`e24a11c0451f3be60e8d033a89382db7f4d915f9135324c777b56dab50ceaaa8`.
Для другого движка требуется отдельная адаптация. Существующий runtime
не перезаписывается; новый каталог задаётся через `--output`.

## Установка

```sh
scp launcher/build-rpm/RPMS/ru.nighteugene.k2clauncher-1.1.2-1.aarch64.rpm defaultuser@192.168.2.15:~/Downloads/
ssh defaultuser@192.168.2.15 'sdk-deploy-rpm --silent --keepUserData ~/Downloads/ru.nighteugene.k2clauncher-1.1.2-1.aarch64.rpm'
```

Для сертификата SDK может потребоваться режим разработчика. Откройте
**Kingdom**, предоставьте запрошенные разрешения, выберите APK и нажмите
**Запустить игру**. Галочка **Пропускать в следующий раз** включает
автоматический запуск. Если APK недоступен, снова откроется выбор файла.

## Встроенный вариант

```sh
sh launcher/build-builtin-rpm.sh /path/to/compatible-game.apk
```

Этот вариант включает APK в RPM и сразу запускает игру. Его App ID —
`ru.nighteugene.kingdom`; настройки, кэш и сохранения независимы от лаунчера.
Результат — `launcher/build-builtin-rpm/RPMS/ru.nighteugene.kingdom-1.0.0-2.aarch64.rpm`.
Для установки требуются место под пакет и распакованные игровые данные.

## Данные приложения

| Данные | Путь |
| --- | --- |
| Настройки | `~/.config/ru.nighteugene/k2clauncher/settings.ini` |
| Извлечённые assets | `~/.local/share/ru.nighteugene/k2clauncher/apk-data/assets` |
| Сохранения | `~/.local/share/ru.nighteugene/k2clauncher/game` |
| Журнал запуска | `launcher.log` в каталоге сохранений |

На первом запуске создаются начальные настройки: русский язык и масштаб 2×.
Существующие игровые настройки сохраняются.

## Архитектура

Qt Quick управляет окном, выбором файла и жизненным циклом приложения.
Unity работает в потоке того же процесса. libhybris загружает Android
библиотеки, JNI bridge предоставляет необходимое окружение, а API hooks
связывают ресурсы, ввод и звук с нативными сервисами.

При наличии системной libandroid используются её Looper и Choreographer.
Иначе цикл событий реализуется через pthread keys, eventfd и poll;
callbacks кадров вызываются с монотонными временными метками. Библиотека
совместимости в RPM предоставляет ELF-символы для Android linker.
Графические буферы выделяются через AHardwareBuffer и выводятся в окно Qt.

## Структура

- `launcher/` — C++/QML лаунчер, ресурсы, подготовка runtime и сборка RPM.
- `launcher/rpm-source/` — конфигурация сборки и bridge libhybris.
- `launcher/rpm/` — спецификации пакетов.
- `shim/src/` — JNI, Android API, графика, ввод и аудиомост.
- `shim/tests/` — тест цикла событий Android API.

Сборочные скрипты копируют исходники в генерируемые каталоги.

## Проверка цикла событий

```sh
cc -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
  -I shim/src shim/tests/android_compat_test.c shim/src/android_compat.c \
  -o /tmp/k2c-android-compat-test
/tmp/k2c-android-compat-test
```

## Диагностика и ограничения

`K2C_DEBUG_STDIO=1` оставляет вывод в консоли, `K2C_JNI_TRACE=1` включает
подробную диагностику JNI, `K2C_CPU_MODE=all` отключает выбор мощных ядер.

Поддерживается только aarch64 и указанная версия Unity. JNI реализован
частично; ART, Android Application и DEX не исполняются. Поведение плагинов,
сетевых сервисов и модификаций может отличаться от Android. Native-библиотеки
модификаций входят в локально подготовленный runtime. Частота кадров зависит
от устройства и сцены. На устройствах без libandroid датчики движения
сообщаются как отсутствующие; игра использует сенсорное управление.
