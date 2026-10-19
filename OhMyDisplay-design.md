# OhMyDisplay - 스펙 및 설계 문서

| 항목 | 내용 |
|---|---|
| 문서 버전 | v0.2 (Draft, 결정사항 반영) |
| 상태 | 설계 확정 -> 구현 착수 단계 |
| 대상 OS | Windows 10 21H2+ / Windows 11 (x64, ARM64는 v2) |
| 언어/런타임 | C++17/20, .NET 비의존, 단일 포터블 exe |
| 범위 | Layout Keeper(상주) + Sleep Doctor(온디맨드) + 공유 Core + 트레이 셸 |
| 저장소 | JSON (확정) |

---

## 0. 결정 로그 (Resolved decisions)

이전 초안의 미해결 질문(§19)에 대한 확정 사항.

| # | 질문 | 결정 |
|---|---|---|
| 1 | 저장소 JSON vs SQLite | **JSON 확정.** 이력/드레인 추이 질의가 커지면 v2에서 SQLite 승격 검토 |
| 2 | 동일 모델 다중 모니터 수동 라벨 UI를 v0에 넣을지 | **v0 포함 확정.** 사용자 라벨링이 Identity 충돌의 1차 해결책 |
| 3 | 디바운스 기본값 고정 vs 적응형 | **적응형 확정.** 모니터 수에 비례하는 적응형 디바운스 |
| 4 | Doctor 상승을 메인 통합 vs 분리 헬퍼 | **분리 헬퍼 확정.** 메인은 asInvoker 유지, 상승 작업은 별도 헬퍼 프로세스 |
| 5 | 작업명 | **OhMyDisplay 확정** |

---

## 1. 개요 (Overview)

OhMyDisplay는 Windows의 **전원/디스플레이 전이로 인한 두 가지 만성 불편**을 하나의 도구로 해결한다.

1. **창 배치 붕괴(Layout collapse):** 모니터가 절전/입력 전환으로 핸드셰이크를 잃으면 모든 창이 한 화면으로 몰리고 크기까지 변형됨.
2. **절전 불안정(Sleep insomnia / phantom wake):** PC가 잠들지 않거나 멋대로 깨어나는데, `powercfg`가 CLI에 파편화돼 있어 일반 사용자가 원인을 못 짚음.

두 문제는 **동일한 신호원**(전원 상태 전이 + 디스플레이 토폴로지 변화 + Power 이벤트 로그)에서 출발하므로, 단일 Core 이벤트 백본 위에 두 모듈을 얹는다.

## 2. 목표 / 비목표

### 2.1 목표 (Goals)

- G1. 디스플레이 토폴로지별 창 배치를 자동 저장하고, 붕괴 후 신뢰성 있게 복원한다.
- G2. "왜 안 자는가 / 왜 깼는가"를 평문 리포트 1장과 원클릭 조치로 제공한다.
- G3. 의존성 0(.NET, 런타임 미요구)에 가까운 가벼운 포터블 exe로 배포한다.
- G4. 두 모듈이 Core를 공유해 코드/이벤트/저장소를 재사용한다.

### 2.2 비목표 (Non-goals)

- N1. 창 타일링/스냅 레이아웃 관리(FancyZones 영역) - 범위 외.
- N2. 전원 프로파일의 전면 대체(제어판 수준 편집) - 진단/조치에 한정.
- N3. 멀티유저/엔터프라이즈 정책 배포 - v2 이후.
- N4. 커널 드라이버 설치 요구 - MVP는 전부 유저모드. (IDD 가상 디스플레이는 v2 옵션)

## 3. 배경 및 문제 정의

### 3.1 Layout collapse의 근본 원인

DisplayPort/HDMI 모니터가 절전에 들어가면 링크 핸드셰이크가 끊겨 OS는 모니터가 **물리적으로 제거된 것**으로 인식한다(Rapid Hot Plug Detect). 토폴로지가 일시적으로 1개로 줄면서:

- 다른 모니터에 있던 창이 살아있는 모니터로 이동.
- 대상 모니터 해상도가 더 작으면 창 크기까지 축소.
- 깨어날 때 모니터가 비동기적으로 복귀해 일부만 되돌아오는 부분 복원 발생.

핵심 난점은 **모니터를 안정적으로 식별**하는 것과 **토폴로지가 안정화될 때까지의 디바운싱**이다. 단순 인덱스(모니터 1/2)는 재배열되므로 신뢰 불가.

### 3.2 Sleep 진단의 파편화

원인 신호가 여러 곳에 흩어져 있다.

