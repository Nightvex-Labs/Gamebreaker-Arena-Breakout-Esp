# Packaging Runbook — v1.0.20+ anti-theft

Точная процедура выпуска релиза для админки KoenFlow (arena-breakout-esp).
Копия этого файла лежит в `C:\GamebreakerHack\PACKAGING.md` — процедура
идентичная, отличаются только пути и product_id.

---

## 0. Итоговая форма (что получаем)

```
releases\WinRuntimeHost.zip   (~15-16 MB)
  ├── WinRuntimeHost.exe       ← VMProtect Ultra 3.9.4 + AHBE-трейлер с bundle
  └── db\
      ├── EneIo64.bin           ← kdu vulnerable driver #6
      ├── inpoutx64.bin         ← kdu #26 (REDFOX)
      ├── MsIo64.bin            ← kdu fallback
      └── rtkio64.bin           ← kdu fallback
```

**WinRuntimeHost.exe = единый бинарь**. Bundle с зашифрованным overlay
пришит трейлером в хвост, ключ запечён в стаб и виртуализован VMProtect.
Никаких sidecar-файлов.

---

## 1. Что должно быть готово ДО сборки (pre-flight)

### 1.1 Toolchain
- **Visual Studio 2022** (Community / BuildTools / Professional / Enterprise
  — build.bat автодетектит все four).
  Обязательный компонент: MSVC v143 x64/x86 build tools + Windows SDK 10.
- **Python 3.10+** с `cryptography`:
  ```
  python -c "from cryptography.hazmat.primitives.ciphers.aead import AESGCM"
  ```
  Если import fail: `pip install cryptography`.

### 1.2 VMProtect
- `VMProtect_Con.exe` должен лежать по одному из двух путей (в этом порядке):
  1. `C:\vmp\notVmpFull\notVmp\VMProtect_Con.exe`  ← primary
  2. `C:\vmp\notVmp\VMProtect_Con.exe`             ← fallback
- Пароль на исходный `notVmp.zip` — `1231`.
- Проверить: `dir C:\vmp\notVmpFull\notVmp\VMProtect_Con.exe`

### 1.3 Актуальные оффсеты
Файл `inc\ah_offsets.h` — при каждом патче ABI Tencent катит новые RVA.
В шапке файла зафиксирована дата последнего дампа. Если офсеты старше 5 дней:
```
C:\abidumper\RUN.bat
```
Скопировать значения в `inc\ah_offsets.h` вручную; либо `abidumper` может
сам их подставить — см. `C:\abidumper\README.md`.

### 1.4 Свежий overlay build (если правил src\*)
Если менял C++ код overlay — пересобрать:
```
cd C:\arenaREmake\arenahack
.\build.bat
```
Дадёт `build\ah_overlay.exe` (~3.9 MB с random padding для hash rotation).

Если правил только launcher (`launcher\src\ah_launcher.c`) — overlay можно
не пересобирать, использовать `--no-overlay-build` в make_release_zip.

### 1.5 AH_DIAG (debug логи overlay)
Файл `build.bat`, строка 55:
```
set OVFLAGS=... /DAH_DIAG ...
```
- **ВКЛЮЧЁН /DAH_DIAG** (сейчас так): overlay пишет диагностику в
  `C:\Users\Public\ah_reader.log` + `ah_procs.log`. Оба крутятся,
  раздуваются быстро (десятки MB за час).
- **ВЫКЛЮЧИТЬ /DAH_DIAG** для тихого prod-релиза: убрать `/DAH_DIAG`
  из строки, пересобрать.

Сейчас поддерживаем **включённым** — телеметрия важна для диагностики.
Отключаем перед wide-distribute.

### 1.6 Убить работающие инстансы
Перед пересборкой overlay/launcher — killить с elevation, иначе LNK1104
"cannot open file 'build\ah_overlay.exe'":
```powershell
Start-Process -Verb RunAs -FilePath 'powershell.exe' -ArgumentList `
  '-NoProfile','-Command','Get-Process ah_overlay,WinRuntimeHost -EA SilentlyContinue | Stop-Process -Force' -Wait
