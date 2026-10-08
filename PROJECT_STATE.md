# NOVA3 Bridge (N.O.V.A. 3 -> Android APK для OnePlus 13s): состояние проекта

Описание архитектуры для читателей репозитория — `nova3-native-arm/android/README.md`.

Обновлено: 2026-10-08.

Рабочая папка: `/home/james/nova3probe` (в Windows: `\\wsl.localhost\Ubuntu-20.04\home\james\nova3probe`).
Всё исполняется в WSL (Ubuntu-20.04); VS Code открыт с Windows. Телефон: OnePlus 13s (CPH2723, Snapdragon 8 Elite,
Oryon, только AArch64, Android/OxygenOS 15+), подключён по USB, виден через Windows `adb.exe`.

## Правила работы
- По-русски, кратко.
- **Установка APK + перезапуск игры — можно всегда. Любая другая команда на телефоне (push, rm, shell, профиль,
  настройки, бенчмарки) — только после явного «да» пользователя.**
- Перед крупными загрузками спрашивать. Ничего не ставить глобально (sudo нет); сборки — в WSL/Docker.
- git: репозиторий `nova3-native-arm` (origin github.com/EapRules/nova3-native-arm). Работа Android-обёртки —
  в локальной ветке `android-apk` (не запушена).

## Архитектура APK
- Игра (ARM32, softfp) исполняется в **своём qemu-arm 11.1.2** (статический aarch64, в APK как `libqemu.so`),
  порт-лоадер `build/nova3` (armhf) + armhf sysroot. Данные игры: APK (libNOVA3_neon.so) в
  `Android/data/com.eaprules.nova3/files/`, OBB в `Android/obb/com.eaprules.nova3/`.
- **GPU-мост**: все GLES-вызовы игры сериализуются в пайп (`portbase/src/gl_bridge.cpp`), Java (`GlBridge.java`)
  исполняет их на Adreno 830 через EGL/GLES30 на SurfaceView. Синхронные запросы — второй пайп.
- Ввод: «обычный телефон» (`NOVA3_TOUCH=1`, `nativePowerStatus(false)`), сырой мультитач в `touchEvent`
  (1 down, 2 move, 0 up, pointer id), Back -> keycode 4, геймпад автоматически переключает `pad 1`.
- Одна копия игры на процесс (`singleTask`, полный `configChanges`, статический GameProcess).
- Оверлей: счётчик FPS (presented frames).
- Звук: SDL disk -> FIFO -> AudioTrack.
- Манифест: `debuggable=false` (CheckJNI резал GL-поток), `profileable shell=true`, `isGame`, `game_mode_config`
  (supportsPerformanceGameMode). ADPF: `PerfHint.java` — hint-сессия на потоки qemu, отчёт длительности кадров.

## Файлы-настройки рядом с логом (Android/data/com.eaprules.nova3/files/)
- `nova3.log` — лог игры и обёртки (пересоздаётся при запуске).
- `gpu_name.txt` — подмена GL_RENDERER (сейчас `Adreno (TM) 320` -> профиль GPU_3 вместо GPU_5).
- `engine_env.txt` — KEY=VALUE в окружение порта (сейчас `NOVA3_GL_PROFILE=1`).
- `resolution.txt`, `pad_overlay.flag`, `notrace.flag`, `qemu_args.txt`, `diag_cmd.txt`, `xmldump.flag`, `xml_override.txt`.
- Переменные порта: `NOVA3_GAME_LOG=1` (все логи игры), `NOVA3_READPIXELS_EVERY`, `NOVA3_PROGRAM_INFO=0`,
  `NOVA3_PHYSICS_THREAD`, `NOVA3_GL_PROFILE=1|2`; qemu: `QEMU_STRICT_FP=1` (выключить быстрый float).
- Меню настроек в APK сделано (SettingsActivity: FOV, разрешение до 720p, кадры в полёте, физика, профилирование; settings.txt).