- `powercfg /requests` - 절전을 막는 활성 요청(앱/드라이버).
- `powercfg /lastwake` - 마지막 기상 원인(단, ACPI wake source count가 0이면 공란).
- `powercfg /waketimers` - 예약된 기상 타이머.
- `powercfg /devicequery wake_armed` - 기상 가능 장치.
- `powercfg /sleepstudy` - Modern Standby의 DRIPS/residency(절전 체류율).
- Event Log: `Kernel-Power`, `Power-Troubleshooter`(기상 원인 보강).

이를 하나로 모아 상관 분석하고 평문화하는 소비자 도구가 없다.

## 4. 용어 (Glossary)

| 용어 | 정의 |
|---|---|
| Topology | 현재 연결/활성 모니터의 집합과 배치 |
| Monitor Identity | 모니터를 토폴로지 변화에도 안정적으로 식별하는 키(EDID 기반) |
| Monitor Label | 사용자가 부여한 친화적 이름(Identity 충돌 해소용) |
| Layout Snapshot | 특정 토폴로지에서의 모든 창 위치/상태 기록 |
| Collapse | 토폴로지 축소로 창이 한 화면에 몰리는 현상 |
| Restore | 저장된 Layout Snapshot을 현재 토폴로지에 재적용 |
| DRIPS | Deepest Runtime Idle Platform State, Modern Standby 저전력 체류 |
| Power Request | 시스템/디스플레이가 잠들지 못하게 하는 요청 |

## 5. 사용자 시나리오 (Use cases)

- **UC1 (도킹/언도킹):** 노트북을 듀얼 모니터 도크에 붙였다 뗀다. 각 토폴로지의 창 배치가 자동 저장/복원된다.
- **UC2 (모니터 절전 복귀):** 자리를 비웠다 오니 모니터가 깨어났는데 창이 안 튄다(또는 즉시 복원).
- **UC3 (해상도 변경 게임):** 전체화면 게임 종료 후 데스크톱 창들이 제자리로.
- **UC4 (안 자는 PC):** "절전 진단" 클릭 -> "Spotify가 시스템 절전을 막고 있음 -> 여기서 해제" 평문 안내.
- **UC5 (밤마다 깨는 PC):** "마지막 기상: Realtek NIC의 Wake-on-LAN -> 이 장치 기상 해제" 원클릭.
- **UC6 (동일 모델 2대):** 같은 모델 모니터 2대를 "왼쪽/오른쪽"으로 직접 라벨링해 정확히 구분한다.

## 6. 요구사항

### 6.1 기능 요구사항 (Functional)

**Layout Keeper**
- FR-A1. 토폴로지를 모니터 Identity 집합으로 식별한다.
- FR-A2. 토폴로지별 Layout Snapshot을 주기/이벤트 기반으로 자동 저장한다.
- FR-A3. 토폴로지 변경 감지 시 **적응형 디바운스** 후 매칭 Snapshot을 복원한다.
- FR-A4. 핫키로 "지금 저장 / 지금 복원"을 제공한다.
- FR-A5. 유령(비활성 경로) 모니터 등록을 탐지/정리 제안한다.
- FR-A6. 특정 앱/창을 복원 대상에서 제외하는 규칙을 지원한다.
- FR-A7. **(v0) 모니터 수동 라벨 UI**: 사용자가 각 모니터에 친화적 이름을 부여하고, 동일 모델 충돌을 직접 해소한다.

**Sleep Doctor**
- FR-B1. `powercfg`의 requests/lastwake/waketimers/devicequery 결과를 수집/파싱한다.
- FR-B2. SleepStudy(DRIPS/residency)를 수집/요약한다(v1).
- FR-B3. Power 관련 이벤트 로그를 타임라인으로 상관한다.
- FR-B4. "절전 차단 범인 / 기상 범인"을 평문 + 권장 조치로 제시한다.
- FR-B5. 안전한 조치(장치 기상 해제, 요청 override)를 **분리 헬퍼**를 통해 원클릭 실행한다.

**공통**
- FR-C1. 트레이 상주 + 설정/대시보드 창.
- FR-C2. 모든 데이터는 로컬에만 저장(네트워크 송신 없음).

### 6.2 비기능 요구사항 (Non-functional)