```

---

## 2. Три режима сборки

Все три вызываются через `scripts\make_release_zip.py`. Отличаются
источником KFPL ключа для decrypt bundle.

### 2.1 --zero-key (★ ANTI-THEFT — DEFAULT для prod)
```
python scripts\make_release_zip.py --zero-key --version 1.0.20
```
- Baked `KFPL_KEY[32] = 00 00 00 ...` (пустая заглушка).
- **Ключ приходит ТОЛЬКО из launcher context.json** (DPAPI-wrapped или plain-JSON).
- Копия WinRuntimeHost.exe без launcher'а — тихий exit (см. anti-theft ниже).
- Скрипт печатает свежий b64 key в stdout — **этот key копируется в поле
  KFPL KEY админки**.

**Это режим, который используется для всех prod-релизов начиная с v1.0.20.**

### 2.2 (без флага) — baked mode (legacy, для локального теста)
```
python scripts\make_release_zip.py --version 1.0.19-local
```
- `bake.py --bake` записывает **random real key** в исходник `ah_launcher.c`.
- WinRuntimeHost.exe запустится **сам по себе** без launcher'а (baked key
  используется как fallback).
- **Для prod не годится** — теряется anti-theft: любая копия exe работает.
- Полезно для локального smoke-теста без KoenFlow launcher'а.

### 2.3 --no-kfpl — открытый overlay (deprecated, не использовать)
```
python scripts\make_release_zip.py --no-kfpl --version 1.0.18
```
- Полностью убирает KFPL. WinRuntimeHost.exe = raw ah_overlay.exe с VMP.
- Overlay не зашифрован — spinner-грамот. **Не использовать в prod.**

---

## 3. Пошаговая процедура (v1.0.20+ prod)

### Шаг 1 — Kill running
```powershell
Start-Process -Verb RunAs -FilePath 'powershell.exe' -ArgumentList `
  '-NoProfile','-Command','Get-Process ah_overlay,WinRuntimeHost -EA SilentlyContinue | Stop-Process -Force' -Wait
```

### Шаг 2 — Собрать overlay (если менял src\*)
```
cd C:\arenaREmake\arenahack
.\build.bat
```
Проверить: `dir build\ah_overlay.exe` — должен быть свежий (LastWriteTime
= сейчас).

### Шаг 3 — Полный prod-build
```
python scripts\make_release_zip.py --zero-key --version 1.0.20
```
Скрипт по шагам:
1. `bake.py` — записывает zero-key в исходник launcher'а
2. `launcher\build.bat` — компилит WinRuntimeHost.exe (стаб, ~170 KB)
3. `scripts\vmprotect_wrap.py` — VMProtect Ultra виртуализует **4 функции**:
   - `resolve_key`
   - `aes_gcm_decrypt`
   - `extract_key_from_launch_context` (включая DPAPI unwrap)
   - `antitheft_guard` (все 5 антидебаг проверок)
   Результирующий VMPed стаб — ~13 MB.
4. `pack_kfpl.py` — AES-256-GCM шифрует overlay с random per-build ключом,
   создаёт `bundle.kfpl` с magic `AHKF`.
5. AHBE-трейлер — bundle приклеивается в хвост exe:
   ```
   [ MZ + PE + VMP payload ][ ciphertext bundle ][ u64 LE size ][ 'AHBE' ]
   ```
6. Копирует драйверы из `src\db\*.bin` в `stage\db\`.
7. Zip → `releases\WinRuntimeHost.zip`.

### Шаг 4 — Забрать KFPL key из stdout
Скрипт печатает в конце:
```
KFPL key (hex):    e93b79e4a4467904f75733305f5987e9965a8133e0e65ce6d9fa95c13955c61e
KFPL key (base64): 6Tt55KRGeQT3VzMwX1mH6ZZagTPg5lzm2fqVwTlVxh4=
```
Скопировать **b64** — пойдёт в поле KFPL KEY админки.

### Шаг 5 — Smoke test локально (обязательно!)
```powershell
$smoke = "$env:TEMP\ah_smoke"
if (Test-Path $smoke) { Remove-Item $smoke -Recurse -Force }
New-Item -ItemType Directory -Force -Path $smoke | Out-Null
Expand-Archive C:\arenaREmake\arenahack\releases\WinRuntimeHost.zip -DestinationPath $smoke

# Проверить структуру
Get-ChildItem $smoke -Recurse -File | Format-Table Name,Length -AutoSize

# Проверить MZ header
$b = [System.IO.File]::ReadAllBytes("$smoke\WinRuntimeHost.exe")[0..1]
"first bytes: 0x{0:X} 0x{1:X}" -f $b[0], $b[1]   # ожидаем 0x4D 0x5A

# Проверить AHBE трейлер
$exe = "$smoke\WinRuntimeHost.exe"
$size = (Get-Item $exe).Length
$tail = [System.IO.File]::ReadAllBytes($exe)[($size-12)..($size-1)]
$magic = [System.Text.Encoding]::ASCII.GetString($tail[8..11])
"trailer magic: '$magic'"   # ожидаем 'AHBE'

# Проверить anti-theft: запуск БЕЗ launcher'а должен упасть silent
Start-Process -Verb RunAs -FilePath $exe
Start-Sleep -Seconds 5
Get-Content "$env:TEMP\ah_launcher.log" -Tail 5
# ожидаем:
#   key32 first bytes: 00 00 00 00 ...
#   aes_gcm_decrypt fail NTSTATUS=0xC000A002
# Это правильное поведение — copy-and-run НЕ работает.

