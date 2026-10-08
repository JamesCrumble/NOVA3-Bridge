# NOVA3 Bridge

Запуск 32-битной N.O.V.A. 3 (1.0.7, armeabi-v7a) на 64-битных Android-телефонах без AArch32
(проверено на OnePlus 13s, Snapdragon 8 Elite): собственный пропатченный qemu-user исполняет игру,
а её OpenGL ES вызовы пробрасываются на настоящий GPU телефона. Игра не распространяется — нужны свои APK и OBB.

## Структура

| папка | что |
|---|---|
| `port/` | порт-лоадер (bionic ELF loader, JNI/Android API, GL-мост) и `port/android/` — APK-обёртка. Подробная архитектура: [port/android/README.md](port/android/README.md) |
| `qemu/` | патч qemu 11.1.2, Dockerfile для кросс-сборки, тест точности float, бенчмарк. См. [qemu/README.md](qemu/README.md) |
| `tools/` | сборка/установка (`build_install.sh`, `launch.sh`), логи и телеметрия телефона, профилирование, стенд qemu; `tools/diag/` — разовые исследования |
| `PROJECT_STATE.md` | журнал разработки: состояние, выводы, план |

## Сборка (WSL Ubuntu)

Нужны (не входят в репозиторий, кладутся в корень): `jdk/` (JDK 17), `android-sdk/` (build-tools 34.0.0, platform 34,
platform-tools), `sdl-arm/` (SDL2 armhf), armhf-тулчейн (`gcc-arm-linux-gnueabihf`), Docker.

```sh
source env.sh
tools/qemu_fetch.sh          # исходники qemu + docker-образ; затем patch -p1 < qemu/patches/nova3.patch в qemu-src/qemu-11.1.2
tools/qemu_build.sh          # наш qemu-arm (вариант D) + проверка float
tools/build_install.sh       # порт + APK + установка на телефон (adb)
```

## Лицензия

GPL-3.0 (см. `LICENSE`, происхождение частей — `port/NOTICE.md`). Патч qemu — GPL-2.0-or-later, как сам QEMU.
Порт N.O.V.A. 3 для PortMaster, на котором основан проект, создан EapRules.
N.O.V.A. 3 © Gameloft.

## Релизы и настройки

Готовые сборки — на странице Releases (релизный APK, исходники, инструкция `INSTALL.txt`). Игра и её данные
не входят в репозиторий. При старте приложения открывается меню настроек: угол обзора, разрешение рендера
(до 720p), кадры «в полёте», качество (тени, пост-эффекты, LOD, детали), закрепление на быстрых ядрах.
История версий — `CHANGELOG.md`; релиз собирает `tools/make_release.sh`.