- NFR1. 유휴 시 CPU ~0%, 상주 메모리 < 30MB 목표.
- NFR2. Layout 복원 지연 < 토폴로지 안정화 + 적응형 디바운스 + 300ms.
- NFR3. 단일 exe, 설치 불요(포터블). 설정은 `%LOCALAPPDATA%\OhMyDisplay\`.
- NFR4. 크래시/예외가 사용자 창/전원 상태를 손상시키지 않음(읽기 우선, 쓰기는 검증 후).
- NFR5. 코드 서명된 릴리스 바이너리.

## 7. 아키텍처 개요

```
+-------------------------------------------------------------+
|                       Tray Shell (Win32)                    |
|     - tray icon, hotkeys, settings/dashboard, label UI      |
+----------------------------+--------------------------------+
                             |
            +----------------+-----------------+
            |                                  |
+-----------v-----------+        +-------------v-------------+
|   Module A: Keeper     |        |   Module B: Sleep Doctor  |
|  - topology identity   |        |  - powercfg collectors    |
|  - monitor labels      |        |  - sleepstudy parser (v1) |
|  - window enum/capture |        |  - event log correlator   |
|  - adaptive debounce   |        |  - report + actions       |
|  - save / restore      |        |    (via elevated helper)  |
|  - collapse guard      |        |                           |
+-----------+-----------+        +-------------+-------------+
            |                                  |
            +----------------+-----------------+
                             |
