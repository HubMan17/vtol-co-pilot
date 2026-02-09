# Настройка параметров ArduPilot для GUIDED + GPS_INPUT + EKF Source Switching

## Обязательные параметры (установить ДО полёта)

### 1. GPS_TYPE2 = 14 (MAV)
Второй GPS-слот принимает данные по MAVLink (GPS_INPUT сообщение).
Через этот слот мы отправляем позицию от Dead Reckoning при потере основного GPS.

**ВАЖНО: требует РЕБУТ после изменения!**

```
GPS2_TYPE = 14
```

### 2. GPS_AUTO_SWITCH = 1
Автоматическое переключение на лучший GPS-источник.
```
GPS_AUTO_SWITCH = 1
```

### 3. GPS_PRIMARY = 0
Основной GPS = первый (аппаратный).
```
GPS_PRIMARY = 0
```

---

## Стратегия EKF Source Switching (SRC1 ↔ SRC3)

### Принцип работы

При потере GPS1 система переключает EKF с Source Set 1 (полный GPS) на Source Set 3
(только позиция от GPS_INPUT, скорость вычисляется EKF самостоятельно).

**Почему не просто GPS_AUTO_SWITCH?**
- GPS_INPUT подаёт позицию от нашего Dead Reckoning
- Если также подавать скорость, возникает конфликт двух независимых DR
  (наш DR считает скорость через airspeed+ветер, EKF — через IMU+GPS)
- VELXY=0 в SRC3 решает это: EKF берёт позицию от GPS_INPUT, но скорость
  вычисляет самостоятельно (IMU + airspeed + собственная модель ветра)

**Переключение через RC9 (RC_OPTION=90):**
- PWM ≤1200 → SRC1 (нормальный GPS)
- PWM >1800 → SRC3 (DR GPS_INPUT, без скорости)
- Управляется программно через RC_CHANNELS_OVERRIDE на канале 9

### Параметры EK3_SRC

```
# SRC1: нормальный GPS (полный набор)
EK3_SRC1_POSXY = 3   (GPS)
EK3_SRC1_POSZ  = 1   (Baro)
EK3_SRC1_VELXY = 3   (GPS)
EK3_SRC1_VELZ  = 0   (None — Baro-derived)
EK3_SRC1_YAW   = 1   (Compass)

# SRC3: DR через GPS_INPUT (только позиция, скорость — EKF сам)
EK3_SRC3_POSXY = 3   (GPS — наш GPS_INPUT)
EK3_SRC3_POSZ  = 1   (Baro)
EK3_SRC3_VELXY = 0   (None — EKF вычисляет сам!)
EK3_SRC3_VELZ  = 0   (None)
EK3_SRC3_YAW   = 1   (Compass)
```

**КРИТИЧЕСКИ ВАЖНЫЕ параметры:**

```
EK3_SRC_OPTIONS = 0   # ОБЯЗАТЕЛЬНО 0! Бит 0 = FuseAllVelocities — если 1,
                       # EKF игнорирует VELXY=0 и всё равно фузит скорость GPS.
                       # Это полностью ломает стратегию.

EK3_SRC1_VELZ = 0     # None вместо 3 (GPS). При переключении SRC1→SRC3
                       # VELZ меняется с 3→0 — если оставить 3 в SRC1,
                       # EKF получит "скачок" при переключении.
```

### RC9 — канал переключения EKF Source

```
RC9_OPTION = 90        # EKF Pos Source switching
```

Co-pilot автоматически управляет RC9 через RC_CHANNELS_OVERRIDE:
- GPS OK → RC9=1100 (SRC1)
- GPS потерян (5+ циклов) → RC9=1900 (SRC3)
- RC override re-send каждую ~1с (RC_OVERRIDE_TIME=3с)

---

## Параметры GPS_INPUT (accuracy/speed_accuracy)

### Пороги здоровья EKF (`calcGpsGoodToAlign`)

EKF проверяет GPS здоровье:
- `horiz_accuracy > MAX(EK3_POSNE_M_NSE, 5.0)` → GPS "unhealthy"
- `speed_accuracy > MAX(EK3_VELNE_M_NSE*2, 1.0)` → GPS "unhealthy"

Если хоть один порог превышен — **ВСЕ данные GPS_INPUT игнорируются**.

### Значения в коде