## Сборка и инструменты (tools/, запуск: `wsl -d Ubuntu-20.04 --cd /home/james -e sh nova3probe/tools/run.sh nova3probe/tools/<x>.sh`)
- `build_install.sh noinstall` — порт + APK (берёт свой qemu из `qemu-build/out/`, если есть).
- `launch.sh [install]` — установить и перезапустить игру. `adb` — обёртка над Windows adb.exe.
- `pull_log.sh`, `grep_log.sh`, `fps.sh`, `cpu_diag.sh` (частоты/ядра/температуры), `profile.sh N` (simpleperf),
  `qemu_hot.py` (символизация qemu), `timeline.py` (FPS/нагрузка по логу), `log_digest.py`, `sync_ops.py`.
- qemu: исходники `qemu-src/qemu-11.1.2`, Docker-образ `nova3-qemu-build` (`qemu-build/Dockerfile`),
  сборка `docker run --rm -u 1000:1000 -e HOME=/tmp -v /home/james/nova3probe:/work nova3-qemu-build bash /work/qemu-build/build_lf.sh`,
  затем `tools/qemu_finish.sh` (strip), `tools/fptest.sh` (бит-в-бит тест float), патчи — `qemu-build/patches/nova3.patch`
  (`tools/qemu_patch_save.sh`). `android/patch_syscalls.py` переписывает set_robust_list/rseq/faccessat2 под seccomp.

## Последняя сессия (2026-10-08, вечер) — итог: «результат вообще разъеб» (пользователь)
- qemu вариант **D** (в APK): быстрый A32 `lookup_tb_ptr` (вызовы/возвраты 1,7×), jmp-cache 16 бит, -O3 armv8.2,
  **ленивое FPCR для NEON** (NEON 6,7× на стенде: писать FPCR на Oryon очень дорого). Стенд: `tools/bench_phone.sh`,
  бенчмарк `armbench`, варианты в `qemu-build/out/qemu-arm-<V>.stripped`, сборка `qemu-build/build_variant.sh`.
- Патч qemu и сборочные файлы скопированы в репозиторий: `nova3-native-arm/android/qemu/` (README там).
- Системный game mode `performance` выставлен (`cmd game mode performance com.eaprules.nova3`).
- Телеметрия в логе: ядро и частота главного потока qemu (`| cpuN cur/max MHz` в строке `[gl] frames`).
- `cpu_mask.txt` (hex, напр. `c0`) рядом с логом закрепляет qemu на ядрах через taskset — встроено, НЕ включено.
- Tango (системный транслятор ARM32) в глобальной прошивке отсутствует (`1554_Tango` — это оператор связи).
  Оригинальной NOVA 3 на телефоне нет.
- `HardwareSkinning` в движке — мёртвый флаг (никто не читает поле DeviceOptions+0x24); скиннинг выбирает
  `CColladaFactoryChooseSkin` по техникам материалов. Подмена через `xml_override.txt` (лежит на телефоне,
  безвредна) эффекта не дала. Профили pugixml расшифрованы (дамп в logs/xmldump): таблица GPU, GPU_3/4, CPU_M, MEM_512.
  Кастомный `Adreno_(TM)_320.xml` делает картинку тяжелее (тени, Details 0, RTT) — учесть при выборе gpu_name.
- **Следующий шаг (найдено последним замером, `QEMU_SYSCALL_STATS=1` включён в GameProcess):** ~730 openat/с —
  SDL каждый кадр ищет libudev.so (опрос джойстиков) + access('/.flatpak-info'); ещё ~800 clock_gettime/с
  (vDSO qemu для ARM — просто svc). Убрать: SDL hint SDL_JOYSTICK_DISABLE_UDEV / не инициализировать
  joystick/gamecontroller подсистемы SDL в Android-режиме (ввод идёт через ctl/commands). Затем:
  быстрый clock_gettime в qemu, кэш возвратов (E3), лёгкие профили качества через xml_override,
  эксперимент cpu_mask.txt.