# Cleanup
Get-Process ah_overlay,WinRuntimeHost -EA SilentlyContinue | Stop-Process -Force
Remove-Item $smoke -Recurse -Force
```

Если smoke прошёл — релиз готов к деплою.

### Шаг 6 — Загрузка в админку KoenFlow
Форма нового релиза для продукта `arena-breakout-esp`:
```
VERSION            1.0.20                         ← без 'v' префикса
CHANNEL            stable
PACKAGE            WinRuntimeHost.zip
LAUNCH EXECUTABLE  WinRuntimeHost.exe
LAUNCH ARGUMENTS   (empty)
CONTENT KEY        (empty)
LOADER KEY         (empty)
KFPL KEY           6Tt55KRGeQT3VzMwX1mH6ZZagTPg5lzm2fqVwTlVxh4=   ← b64 из stdout
Activate now       ON
```

**⚠️ ВАЖНО backend-side**: продукт должен быть в **plain-launcher mode**
(не loader-product). Если бэкенд оборачивает context.json в KFPC-DPAPI —
это тоже работает, наш стаб unwrap'ит. Но LOADER KEY в форме **должен
остаться пустым** — иначе бэк начнёт inject'ить в env `RTK_LOADER_KEY_B64`,
что нам не нужно.

---

## 4. Anti-theft — что уже стоит (v1.0.20+)

Кратко о защите (для памяти):

### 4.1 Zero-baked key + DPAPI context
- `KFPL_KEY[32]` в бинарнике = все нули.
- Единственный путь к настоящему ключу — `--koenflow-launch-context <ctx.json>`,
  который KoenFlow launcher пишет как:
  - **Plain JSON**: `{"kfplKey":"<b64>", ...}` — если продукт plain-launcher
  - **KFPC-wrapped**: `[4B "KFPC"][DPAPI-protected JSON]` — если loader-product
- DPAPI blob привязан к SID **текущего Windows-пользователя**. Копия
  context.json на другом ПК → `CryptUnprotectData fails` → silent exit.

### 4.2 Anti-debug guard (5 проверок, все в VMProtect)
Файл `launcher\src\ah_launcher.c` → функция `antitheft_guard()`:
1. `IsDebuggerPresent()`
2. `CheckRemoteDebuggerPresent()`
3. PEB `NtGlobalFlag & 0x70` (heap debug bits)
4. Dr0-Dr3 hardware breakpoint registers
5. `NtQueryInformationProcess(ProcessDebugPort)` + `(ProcessDebugObjectHandle)`

Любая положительная проверка → `TerminateProcess(GetCurrentProcess(), 0)`.
**Никаких MessageBox, никаких логов** — не помогаем реверсерам определить
какая проверка сработала.

### 4.3 VMProtect Ultra 3.9.4 — 4 функции виртуализованы
Смотреть в build log при VMP wrap:
```
[U] 140001EE3 VMProtectMarker "resolve_key"
[U] 14000274A VMProtectMarker "aes_gcm_decrypt"
[U] 1400014A5 VMProtectMarker "extract_key_from_launch_context"
[U] 1400024EB VMProtectMarker "antitheft_guard"
```
Если каких-то маркеров не хватает — VMP не нашёл. Значит их удалил linker
(OPT:REF выкинул неиспользованные) или функция inline'нулась. Проверь
что все 4 функции вызываются из wmain и объявлены `static void`.

---

## 5. Common issues и fixes

### 5.1 "LNK1104: cannot open file 'build\ah_overlay.exe'"
Overlay процесс держит файл. Kill elevated:
```powershell
Start-Process -Verb RunAs -FilePath 'powershell.exe' -ArgumentList `
  '-NoProfile','-Command','Get-Process ah_overlay -EA SilentlyContinue | Stop-Process -Force' -Wait
```

### 5.2 "vcvars64.bat not found"
VS2022 не установлен или в нестандартном пути. Открыть `build.bat`,
дописать свой путь в список `VCVARS=` через `if not exist`.