| Состояние | horiz_accuracy | speed_accuracy | Смысл |
|-----------|---------------|----------------|-------|
| Warmup (GPS OK) | 3.0 | 0.8 | Ниже порогов, но выше GPS1 (0.3м) — GPS1 доминирует |
| DR (GPS lost) | 1.0 | 0.5 | Низкий = высокий Kalman gain → EKF доверяет DR |

**Почему фиксированные значения:**
- Растущая accuracy была ОШИБКОЙ: EKF снижал доверие → позиции расходились → RED EKF
- С фиксированным accuracy=1.0 и VELXY=0 нет конфликта скоростей

---

## Параметры Q_ASSIST (защита от срабатывания коптерных моторов)

Текущие значения:
```
Q_ASSIST_SPEED   = 18    (м/с)
Q_ASSIST_ANGLE   = 30    (градусы)
Q_ASSIST_DELAY   = 0.5   (секунды)
```

При переключении SRC1→SRC3 скорость может кратковременно измениться.
Если Q_ASSIST срабатывает ложно:
- Увеличить `Q_ASSIST_DELAY = 1.5`
- Или уменьшить `Q_ASSIST_SPEED = 12`

---

## Тумблер GPS (RC8) — защита от спуфинга

**ВАЖНО: RC8_OPTION НЕ должен быть = 65!**

`RC_OPTION=65` (GPS_DISABLE) отключает **ВСЕ** GPS на уровне AP_GPS,
включая GPS_INPUT. Это делает DR полностью неработоспособным.

```
RC8_OPTION = 0          # НЕ 65! Co-pilot сам обрабатывает тумблер
```

**RC8 HIGH — пилот отключает GPS1:**
1. Co-pilot устанавливает `SIM_GPS_DISABLE=1` (SITL) — останавливает симулированный GPS1
2. Co-pilot переключает EKF на SRC3 (`set_ekf_source(3)`)
3. DR перестаёт привязываться к GPS позиции, начинает счисление
4. GPS_INPUT шлёт DR позицию → EKF использует её (позиция), скорость считает сам

**RC8 LOW — возврат к нормальной работе:**
1. `SIM_GPS_DISABLE=0` — восстанавливает симулированный GPS1
2. Co-pilot переключает EKF на SRC1 (`set_ekf_source(1)`)
3. ArduPilot делает `ResetPosition()` — мгновенный телепорт к реальному GPS (безопасно)

**Автоматическая потеря GPS (без тумблера):**
- Система определяет gps_fix < 3 по GPS_RAW_INT (GPS1 only)
- После 5 циклов потери → автоматическое переключение на SRC3
- При возврате GPS → автоматическое переключение на SRC1

---

## Как загрузить параметры

### В SITL (Mission Planner):
1. Подключиться к SITL
2. Config/Tuning -> Full Parameter List
3. Загрузить файл `default.param` через "Load from file"
4. Нажать "Write Params"
5. **Перезагрузить SITL** (GPS2_TYPE требует ребут)

### В SITL (MAVProxy):
```
param load default.param
reboot
```

---

## Чеклист перед тестированием

- [ ] `GPS2_TYPE = 14` установлен и ребут выполнен
- [ ] `GPS_AUTO_SWITCH = 1`
- [ ] `EK3_SRC_OPTIONS = 0` (**НЕ 1!** FuseAllVelocities ломает VELXY=0)
- [ ] `EK3_SRC1_VELZ = 0` (None, не 3/GPS)
- [ ] `EK3_SRC3_POSXY = 3` (GPS)
- [ ] `EK3_SRC3_VELXY = 0` (None — ключевой параметр)
- [ ] `EK3_SRC3_YAW = 1` (Compass)
- [ ] `RC9_OPTION = 90` (EKF Pos Source)
- [ ] `RC8_OPTION = 0` (**НЕ 65!**)
- [ ] `SIM_GPS2_DISABLE = 1` (для SITL)
- [ ] SITL/полётник перезагружен после установки GPS2_TYPE
- [ ] Q_ASSIST_SPEED адекватный (18 или ниже если срабатывает ложно)

## Проверка работы

1. **Прогрев**: Запустить, подождать 30с → `GPS[1].HAcc=3.0, sAcc=0.8` в логе
2. **RC8 toggle**: Щёлкнуть → лог "EKF SOURCE SET: 3", полёт продолжается без RED EKF
3. **Коррекция позиции**: Кликнуть на карте → GPS_INPUT плавно сдвигается (не прыжок)
4. **Возврат GPS**: RC8 LOW → `ResetPosition()` телепортирует к GPS1, лог "EKF SOURCE SET: 1"