## Что сделано по производительности (бой: было 12–16 FPS, стало до 55, просадки от лимитера частот)
- qemu 11.1.2: быстрый float на FPU хоста (VFP add/sub/mul/div/fma, конверсии), NEON f32 (vadd/vsub/vmul/vmla/vmls,
  в т.ч. по скаляру) циклом под FPCR=FZ+DN, cpsr_write без rebuild_hflags для NZCVQ/GE,
  realpath в open только для путей с "proc" (иначе faccessat2 -> SIGSYS). Тест бит-в-бит проходит.
- Мост: без мьютекса (однопоточный GL), запись команд словами, отсев повторных glUseProgram/ActiveTexture/
  BindTexture/uniform (uniform/кадр 1720 -> ~290), информация о программе одним запросом при линковке,
  тень индексных буферов (draw с индексами в VBO и вершинами в памяти), троттлинг glReadPixels.
- Лог игры: по умолчанию только W/E/F, одна запись на строку (было 3+ syscalls на каждое касание).
- GPU-профиль движка через подмену GL_RENDERER; перехваты TinyXML/SlimXML (профили в них не идут — нужен IrrXML).

## Главные выводы диагностики
- Узкое место — **главный поток игры в qemu** (85–100% одного ядра); GPU-поток 15–45%, видеокарта простаивает.
  Параллельность есть (каждый гостевой поток на своём ядре), но движок почти всё делает в одном потоке.
- **Просадка со временем — частотный лимитер OxygenOS**, не нагрев: при одинаковой работе на кадр step() растёт
  61 -> 84 мс; потолки прайм-ядер режутся до 1,2–1,4 ГГц (из 4,32), температуры ~53 °C, thermal status 0.
  «Allow background activity» + системный high performance помогли (потолки до 3,07 ГГц), но лимитер всё равно
  со временем душит. Приложение не в списке игр OnePlus.
- Профиль qemu-потока: поиск блоков при косвенных переходах ~27% (lookup_tb_ptr, get_tb_cpu_state, qht,
  interval_tree), транслированный код ~25%, NEON ~8%. Работа на кадр в бою: 110–180 draw, 2,4–5,8 МБ клиентских
  вершин (CPU-скиннинг L_NEONSKIN_*), ~300–450 uniform.
- glReadPixels в бою не вызывается (просадки на бликах — просто доп. эффекты).

## План (по приоритету)
P0 (телефон, минуты): проверить Tango (системный ARM32-транслятор OnePlus/OPPO: `ro.product.cpu.abilist`,
`ro.dalvik.vm.native.bridge`, libtango) и запуск оригинальной NOVA 3 (установлена у пользователя);
`cmd game mode performance com.eaprules.nova3`; добавить игру в приложение Games; телеметрия частоты ядра qemu в логе.
P1: стенд бенчмарков qemu на телефоне (/data/local/tmp) -> E1 jmp-cache 12->16 бит, E4 -O3/LTO/-march, E5 VFP-хелперы,
E2 быстрый lookup_tb_ptr, E6 больше NEON; аппаратный скиннинг (найти хранение опции HardwareSkinning);
отключение дорогих эффектов через сеттеры DeviceOptions; E7 -perfmap (TMPDIR) + символы игры.
P2: HLE-обёртки в qemu (zlib inflate игры — загрузки, memcpy/libm, кодирование моста), общая память вместо пайпа,
ограничение FPS/sustained mode, меню настроек; E3 кэш возвратов в TCG.

## Известные проблемы
- Заставка на титульном экране рисуется в левом верхнем углу (причина не найдена; меню и игра — во весь экран).
- Загрузки долгие (эмуляция распаковки).
- TODO: разобрать редкое зависание на экране загрузки (2026-10-08, сразу после инициализации звука: главный гостевой поток
  в futex, CPU простаивает, фаза LOADING frame=0; один раз, повторить не удалось). Подозрение: наша ldrex/strex-замена
  kuser cmpxchg (NOVA3_FAST_CAS=0 отключает) или старая гонка при старте. Смотреть лог такого запуска и стек потоков.