+----------------------------v--------------------------------+
|                          Core                               |
|  EventBus | PowerNotify | DisplayNotify(CCD) | EventLog     |
|  Store(JSON) | Logger | ElevatedHelper IPC | Scheduler      |
+-------------------------------------------------------------+
```

설계 원칙: Core는 **신호를 수집/정규화/발행**만 한다. 두 모듈은 EventBus를 구독해 각자 반응한다. 모듈 간 직접 의존 없음.

상승 권한 처리: 메인 프로세스는 `asInvoker`로 비상승 실행. 상승이 필요한 작업(일부 powercfg 명령, 장치 기상 해제, 상승 창 조작)은 **분리 헬퍼 `OhMyDisplay.Elevated.exe`** 를 필요 시점에만 띄워 수행하고, 경량 IPC(named pipe)로 결과를 회수한다.

## 8. 컴포넌트 상세 설계

### 8.1 Core - Event Backbone

수집 신호와 사용 API.

| 신호 | API | 비고 |
|---|---|---|
| 절전/복귀(suspend/resume) | `PowerRegisterSuspendResumeNotification` | 콜백 기반 |
| 디스플레이 전원 on/off | `RegisterPowerSettingNotification(GUID_CONSOLE_DISPLAY_STATE)` | 메시지 윈도우 필요 |
| 모니터 전원 | `GUID_MONITOR_POWER_ON` | 레거시 보강 |
| 디스플레이 토폴로지 변경 | `WM_DISPLAYCHANGE` + CCD `QueryDisplayConfig` | 핵심 트리거 |
| 모니터 식별/EDID | `DisplayConfigGetDeviceInfo(GET_TARGET_NAME)` | Identity 산출 |
| 전원 진단(Doctor) | `powercfg` 서브프로세스 | 출력 파싱 |
| Power 이벤트 | `wevtapi`(`EvtQuery`) | System 로그 필터 |

메시지 펌프: Core는 보이지 않는 메시지 전용 윈도우(`HWND_MESSAGE`)를 하나 만들어 `WM_DISPLAYCHANGE`, `WM_POWERBROADCAST`, `WM_SETTINGCHANGE`를 받는다.

```cpp
// Core message window proc skeleton, single-exit style
LRESULT CALLBACK CoreWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT result = 0;
    do
    {
        if (msg == WM_DISPLAYCHANGE)
        {
            // topology may have changed; debounce then re-evaluate
            g_eventBus->Post(CoreEvent::DisplayChanged);
            result = 0;
            break;
        }
        if (msg == WM_POWERBROADCAST)
        {
            // PBT_APMSUSPEND / PBT_APMRESUMEAUTOMATIC / display state via setting notify
            g_eventBus->Post(CoreEvent::PowerTransition, static_cast<int>(wp));
            result = TRUE;
            break;
        }
        result = DefWindowProcW(hWnd, msg, wp, lp);
    } while (false);
    return result;
}
```

EventBus: 단일 프로세스 내 경량 pub/sub. 발행은 UI 스레드, 무거운 작업은 워커 스레드 디스패치(§10 참조).

### 8.2 Module A - Layout Keeper

#### 8.2.1 Monitor Identity 산출

인덱스는 불안정하므로 EDID 기반 안정 키를 만든다.

- CCD로 활성 path를 열거 -> 각 target의 `monitorDevicePath`(컨테이너 기반, 비교적 안정) 확보.
- `DISPLAYCONFIG_TARGET_DEVICE_NAME`에서 제조사ID + 제품코드, 가능하면 EDID serial.
- Identity = `hash(manufacturerId + productCode + serial + monitorDevicePath)`.
- serial이 없는 패널(동일 모델 2대)은 `monitorDevicePath`로 분기. 그래도 충돌하면 물리 connector(adapterId+sourceId)로 tie-break.
- 최종 방어선은 **사용자 라벨**(FR-A7): 충돌이 감지되면 라벨 UI로 사용자에게 구분을 요청한다.

#### 8.2.2 Topology Signature

```
TopologySignature = sorted_set( MonitorIdentity[] )
```

배치(좌표)는 시그니처에 **넣지 않는다.** "같은 모니터 집합이면 같은 토폴로지"로 보고, 그 안에서 배치는 복원 대상 데이터로 다룬다. 이렇게 해야 절전->복귀처럼 배치가 망가져도 동일 토폴로지로 매칭된다.

#### 8.2.3 모니터 수동 라벨 (v0)

- 라벨 UI는 현재 감지된 모니터를 나열하고 각각에 친화적 이름("왼쪽 27", "세로 모니터" 등)을 부여한다.
- 라벨은 Identity에 1:1로 바인딩되어 저장(`labels` 맵).
- **충돌 자동 감지:** Identity 산출이 동일 키를 2개 이상 만들면, 트레이 알림으로 "동일 모델 2대가 감지됨, 구분 라벨을 지정하세요"를 띄우고 라벨 UI를 연다.
- 라벨이 지정되면 해당 모니터의 Identity에 사용자 지정 discriminator를 추가해 키를 분리한다.
- 라벨은 복원 로그/리포트의 사람 친화적 표기에도 사용된다.

#### 8.2.4 창 캡처(Snapshot)

- `EnumWindows`로 top-level 순회 -> 필터는 작업 표시줄 규칙을 따른다(exe 목록이 아님):
  - visible, 크기 0이 아닌 창(최소화 포함).
  - `WS_EX_APPWINDOW`가 있으면 tool/owned/no-activate여도 포함(작업 표시줄 강제 노출).
  - 그 외에는 `WS_EX_TOOLWINDOW`/`WS_EX_NOACTIVATE` 제외, ALT-TAB owner walk 승자만 포함.
  - 클로킹: 앱 클로킹은 제외. 셸 클로킹은 `IVirtualDesktopManager`가 다른 가상 데스크톱에
    있다고 보고할 때만 포함(일시 중단된 UWP 프레임은 제외).
  - 셸 표면(Progman, 작업 표시줄)은 스타일로 걸러지므로 `explorer.exe`를 exe로 제외하지 않는다
    (파일 탐색기 창 누락 방지).
- 스냅(arranged) 창은 `rcNormalPosition`이 스냅 이전 rect이므로 실제 창 rect로 캡처하고, 복원 시
  여전히 스냅 상태면 `SetWindowPos`로 명시 이동한다.
- 각 창: `GetWindowPlacement`(WINDOWPLACEMENT: normal/min/max + restore rect), 소속 모니터 Identity(`MonitorFromWindow` -> CCD 매핑), 프로세스 경로(`QueryFullProcessImageNameW`), 가능하면 명령줄(실패 허용).
- 좌표는 **소속 모니터 work area 기준 상대 좌표**로 정규화 저장(해상도 변화 대응).

#### 8.2.5 적응형 디바운스 (Adaptive debounce)

토폴로지 변경 burst를 흡수하는 대기 시간을 **모니터 수에 비례**해 동적으로 정한다. 모니터가 많을수록 비동기 복귀가 더 오래 걸리기 때문이다.

```
debounceMs = clamp(baseMs + perMonitorMs * activeMonitorCount, minMs, maxMs)

기본값:
  baseMs       = 300
  perMonitorMs = 250
  minMs        = 400
  maxMs        = 1500

예) 모니터 1대 -> clamp(300+250, 400, 1500) = 550ms
    모니터 2대 -> 300+500 = 800ms
    모니터 3대 -> 300+750 = 1050ms
    모니터 5대 -> 300+1250 = 1550 -> 1500ms (상한)
