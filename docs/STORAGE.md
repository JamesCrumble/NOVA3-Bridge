# Хранилище файлов: папка BridgeGames (N.O.V.A. 3)

Модуль `Storage.java` (лежит рядом с `GameProcess.java`) не привязан к этой игре: все пути считаются
от имени пакета (`getPackageName()`), внешних зависимостей нет. Чтобы получить то же поведение в другом порту, достаточно
скопировать файл (поменять `package`) и подключить его, как описано ниже.

## Как это работает

Есть две папки приложения:

| Папка | Путь | Когда доступна |
|---|---|---|
| основная (shared) | `/sdcard/BridgeGames/<пакет>/` | с доступом ко всем файлам (API 30+) или с разрешением на запись (API 26-28) |
| старая (legacy) | `/sdcard/Android/data/<пакет>/files/` | всегда |

- Приложение при запуске просит доступ ко всем файлам. Отказ не мешает: всё работает в legacy-папке.
- С доступом приложение само создаёт `BridgeGames/<пакет>/` и переносит туда **все файлы** из legacy (каждый файл копируется во временный `.part`, размер сверяется, затем переименование и удаление оригинала; уже
  существующие файлы не перезаписываются, неполная копия прерванного переноса заменяется). После этого shared
  становится основной: лог, настройки и флаги лежат там.
- APK, cache и любые другие файлы ищутся в обеих папках, сначала в основной.
- Сохранения игра хранит в приватной папке приложения (она не переносится).
- API 29 оставляет scoped storage (общий доступ к файлам для целевого API 34 невозможен), там используется только legacy.

## Подключение в другом порту

1. Манифест:

   ```xml
   <uses-permission android:name="android.permission.READ_EXTERNAL_STORAGE" android:maxSdkVersion="29" />
   <uses-permission android:name="android.permission.WRITE_EXTERNAL_STORAGE" android:maxSdkVersion="28" />
   <uses-permission android:name="android.permission.MANAGE_EXTERNAL_STORAGE" />
   ```

2. На стартовом экране: если `!Storage.hasAccess(ctx)`, показать пояснение и вызвать `Storage.requestAccess(activity, код)`;
   в `onResume` / `onRequestPermissionsResult` при появлении доступа вызвать `Storage.migrate(ctx, null)` в фоновом потоке
   (там может быть копирование).
3. Перед запуском игры (в фоновом потоке) вызвать `Storage.migrate(ctx, прогресс)`.
4. Вместо `getExternalFilesDir(null)` использовать:
   - `Storage.mainDir(ctx)`: основная папка (лог, настройки);
   - `Storage.find(ctx, "settings.txt")` / `Storage.firstWithSuffix(ctx, ".apk")`: поиск в обеих папках;
   - `Storage.saveDir(ctx, приватнаяПапкаСохранений)`: куда писать сохранения;
   - `Storage.describe(ctx)`: обе папки для сообщения об ошибке.

Пример использования: `GameProcess` (`start()`, `prepare()`) и `SettingsActivity` в этом репозитории.