### 5.3 "VMProtect_Con.exe not found"
Проверь: `dir C:\vmp\notVmpFull\notVmp\VMProtect_Con.exe`. Если нет —
распакуй `notVmp.zip` (пароль `1231`) в `C:\vmp\`.

### 5.4 Client сообщает Error #15 при Play
Диагноз в файле `%LOCALAPPDATA%\KoenFlowLauncher\logs\launcher.log`:
```powershell
Get-Content "$env:LOCALAPPDATA\KoenFlowLauncher\logs\launcher.log" -Tail 50 | Select-String "branch:"
```
Смотреть строчку `branch: !Success → PostDhError(15) msg='...'` — там
точная причина.

И проверить наш стаб log:
```powershell
Get-Content "$env:TEMP\ah_launcher.log" -Tail 20
```
- `context: kfplKey extracted (b64 44 chars, DPAPI=0)` = plain-JSON path OK
- `context: kfplKey extracted (b64 44 chars, DPAPI=1)` = KFPC-wrapped OK
- `context: DPAPI unwrap FAILED` = context.json попал не тому юзеру
- `key32 first bytes: 00 00 00 00 ...` = context не пришёл вообще → анти-вор
- `aes_gcm_decrypt fail NTSTATUS=0xC000A002` = ключ неправильный

### 5.5 Client запускается, но GWorld не находит
Смотреть `C:\Users\Public\ah_reader.log`:
```
LATCH BAILOUT — GWorld stayed 0 for 30s after attach (pid=... procCR3=...)
```
Норма — стаб сам делает full re-probe через 30 сек и находит правильный процесс. Если бесконечный цикл BAILOUT — оффсеты `AH_RVA_GWORLD` устарели, обновить через abidumper.

### 5.6 FPS падает / reader тормозит
Проверь что в build.bat стоит `/DAH_DIAG` только для диагностических билдов.
Prod без /DAH_DIAG работает быстрее (нет log-throttle overhead'а).

---

## 6. Rollback / Emergency

### Откатиться к прошлой версии
```
cd C:\arenaREmake\arenahack
git log --oneline -10
git checkout <хэш нужного коммита>
python scripts\make_release_zip.py --zero-key --version 1.0.19-rollback
```
Загрузить как новый релиз в админку с VERSION = `1.0.19-rollback`.

### Backup релиза перед перезаписью
```powershell
Copy-Item releases\WinRuntimeHost.zip "releases\WinRuntimeHost-v1.0.20-backup.zip"
```

---

## 7. Gamebreaker (ребренд) — параллельная процедура

Полностью аналогично, но:
- Директория: `C:\GamebreakerHack\`
- Отдельный product_id в KoenFlow админке (не `arena-breakout-esp`)
- Отдельный KFPL key печатается в его build'е (свой, не тот же что у arenahack)

Порядок работ:
1. Синкнуть код если менял arenahack:
   ```powershell
   Copy-Item C:\arenaREmake\arenahack\src\ah_reader_thread.cpp C:\GamebreakerHack\src\ah_reader_thread.cpp -Force
   Copy-Item C:\arenaREmake\arenahack\src\ah_reader_thread.h   C:\GamebreakerHack\src\ah_reader_thread.h   -Force
   Copy-Item C:\arenaREmake\arenahack\launcher\src\ah_launcher.c C:\GamebreakerHack\launcher\src\ah_launcher.c -Force
   Copy-Item C:\arenaREmake\arenahack\scripts\make_release_zip.py C:\GamebreakerHack\scripts\make_release_zip.py -Force
   ```
2. Пересобрать overlay (у него **свой UI** — esp_style.cpp, status_bar.cpp,
   freetype fonts — они не трогаются при синке):
   ```
   cd C:\GamebreakerHack
   .\build.bat
   ```
3. Пакет:
   ```
   python scripts\make_release_zip.py --zero-key --no-overlay-build --version 1.0.20
   ```
4. Загрузить с ЕГО KFPL key под свой product_id.

---

## 8. Чек-лист перед публикацией

- [ ] Все процессы `ah_overlay` / `WinRuntimeHost` убиты (elevated).
- [ ] `inc\ah_offsets.h` актуален (дата в шапке < 5 дней или последний
  ABI micropatch).
- [ ] Overlay пересобран (`build\ah_overlay.exe` LastWriteTime свежий).
- [ ] Собрано в `--zero-key` mode (см. stdout: "ZERO-KEY ship mode").
- [ ] VMP-log показал **все 4 маркера** (`resolve_key`, `aes_gcm_decrypt`,
  `extract_key_from_launch_context`, `antitheft_guard`).
- [ ] AHBE трейлер валиден (magic = `AHBE`, size совпадает).
- [ ] Smoke test прошёл: запуск без launcher'а → `key32 first bytes:
  00 00 00 00` → `aes_gcm_decrypt fail`.
- [ ] KFPL key (b64) скопирован в поле KFPL KEY админки.
- [ ] LOADER KEY / CONTENT KEY поля **пусты**.
- [ ] Activate now = ON.
- [ ] После деплоя — проверить на клиенте через launcher (не через
  прямой запуск!): `key32 first bytes` **не** нули, `decrypt OK`,
  child spawned, GWorld attached.

Готово.