```

추가로, 디바운스 창 안에서 새 `DisplayChanged`가 들어오면 타이머를 리셋(슬라이딩 윈도우)해 복귀가 완전히 멈춘 뒤에만 복원이 트리거되게 한다. 모든 파라미터는 설정에서 조정 가능.

```cpp
// Adaptive debounce window computation, single-exit style
DWORD ComputeDebounceMs(const DebounceConfig& cfg, int activeMonitorCount)
{
    DWORD value = 0;
    do
    {
        long raw = static_cast<long>(cfg.baseMs)
                 + static_cast<long>(cfg.perMonitorMs) * activeMonitorCount;
        if (raw < static_cast<long>(cfg.minMs))
        {
            value = cfg.minMs;
            break;
        }
        if (raw > static_cast<long>(cfg.maxMs))
        {
            value = cfg.maxMs;
            break;
        }
        value = static_cast<DWORD>(raw);
    } while (false);
    return value;
}
```

#### 8.2.6 복원(Restore) 시퀀스

```
on DisplayChanged or PowerResume:
  1. compute adaptive debounce from current active monitor count
  2. wait debounce window (sliding; reset on new DisplayChanged)
  3. read current topology -> TopologySignature S
  4. if no snapshot for S -> capture-and-store, return
  5. for each saved window w in snapshot(S):
        a. find live HWND (by pid+class+title heuristic; or tracked handle if alive)
        b. resolve target monitor rect via w.monitorIdentity -> current HMONITOR
        c. translate normalized coords -> absolute rect
        d. SetWindowPlacement(restore)
  6. verify pass after 200ms; retry mismatched windows up to N times
```

핵심 디테일:
- **적응형 디바운스**가 안정성의 절반. 모니터가 순차 복귀하는 동안 즉시 복원하면 다시 튄다.
- **검증 재시도**: 일부 앱은 자체적으로 위치를 되돌리므로 1회 복원으로 안 되는 경우가 있음.
- 창 재식별은 핸들이 살아 있으면 핸들 우선, 죽었으면 클래스+타이틀+exe로 best-effort 매칭.
  exe/클래스/타이틀이 모두 같은 창(예: 스티커 메모 여러 장)은 같은 모니터에서 현재 위치가 가장
  가까운 창끼리 짝짓고, 그래도 같으면 캡처 순서를 따른다.
- **불완전 토폴로지 무시**: 절전 복귀/핫플러그 중에는 일부 모니터의 CCD device path가 비어
  Identity가 GDI 이름으로 대체되므로 잘못된 시그니처가 생긴다. 이런 토폴로지는 저장하지 않고
  몇 차례 재시도한 뒤에만 그대로 받아들인다.
- 디바운스 창은 마지막으로 안정된 모니터 수 기준으로 잡고, 대기 중인 마감 시각을 앞당기지 않는다
  (3 -> 1 -> 3 복귀 중 1대 시점에 조기 발화 방지).

#### 8.2.7 Collapse Guard (붕괴 차단)

- **MVP(복원형):** 위 Restore 시퀀스로 "튄 뒤 즉시 되돌림". 구현 단순, 의존성 0.
- **v2 옵션(차단형):** Indirect Display Driver(IDD)로 항상 존재하는 가상 모니터를 1개 유지 -> 물리 모니터가 절전돼도 토폴로지가 1개로 붕괴하지 않게 함. 드라이버 설치/서명 필요라 별도 트랙.

#### 8.2.8 UIPI/권한

비상승 프로세스는 상승 창에 `SetWindowPlacement` 불가(UIPI). 모든 창을 관리하려면 상승이 필요하다. 기본은 비상승으로 두고, "상승 권한으로 모든 창 관리" 옵션 시 분리 헬퍼가 해당 창 조작을 대행한다.

### 8.3 Module B - Sleep Doctor

#### 8.3.1 Collector

`powercfg`를 서브프로세스로 실행, stdout 캡처. 관리자 권한이 필요한 명령은 **분리 헬퍼**를 통해 실행한다. 파서는 정규식 대신 라인 토크나이저(섹션 헤더 기반)로 견고하게.

| 명령 | 산출 | 권한 |
|---|---|---|
| `powercfg /requests` | DISPLAY/SYSTEM/AWAYMODE/EXECUTION별 차단 주체 | 관리자(헬퍼) |
| `powercfg /lastwake` | 마지막 기상 원인 | 일반 |
| `powercfg /waketimers` | 예약 기상 타이머 | 관리자(헬퍼) |
| `powercfg /devicequery wake_armed` | 기상 가능 장치 | 일반 |
| `powercfg /sleepstudy /output <html>` | DRIPS/residency, 세션별 드레인(v1) | 관리자(헬퍼) |
| `powercfg /systemsleepdiagnostics /xml` | 절전 방해 상세(v1) | 관리자(헬퍼) |

#### 8.3.2 Event Log Correlator

`wevtapi`로 System 로그를 XPath 필터링.

- `Kernel-Power` 42(절전 진입), 107(복귀), 41(비정상 종료) 등.
- `Power-Troubleshooter` ID 1(기상 시각/원인) - `/lastwake`가 공란일 때 보강.
- 결과를 시간순 타임라인으로 병합 -> "23:14 절전 -> 23:51 NIC 기상" 식 서사.

#### 8.3.3 진단 룰 엔진 (규칙 -> 평문 + 조치)

| 증상 | 신호 | 평문 | 조치 |
|---|---|---|---|
| 안 잠 | `/requests`에 [PROCESS] X | "X가 시스템 절전을 막는 중" | 해당 요청 override 또는 앱 안내 |
| 안 잠 | `/requests`에 [DRIVER] audio | "오디오 드라이버 활성 스트림" | 해당 앱 종료 안내 |
| 자꾸 깸 | lastwake = device NIC | "Wake-on-LAN/매직패킷" | `powercfg /devicedisablewake "<NIC>"` |
| 자꾸 깸 | waketimers = 작업스케줄러 | "예약 작업이 기상 요청" | 해당 작업 wake 권한 해제 안내 |
| 배터리 드레인(MS) | sleepstudy DRIPS 낮음 | "Modern Standby 저전력 진입 실패" | 방해 컴포넌트 표시 |

조치 실행은 **안전 목록만** 분리 헬퍼로 자동화(장치 기상 해제, requests override). 파괴적/광범위 변경은 안내만.

### 8.4 Tray Shell & UI

- 트레이 아이콘 + 컨텍스트 메뉴(저장/복원/진단/라벨/설정/종료) + 전역 핫키(`RegisterHotKey`).
- Keeper 설정창: 토폴로지 목록, **모니터 라벨 편집(v0)**, 제외 규칙, 디바운스 파라미터(base/perMonitor/min/max), 재시도 횟수.
- Doctor 패널: 리포트 표가 많아 **WebView2 렌더링 권장**. 미설치 환경 대비 평문 폴백.
  - 트레이드오프: WebView2(Evergreen)는 대개 사전 설치돼 있고 표 가독성이 우수. 의존성 0 원칙과 충돌하나, Doctor의 가치가 리포트 가독성에 있으므로 가용 시 WebView2, 불가 시 정적 HTML/텍스트로 폴백.

## 9. 데이터 모델 / 스키마

저장소: **JSON 파일 확정.** 경로 `%LOCALAPPDATA%\OhMyDisplay\`.

```jsonc
// layouts.json
{
  "schemaVersion": 2,
  "labels": {
    // monitorIdentity -> user friendly label (v0)
    "m_8f2a...": "Left 27",
    "m_91c4...": "Right 27"
  },
  "topologies": [
    {
      "signature": "a1b2c3...",            // hash of sorted monitor identities
      "monitors": [
        { "id": "m_8f2a...", "label": "Left 27",
          "edid": { "mfg": "DEL", "product": 16700, "serial": "ABC123" },
          "devicePath": "\\\\?\\DISPLAY#DELA0FF#..." }
      ],
      "lastSeenUtc": "2026-06-14T11:02:33Z",
      "windows": [
        {
          "exe": "C:\\...\\Code.exe",
          "title": "design.md - VS Code",
          "monitorId": "m_8f2a...",
          "placement": { "showCmd": "normal",
                         "normRect": { "x": 0.10, "y": 0.05, "w": 0.55, "h": 0.80 } },
          "match": { "class": "Chrome_WidgetWin_1" }
        }
      ]
    }
  ]
}
```

```jsonc
// settings.json
{
  "keeper": {
    "enabled": true,
    "debounce": { "baseMs": 300, "perMonitorMs": 250, "minMs": 400, "maxMs": 1500 },
    "restoreRetries": 3,
    "elevateForAllWindows": false,
    "excludeExe": [],              // shell surfaces are filtered by window style, not exe
    "excludeTitleRegex": []
  },
  "doctor": { "useWebView2": true, "collectSleepStudy": false },
  "hotkeys": { "saveNow": "Ctrl+Alt+S", "restoreNow": "Ctrl+Alt+R" }
}
```

쓰기 안전성: 모든 JSON 쓰기는 temp 파일에 기록 후 원자적 rename(`ReplaceFileW`/`MoveFileEx`)으로 교체해 부분 손상을 방지한다. 스키마 버전 불일치 시 마이그레이션 또는 백업 후 재생성.

## 10. 동시성 / 스레딩 모델

- **UI/메시지 스레드(메인):** 메시지 펌프, 트레이, 핫키, 이벤트 수신. 블로킹 작업 금지.
- **워커 스레드(Keeper):** 적응형 디바운스 타이머 만료 시 토폴로지 평가/캡처/복원 수행.
- **워커 스레드(Doctor):** `powercfg` 서브프로세스/로그 질의(수백 ms~수 초) 수행, 완료 시 결과를 UI로 마샬링.
- **헬퍼 프로세스(Elevated):** 상승 작업 전담, named pipe IPC로 통신, 작업 단위로 기동/종료.
- 공유 상태(현 토폴로지, 스냅샷 캐시)는 단일 mutex 보호. 락 구간 최소화.
- 디바운스는 워커에서 `WaitForSingleObject(timeout)` + 이벤트 리셋(슬라이딩) 패턴으로 burst 흡수.

## 11. 에러 처리 / 엣지 케이스 / 호환성

| 케이스 | 처리 |
|---|---|
| EDID serial 없음(동일 모델 2대) | devicePath/connector tie-break -> 최종 사용자 라벨 |
| 모니터 비동기 복귀 | 적응형 디바운스(슬라이딩) + 검증 재시도 |
| 앱이 자체적으로 위치 되돌림 | 재시도 N회 후 포기, 로그 기록 |
| 상승 창 조작 실패(UIPI) | 헬퍼로 위임, 불가 시 스킵 + 안내 |
| `/lastwake` 공란 | Event Log Power-Troubleshooter로 보강 |
| Modern Standby 미지원 기기 | sleepstudy 비활성, S3 경로만 표시 |
| 가상 데스크톱 다중 | 창의 데스크톱 소속 보존(Virtual Desktop API, v1) |
| DPI 혼합 | 정규화 좌표 + per-monitor DPI 인지로 위치 보존 |
| `powercfg` 부재/차단(정책) | 해당 섹션 graceful degrade |
| JSON 손상/부분 기록 | 원자적 rename + 백업 복구 |

호환성 노트: per-monitor DPI awareness v2 매니페스트 필수. CCD는 Win10 1607+ 안정.

## 12. 보안 / 권한 / 프라이버시

- **로컬 전용:** 어떤 데이터도 네트워크로 보내지 않음. 텔레메트리 없음.
- **권한:** 메인은 `asInvoker`. 상승 필요 작업은 분리 헬퍼 `OhMyDisplay.Elevated.exe`가 필요 시점에만 수행(UAC 마찰 최소화). IPC는 로컬 named pipe, 호출자 검증.
- **저장 데이터 민감도:** 창 제목/exe 경로 포함 -> 로컬 파일 권한으로 보호, 평문 JSON. 민감 제목 마스킹 옵션 고려.
- **조치 안전성:** 자동 실행은 가역적/국소적 변경만(장치 기상 해제 등). 광범위 전원 정책 변경은 사용자 확인.

## 13. 빌드 / 배포

- 툴체인: MSVC, C++20, `/W4`, `/permissive-`, x64 우선.
- 의존성: Win32/WinRT(헤더 only) + (옵션)WebView2 SDK. 외부 런타임 미요구.
- 산출물: `OhMyDisplay.exe` + `OhMyDisplay.Elevated.exe`(상승 헬퍼). 포터블.
- 서명: 릴리스 바이너리 Authenticode 서명(양 exe 모두).
- 매니페스트: per-monitor DPI v2, longPathAware, 메인은 asInvoker / 헬퍼는 requireAdministrator.

## 14. 코드 스타일 / 규약

- ASCII 전용 주석/로그.
- `{`는 항상 다음 줄, `}`는 단독 줄. `if/else/for/while` 항상 중괄호.
- `else`는 단독 줄 후 다음 줄에 `{`.
- 다중 정리 경로는 `do { ... } while (false)` 단일 출구 선호.
- 줄 위에 실행문/중괄호 동거 금지.

## 15. 로깅 / 진단

- 회전 로그 `%LOCALAPPDATA%\OhMyDisplay\logs\`(ASCII, 레벨: ERROR/WARN/INFO/DEBUG).
- Keeper: 토폴로지 전이, 적응형 디바운스 값, 복원 결과(성공/재시도/스킵), 라벨 충돌 기록.
- Doctor: 수집 명령/종료코드/파싱 경고, 헬퍼 IPC 결과 기록.
- "진단 번들 내보내기"로 로그 + 익명화 스냅샷 zip 생성(지원용).

## 16. 테스트 전략

- **단위:** TopologySignature 산출, 적응형 디바운스 계산(경계값), 좌표 정규화/역변환, powercfg 파서(고정 샘플 fixture), 라벨 충돌 해소.
- **통합:** 가상 디스플레이/해상도 변경 스크립트로 collapse 재현 -> 복원 검증.
- **상호운용:** Chrome, VS Code, Electron류, 전체화면 게임 등 까다로운 앱군 회귀 세트.
- **전원:** S3/Modern Standby 양 경로에서 lastwake/requests 파싱 검증, 헬퍼 상승 경로 검증.
- **장시간:** 24h 상주 누수/핸들 릭 점검.

## 17. 마일스톤 / 로드맵

**v0 (MVP)**
- Keeper: 토폴로지 식별 + 자동 저장 + **적응형 디바운스 복원** + 저장/복원 핫키 + 유령 모니터 탐지 + **모니터 수동 라벨 UI**.
- Doctor: requests/lastwake/waketimers/devicequery 수집 + Power-Troubleshooter 상관 + 평문 리포트 1장.
- 트레이 셸, **JSON 저장**, 분리 상승 헬퍼, 로깅.

**v1**
- SleepStudy/systemsleepdiagnostics 파싱(DRIPS/드레인).
- 안전 조치 원클릭(장치 기상 해제, requests override) 정식화.
- 가상 데스크톱 소속 보존, 제외 규칙 UI, WebView2 리포트.

**v2**
- IDD 가상 디스플레이 기반 collapse 차단(옵션, 드라이버 트랙).
- 배터리 헬스 모듈 합류(시스템 헬스 스위트 확장).
- ARM64, 다국어, (필요 시)JSON -> SQLite 승격.

## 18. 리스크 및 완화

| 리스크 | 영향 | 완화 |
|---|---|---|
| 모니터 식별 충돌(동일 모델) | 잘못된 토폴로지 매칭 | devicePath/connector tie-break + v0 사용자 라벨 |
| 앱이 위치 자체 복원 | 복원 실패 | 검증 재시도, 화이트리스트 지연 적용 |
| UIPI로 상승 창 누락 | 일부 창 미복원 | 상승 헬퍼 위임 |
| `powercfg` 출력 포맷 변동 | 파서 깨짐 | 섹션 기반 파서 + 버전별 fixture |
| 적응형 디바운스 부정확 | 조기 복원으로 재붕괴 | 슬라이딩 윈도우 + 상/하한 클램프 + 사용자 조정 |
| WebView2 부재 | 리포트 미표시 | 정적 HTML/텍스트 폴백 |
| IDD 서명/배포 부담(v2) | 출시 지연 | v2 분리 트랙, MVP는 복원형 |

## 19. 부록 A - 디렉토리 구조(안)

```
OhMyDisplay/
  src/
    core/
      event_bus.{h,cpp}
      power_notify.{h,cpp}
      display_topology.{h,cpp}   // CCD wrapper, identity, signature
      event_log.{h,cpp}          // wevtapi correlator
      store.{h,cpp}              // JSON, atomic write
      elevated_ipc.{h,cpp}       // named pipe client
      logger.{h,cpp}
    keeper/
      window_capture.{h,cpp}
      layout_store.{h,cpp}
      monitor_labels.{h,cpp}     // v0 label model + collision detect
      debounce.{h,cpp}           // adaptive debounce
      restore_engine.{h,cpp}
      ghost_monitor.{h,cpp}
    doctor/
      powercfg_collector.{h,cpp}
      sleepstudy_parser.{h,cpp}  // v1
      rules_engine.{h,cpp}
      report.{h,cpp}
    shell/
      tray.{h,cpp}
      hotkeys.{h,cpp}
      label_ui.{h,cpp}           // v0 monitor label editor
      ui_webview.{h,cpp}         // + text fallback
    elevated/
      helper_main.cpp            // OhMyDisplay.Elevated.exe
      helper_manifest.xml        // requireAdministrator
    app/
      main.cpp
      manifest.xml               // asInvoker
  tests/
  assets/
```

## 20. 부록 B - 핵심 API 빠른 참조

| 목적 | API |
|---|---|
| 디스플레이 구성 질의/설정 | `QueryDisplayConfig`, `SetDisplayConfig`, `DisplayConfigGetDeviceInfo` |
| 모니터-창 매핑 | `MonitorFromWindow`, `GetMonitorInfo` |
| 창 위치 | `GetWindowPlacement`, `SetWindowPlacement`, `EnumWindows` |
| 전원 알림 | `PowerRegisterSuspendResumeNotification`, `RegisterPowerSettingNotification` |
| 이벤트 로그 | `EvtQuery`, `EvtNext`, `EvtRender` |
| 전역 핫키 | `RegisterHotKey` |
| 프로세스 경로 | `QueryFullProcessImageNameW` |
| 원자적 파일 교체 | `ReplaceFileW`, `MoveFileExW` |
| 상승 헬퍼 기동 | `ShellExecuteExW(runas)` + named pipe |
