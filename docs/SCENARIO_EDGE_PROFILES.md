# 시나리오: Edge가 내려주는 카메라 프로파일과 profile-aware RAN 스케줄링

상태: 설계 노트 (2026-09-29 작성, 2026-10-01 §5.6 multi-camera fusion 추가). 송신기는 이후 GStreamer(§6.3 결정)와
SMEC식 사전 인코딩 ffmpeg 트리로 구현되었다(docs/ARCHITECTURE.md). 표기는 FACT(코드·측정·표준으로 확인),
HYPOTHESIS(추론했지만 검증 전), SPECULATION(탐색적)을 따른다. 코드 위치는 이 저장소의 stock checkout
기준이다(libwebrtc M120 `b0b827e0`, srsRAN_Project `release_25_10` `d2f4b70`).

## 0. 요약

* **시나리오.** 사람이 없는 공장에 가벼운 카메라가 아주 많이 달려 있다. 카메라 하나가 여러 태스크에 동시에
  쓰이고(카메라↔태스크 조합이 다양함), 태스크마다 요구 fps / 해상도 / 지연이 다르다. 카메라별 프로파일은
  edge가 정해서 내려주며 시간에 따라 바뀐다. 이때 RAN이 이 조합을 어떻게 이해하고 스케줄링할지 설계하는 것이 목표다.
* **카메라 프로파일은 태스크에 따라 바뀐다.** 카메라가 섬기는 태스크 집합이 바뀌면(태스크 추가·제거·교체) 그 카메라의
  프로파일(fps / 해상도 / bitrate)도 edge가 다시 정해 내려준다. 같은 fusion 그룹의 카메라라도 서로 다른 프로파일을 가질 수 있다.
* **Multi-camera fusion(§5.6).** 한 추론이 여러 카메라의 **같은 시점(같은 frame 번호) 프레임 전부**를 필요로 한다.
  RAN은 이 그룹 구조를 모르므로, 이미 앞서 있는 카메라에 자원을 주고 뒤처진 카메라(그룹의 병목)를 기다리게 할 수 있다.
  RAN이 그룹과 프레임 진행(frontier)을 알고 뒤처진 카메라에 자원을 몰아주게 하는 방법이 연구 주제다.
* **핵심 관찰.** RAN에게 필요한 것은 "해상도"가 아니라 프로파일에서 유도되는 트래픽 모델이다. 즉 주기, 위상,
  burst 크기, 마감시각, 그리고 어떤 카메라 프레임들이 함께 도착해야 하는지(그룹)다. UL은 요청 기반이라
  RAN은 현재 프레임을 BSR이 오기 전에는 볼 수 없다. 그래서 프로파일이 결정적(deterministic)일 때만 RAN이
  미리 준비할 수 있다(§3, §5).
* **libwebrtc 질문에 대한 답.** 반은 맞다. 이 시나리오와 충돌하는 것은 libwebrtc 자체가 아니라, **송신기 안에서
  bitrate·fps·해상도를 스스로 바꾸는 제어기들**이다. 대표가 GoogCC이고, 인코더 쪽 frame drop 경로 세 개가 더 있다.
  이것들은 모두 공식 확장점이나 field trial로 끌 수 있고 교체할 수 있다(§6.1 표). 권장안은 libwebrtc를
  "프로파일 실행기"로 쓰는 것이다. GoogCC 자리에 고정 rate 제어기를 주입하고 frame drop 경로를 끈다.
  그러면 기존 추적을 그대로 쓰면서 GoogCC와 프로파일 고정을 **같은 스택에서 한 변수만 바꿔** 비교할 수 있다.
  다만 실제 산업용 카메라에 가깝게 만드는 것이 목적이라면 WebRTC가 아닌 RTP/RTSP 송신기가 맞다(§6.3).
* **현재 testbed는** 관측(RTP 헤더까지 보이는 gNB PDCP hook)이 이미 충분하다. 빠진 것은 세 가지다:
  (a) 프로파일을 고정하는 송신기 모드, (b) 실행 중 프로파일을 바꾸는 경로, (c) RAN이 프로파일을 아는 경로.
  (c)는 프로젝트 규칙 2("hook은 관측만 한다")와 충돌하므로 범위 결정이 필요하다(§7).

## 1. 문제 정의

```
            task graph / accuracy models                 channel state, PRB budget
  ┌────────────────────────────────────┐        ┌───────────────────────────────────┐
  │ EDGE (profile authority)           │ (2)    │ RAN (resource authority)          │
  │  tasks T1..TM, 각 태스크의 요구      │◄──────│  per-UE SE, BLER, PRB 가용량         │
  │  카메라 집합 C_t, fps/res/D/동기 허용 │ cost/  │  UL 스케줄러 (SR/BSR/grant/HARQ)    │
  │  → 카메라별 프로파일 P_c(epoch)       │feasib. │                                   │
  └───────┬───────────────────┬────────┘        └──────────▲────────────────────────┘
      (1) │ profile           │ (3) RAN-facing descriptor  │
          ▼                   └────────────────────────────┘
   cam1  cam2 ... camN  ── UL video (주기적 burst) ──► gNB ──► UPF ──► edge 추론
```

* 카메라 N개, 태스크 M개. 태스크 t는 카메라 집합 C_t를 쓰고 요구사항 (fps_t, res_t, 화질/bitrate, 마감 D_t,
  다중 시점 동기 허용 δ_t)을 가진다. 카메라 c는 태스크 집합 T_c를 섬긴다.
* 제어 주체는 둘이다. edge는 태스크를 알고, RAN은 무선 자원을 안다. 서로 모르는 정보가 있으므로 경로 (1)~(3)이 필요하다.
* 제약: 카메라는 가볍다(인코더 한 개, 제한된 전력). 셀 UL 용량은 유한하다. 이 testbed의 20 MHz DDDSU 셀은
  **전송 완료 기준 약 4~5 Mbps**다(FACT: docs/RAN_CONFIG.md "Known limits" 3, NOTES 2026-09-29 192247 run의 합계 5.0 Mbps).

## 2. 프로파일 조합: 카메라 하나가 여러 태스크를 섬길 때

| 방식 | 카메라가 내보내는 것 | 장점 | 단점 |
|---|---|---|---|
| (i) 합집합(max) | fps = max_t fps_t, res = max_t res_t 인 스트림 하나 | 인코더 1개, 가장 단순, RAN 입장에서 흐름 1개 | 낮은 요구를 가진 태스크에게는 낭비. 태스크 추가·제거가 곧 전체 프로파일 변경이 된다 |
| (ii) 계층화(SVC) | 시간 계층(L1T2/L1T3) 또는 공간 계층 스트림 하나 | 태스크가 계층을 골라 받는다. 계층마다 중요도가 달라 RAN이 선택적으로 버릴 수 있다 | 공간 SVC는 VP9/AV1 필요. H.264(OpenH264)는 **L1T1~L1T3만 지원**(FACT: `modules/video_coding/codecs/h264/h264.cc:48`) |
| (iii) 태스크별 스트림 | simulcast 또는 여러 번 인코드 | 태스크마다 정확히 맞춤 | UL 바이트가 합으로 늘어난다. 가벼운 카메라에는 비현실적 |

설계 규칙 제안:
* **fps는 격자 위에서만 고른다.** 기준 rate의 약수만 허용하면 합집합 fps의 프레임 중 일부가 곧 각 태스크의 프레임이
  된다(정확한 부분집합이며 추출 지터가 없다). 그리고 시간 계층(L1T3: 30/15/7.5)과 그대로 대응된다.
  예를 들어 10 fps 태스크와 15 fps 태스크를 15 fps 카메라 하나로 섬기면 10 fps 태스크는 불균일한 간격으로 샘플을 받는다.
  30 fps 격자라면 둘 다 정확하다.
* **RAN 격자와의 정렬(2026-10-01 측정으로 확인, FACT).** 2-UE MOT17-03 run에서 IDR이 없는 그룹 프레임의 지연 중앙값이
  k mod 3에 따라 33.0 / 43.3 / 43.6 ms였다(30 fps 33.3 ms, SR 주기 20 ms, UL 슬롯 2.5 ms가 100 ms = 3프레임마다 재정렬).
  아래는 원래의 추론이다. DDDSU의 UL 슬롯은 2.5 ms마다 온다. 30 fps의 주기 33.33 ms는 2.5 ms의 정수배가
  아니므로 프레임 도착과 UL 슬롯의 위상이 3프레임(100 ms) 주기로 돈다. 반면 25/20/10/5 fps(40/50/100/200 ms)는
  TDD 주기의 정수배다. 30 계열 격자와 25 계열 격자 중 무엇을 기준으로 할지는 RAN 설계와 함께 정할 문제다.
  Configured grant 주기도 같은 문제를 겪는다(§5).
* 1단계 실험은 (i)로 한다. (ii)는 RAN의 선택적 폐기(§5.4)를 시험할 때 추가한다.

## 3. RAN이 알아야 하는 것: RAN-facing descriptor

RAN은 프로파일(해상도, 코덱)을 해석할 필요가 없다. 필요한 것은 아래 트래픽 모델이다. edge가 프로파일에서 계산해 준다.

| 필드 | 의미 | 출처 | 비고 |
|---|---|---|---|
| flow id | UE(RNTI) + QoS flow/LCID, 또는 5-tuple + SSRC | edge·core | 지금은 UE마다 기본 QoS flow 하나다(5QI 9, FACT: `ran/core/subscriber_db.csv`의 11명 전원 QCI 9) |
| period T_c | 1/fps | 프로파일 | |
| phase φ_c | 캡처 시각 오프셋(공통 시계 기준) | edge | 동기 캡처에는 공통 시계가 필요하다. testbed는 chrony 동기 LAN을 쓴다. 5GS 자체의 시각 배포(SIB9 referenceTimeInfo, TS 38.331; TS 23.501 §5.27.1)도 대안이다 [조항 확인 필요] |
| burst 크기 | 평균 S̄ = bitrate/fps, I 프레임 S_I ≈ k·S̄ | 프로파일 + 관측 | ARMA는 k = 3~5로 보고했다(docs/RELATED_SYSTEMS.md) |
| GOP / IDR 위상 | IDR 간격과 시점 | edge | 카메라 간 IDR을 엇갈리게 배치한다(ARMA §5.3 inter-UE keyframe interleaving) |
| 마감 D_c | 캡처 기준 RAN이 써도 되는 지연 예산 | edge: 태스크 D_t − 추론 − 코어/N6 | 3GPP의 PDB/PSDB 개념에 해당한다 |
| **그룹** | 함께 완료되어야 하는 flow 집합과 동기 허용 δ | edge | 다중 시점 fusion. 같은 캡처 시각의 K개 프레임이 모두 D 안에 와야 한다(AoC 지표, RELATED_SYSTEMS) |
| 중요도 | I/P 프레임, 기본/향상 계층 | 프로파일 | 3GPP PDU Set Importance에 해당한다 |
| epoch | 프로파일 id·버전과 적용 시각 | edge | 카메라와 RAN이 같은 시각에 전환하기 위해 필요하다. 전환 시 IDR이 발생한다(§6.2) |

표준과의 대응(FACT 수준은 조항마다 다르다):
* 주기·burst 도착 시각·survival time은 TSCAI(TS 23.501 §5.27.2)가 이미 표현한다.
* Rel-18 XR/미디어 지원(TS 23.501 §5.37)에는 PDU Set과 PDU Set QoS 파라미터(PSDB, PSER, PSIHI)가 있다.
  RTP 헤더 확장을 통한 PDU Set 표시는 TS 26.522에, GTP-U 쪽 PDU Set 정보는 TS 38.415에 있다 [각 조항 확인 필요].
* **그룹 마감은 표준 QoS 모델에 없다.** 3GPP QoS는 QoS flow 단위다. Rel-18 multi-modal 서비스(§5.37)는 여러 flow의
  정책을 묶는 방법을 다루지만, 여러 UE에 걸친 "공동 완료 마감"을 스케줄러 입력으로 정의하지는 않는 것으로 안다
  (HYPOTHESIS, 조항 확인 필요). 이 부분이 연구 기여가 될 수 있는 틈이다.

## 4. 프로파일이 RAN에 도달하는 경로

| 경로 | 시간 척도 | 표현력 | 이 testbed에서 | 판단 |
|---|---|---|---|---|
| (a) 3GPP 제어 평면: AF → NEF/PCF → SMF → gNB (QoS flow, GBR/MFBR/PDB, TSCAI, PDU Set QoS) | 초 단위(세션 수정) | flow 단위, 그룹 없음 | Open5GS 2.7에서 AF/NEF/TSCTSF 경로가 동작하는지 미확인(HYPOTHESIS: 미지원 또는 부분 지원). srsRAN UL 스케줄러는 PDB를 **사용하지 않는다**(아래 FACT) | 표준 호환성을 확인하는 용도 |
| (b) O-RAN near-RT RIC: edge → xApp → E2SM-RC | 10 ms~1 s | slice 단위 PRB 비율 | srsRAN 25.10에 E2SM-RC 제어가 있다. slice의 RRM policy ratio를 제어한다(FACT: `lib/e2/e2sm/e2sm_rc/e2sm_rc_control_action_du_executor.cpp:117`). 측정 노출용 E2SM-KPM도 있다 | 조합이 느리게 바뀌는 경우의 자원 분할 |
| (c) edge → gNB 직접 profile 채널(비표준) | ms | 위 표 전부(그룹 포함) | 스케줄러 수정이 필요하다. ARMA/Tutti의 per-frame UDP 통지(rnti, size, ts)와 같은 계열이다 | 연구용 상한 |
| (d) 추론만 사용: gNB가 BSR 시계열과 PDCP의 RTP 헤더(ts, marker)로 주기·크기를 학습 | 몇 주기 지연 | 마감·그룹은 알 수 없음 | tracer가 이미 per-packet `rtp_ts, rtp_marker, rtp_ssrc`를 본다(FACT: TRACE_SCHEMA `gnb_pdcp_ul`). 단 UL에서는 **전송이 끝난 뒤**에야 보인다 | 선언값의 보정용 |

권장(HYPOTHESIS): **선언과 관측의 결합.** edge가 선언한 descriptor가 의도(마감, 그룹, epoch)의 기준이 되고,
gNB의 관측(실제 burst 크기, 도착 지터)이 수치를 보정한다. 선언값은 프로파일이 바뀌는 순간부터 스케줄러를 준비시키고
(학습 지연 없음), 관측값은 I 프레임 크기나 인코더 오차를 교정한다. testbed에서는 (c)의 의미론을 쓰되
필드를 TSCAI + PDU Set + 그룹 확장으로 맞춘다. 그렇게 하면 결과를 (a)의 표준 경로에 대응시킬 수 있다.

## 5. RAN이 그 정보로 하는 일 (UL 중심)

### 5.1 지금 UL이 반응형이라서 생기는 비용 (FACT, 이 testbed 측정값)
SR 주기 20 ms, SR→grant 3.0 ms, BSR→다음 grant 1~3 ms(p90 6 ms), grant→CRC 4.1 ms, HARQ 재전송 간격
7.5 ms(DDDSU), HARQ 완료 p99 12~22 ms(docs/RAN_CONFIG.md). UE가 grant 없이 쉬고 있으면 프레임의 첫 바이트는
평균적으로 SR 대기(≈10 ms) + 3 ms 뒤에야 나간다. BSR은 버킷 상한값으로 보고되므로 grant가 실제 데이터보다
커진다. 사용된 UL PRB의 약 1/3이 padding이다(FACT, RAN audit 2026-09-28).

### 5.2 알려진 주기·위상이 주는 것
* **선제 grant.** 예측 도착 시각(φ_c + n·T_c + 인코드 지연)에 BSR 없이 grant를 준다. P 프레임 크기에 맞춘
  Configured Grant(TS 38.321 §5.8.2)나 Tutti식 dynamic pre-grant를 기본으로 하고, I 프레임은 dynamic grant로 보탠다.
  단점은 도착 지터가 있으면 grant가 비거나(padding) 모자라다는 것이다. 이는 지연과 효율의 교환이다.
  srsRAN 25.10에는 CG 스케줄링이 없어 보인다. `ConfiguredGrantConfig`는 ASN.1 코드(`lib/asn1/rrc_nr`)에만
  나온다(HYPOTHESIS, grep 결과 기준). Rel-18 XR에는 비정수 주기 CG, 여러 PUSCH를 쓰는 CG, Delay Status Report
  MAC CE(TS 38.321 Rel-18)가 들어갔다 [조항 확인 필요]. 30 fps처럼 TDD 주기와 맞지 않는 fps를 위한 장치다.
* **UL 스케줄러에 마감을 넣는다.** srsRAN `time_qos`는 DL에서만 HOL 지연/PDB 가중치를 쓰고, UL 가중치에는
  지연 항이 없다. UL은 GBR·우선순위·PF만 본다(FACT: `lib/scheduler/policy/scheduler_time_qos.cpp`,
  `compute_ul_qos_weights`가 `delay_weight`에 1.0을 넘김). 즉 stock gNB에서는 UL 마감을 선언할 방법이 없다.

### 5.3 다중 카메라: 동기 캡처 vs 엇갈린 위상
* **fusion 그룹 안에서는 순서가 그룹 마감에 영향을 주지 않는다 — 단, 그룹 프레임 하나만 처리 중일 때에 한한다.**
  work-conserving 스케줄러라면 그 경우 그룹 마지막 프레임의 완료 시각은 Σ바이트/가용 rate로 정해지고 서비스 순서와 무관하다.
  실제로는 33 ms마다 다음 프레임이 도착해 여러 frame 번호가 동시에 큐에 있으므로, 앞선 카메라의 **다음** 프레임을 먼저
  서비스하면 그룹의 현재 프레임이 늦어진다(순서가 중요해진다, §5.6, HYPOTHESIS: 측정으로 확인). 그룹 마감에 중요한 것은 (i) padding과
  재전송으로 용량을 낭비하지 않는 것, (ii) HARQ 꼬리다. 따라서 그룹 안에서는 캡처를 동기화하는 것이 옳다
  (Argus는 |dt| < 3 ms로 맞춘다). IDR만 카메라마다 엇갈리게 둔다.
* **독립 태스크의 카메라끼리는 위상을 엇갈리게 두면 피크가 줄고** 카메라별 지연도 줄어든다.
* **여러 그룹이 공존하면** 그룹 마감 기준 EDF를 적용한다. 어느 카메라가 어느 그룹에 속하는지는 edge만 안다.
  그래서 descriptor에 그룹 구조가 반드시 들어가야 한다(§3).
* 용량 감각(이 셀 기준, HYPOTHESIS: 링크 상태에 따라 2배 이상 달라진다): 5 Mbps 셀이면 33 ms 주기당 약 20 KB다.
  720p30 2.5 Mbps 카메라의 평균 프레임은 10.4 KB이므로 카메라 2대면 셀이 가득 찬다. 이는 NOTES의
  2-UE/5-UE run 결과와 일치한다. "많은 카메라"는 이 셀에서 곧 낮은 프로파일을 뜻한다. 결국 프로파일 선택이
  용량을 조절하는 주된 수단이다.

### 5.4 채널 변동은 누가 흡수하나
NOTES 2026-09-29(192247 run)에서 한 UE가 5초 동안 fade되자(MCS 23→6) 그 UE의 PRB 수요가 수 배로 늘었다.
이것이 셀 전체의 1초짜리 혼잡 펄스로 번졌고, GoogCC가 두 송신기를 60~80% 깎은 뒤 90초 넘게 천천히 회복했다(FACT).
프로파일이 고정되면 송신기는 반응하지 않는다. 따라서 흡수를 아래 계층에 명시적으로 배정해야 한다.

| 루프 | 주체 | 시간 척도 | 동작 |
|---|---|---|---|
| 스케줄링 | gNB MAC | 슬롯~프레임 주기 | 여유가 있는 flow에서 PRB를 빌려오기, 마감 순서, 낮은 중요도 PDU Set(향상 계층, 비참조 프레임) 폐기 |
| 링크 | gNB | ms~s | OLLA, 전력 제어. 마감이 급한 flow에는 BLER 목표를 낮게 둘 수도 있다(현재 srsRAN은 셀 단위 `olla_target_bler`만 있음) |
| 프로파일 | edge | 1~60 s(ASTRA 60 s 창, AWStream) | RAN이 알려주는 비용과 가능 여부를 보고 카메라별 프로파일을 다시 고른다 |
| 태스크 배치 | edge orchestrator | 분 | 어떤 태스크에 어떤 카메라를 쓸지 |

원칙: 안쪽 루프가 바깥 루프의 가정을, 바깥 루프가 관측할 수 있는 속도보다 빠르게 바꾸면 안 된다.
GoogCC는 카메라 안에 있는 세 번째 제어기다. 수백 ms~s 척도로 같은 변수(bitrate)를 다른 목적(자기 경로의 지연)에
따라 움직이는데, edge도 RAN도 이를 볼 수 없다. 게다가 HARQ 지터와 큐 증가를 구분하지 못한다(FACT: NOTES
2026-09-28/29. RLC AM이 손실을 숨기고 trendline이 HARQ 꼬리를 과사용으로 읽음). 이 시나리오에서 GoogCC를 빼야 하는 근거가 이것이다.

### 5.5 RAN → edge 피드백 (경로 2)
edge가 조합을 고르려면 "카메라 c를 프로파일 p로 돌리는 데 드는 PRB-초"가 필요하다. 이 값은
bytes/s ÷ (현재 MCS에서 PRB당 바이트 × (1−BLER)) × (1 + padding 비율) + fade 여유로 추정할 수 있다.
UE별 스펙트럼 효율은 E2SM-KPM이나 현재의 `gnb_metrics.jsonl`로 노출할 수 있다. 그러면 edge는
Σ PRB 수요 ≤ 예산 조건 아래 태스크 효용 합을 최대화하는 선택 문제(배낭 문제 형태)를 푼다(SPECULATION: 정식화는 열린 문제).

### 5.6 Multi-camera fusion: RAN은 그룹의 frame frontier를 모른다 (2026-10-01, 사용자 시나리오)

**시나리오.** 태스크 t가 카메라 집합 G_t(예: cam1~cam3)의 프레임을 묶어 추론 한 번을 한다. frame 번호 k의 추론은 G_t의
**모든** 카메라의 frame k가 도착해야 시작할 수 있다. 따라서 그룹 완료 시각은 C_k = max_{i∈G} c_{i,k}(c_{i,k}: 카메라 i의
frame k 전달 완료 시각)이고, 가장 늦은 카메라(straggler)가 그룹 지연을 결정한다. 먼저 도착한 프레임은 기다리기만 한다.

**RAN이 해야 하는 일(목표 동작).** 카메라별로 "수신 완료된 마지막 frame 번호"(frontier) f_i(t)가 있고, 그룹 frontier는
F(t) = min_{i∈G} f_i(t)다. 예: cam1 = 444, cam2 = 444, cam3 = 440이면 그룹은 440에 묶여 있으므로 지금은 cam3에 자원을 몰아주는
것이 옳다. cam1·cam2의 445번 이후를 먼저 보내도 그룹 결과는 빨라지지 않는다. AI 태스크는 결과가 빠를수록 추론에 쓸 시간이
늘고, 안전 태스크는 조금이라도 빠른 것이 가치가 있으므로, 그룹 지연의 평균뿐 아니라 꼬리를 줄이는 것이 목적이다.
stock gNB는 그룹을 모르고 UE별 BSR·PF·QoS로만 자원을 준다(§5.2: UL 가중치에 지연 항도 없음).

**문제를 어렵게 만드는 것.**
* 프로파일이 태스크에 따라 바뀐다: 그룹 구성원이 바뀌고(epoch), 구성원끼리 fps·해상도·bitrate가 다를 수 있다. 큰
  프로파일의 카메라는 구조적으로 straggler가 되기 쉽다.
* 카메라 하나가 여러 그룹에 속한다: 그룹마다 마감과 필요한 프레임이 다르면 우선순위가 충돌한다(그룹 마감 EDF, §5.3).
* 태스크 fps가 카메라 fps보다 낮으면 그룹은 일부 frame 번호만 필요로 한다(예: 30 fps 카메라, 10 fps fusion → k mod 3 = 0만).
  그 프레임들만 중요도가 높다(PDU Set Importance에 해당).
* IDR: IDR 프레임은 커서 그 카메라를 그 frame 번호의 straggler로 만든다. 그룹 안에서 IDR을 엇갈리게 두면 그룹의 IDR이 든
  frame 번호가 GOP당 |G|개가 되고, 맞추면 1개로 줄지만 셀 피크는 커진다(교환 관계, §5.3의 "그룹 안에서 IDR만 엇갈림"은
  재검토 대상).
* 그룹이 커질수록(카메라 5대 이상) 그룹 지연은 카메라 지연의 최댓값이라 꼬리가 빠르게 커진다(순서통계).

**RAN이 무엇을 알면 되나.** gNB는 PDCP에서 UL RTP 헤더(ssrc, rtp_ts, marker)를 보므로 **UE별 전달 완료 frontier는 이미
관측할 수 있다**(FACT: tracer `gnb_pdcp_ul`; 단 전송이 끝난 뒤). 남은 대기 바이트는 BSR로 안다. 모르는 것은 (1) 어떤 UE·flow가
한 그룹인지, (2) 서로 다른 SSRC의 rtp_ts를 같은 캡처 시각(frame 번호)으로 맞추는 대응(카메라마다 RTP ts 기준이 무작위다;
RTCP SR 또는 abs-capture-time 헤더 확장, 혹은 descriptor의 epoch·phase로 해결), (3) 그룹 마감과 필요한 frame 부분집합이다.
즉 edge가 descriptor(§3의 "그룹" 행 + frame 번호 대응 + 필요 frame 집합)를 주고 gNB가 관측 frontier와 BSR을 결합하면
"가장 뒤처진 구성원 우선"(frontier-aware) 스케줄링이 가능하다(경로 (c), 표준 대응은 PDU Set + 그룹 확장, §4).

**testbed에서 이를 측정할 수 있는 이유.** ffmpeg 트리는 모든 카메라가 같은 순간에 같은 frame 번호를 보내고
(`--content-origin`, `tx-frames.src_frame_idx == grid_slot`) 캡처 시각이 µs 단위로 맞는다. 그래서 RAN이 그룹을 모르는
stock 기준선에서, **분석 시점에 임의의 카메라 부분집합을 그룹으로 정의해** 그룹 지표를 계산할 수 있다(실험 1회로 모든 그룹 구성 평가).

**그룹 조건의 출처.** 그룹(구성원, 마감 D)은 edge가 내려주는 descriptor다. testbed에서는 시나리오 JSON의
`"groups": [{"id": "g1", "cams": ["cam0", "cam1"], "deadline_ms": 100}]`로 선언하고(추후 profile 메시지로 이동), stock 기준선에서는
RAN에 전달하지 않고 분석에만 쓴다. 구현: `analysis/fusion_report.py`(항목 1~6, 8; 결과는 `<run>/analysis/fusion_<gid>.*`).

**분석 항목(기준선, stock RAN):**
1. 그룹 지연 L_k = C_k − capture_k 분포와 그룹 마감 충족률(D ∈ {50, 100, 200} ms), 그룹 크기 |G| = 2..N별(모든 부분집합).
2. 그룹 내 도착 편차(max − min)와 "기다린 시간" = Σ_i (C_k − c_{i,k}): 먼저 온 프레임이 낭비한 시간.
3. Frontier 지연: 시간에 따른 f_i(t) − F(t)(예 444/444/440에서 4), 그 크기·지속시간 분포.
4. 잘못 쓰인 자원: straggler가 있는 동안 frontier보다 앞선 frame을 위해 다른 구성원에 준 PRB-시간의 비율(=frontier-aware
   스케줄러가 돌려줄 수 있는 자원의 상한).
5. Straggler 정체: 매번 같은 카메라인가(구조적: 링크·프로파일 → edge의 프로파일 재조정 대상), 무작위인가(일시적:
   HARQ/RLC·SR 위상 → 슬롯 단위 스케줄러 대상). straggler 원인 귀속(SR 대기, HARQ 재전송, RLC 복구, IDR, 같은 슬롯 공유, MCS).
6. RAN이 관측만으로 straggler를 맞힐 수 있는가: 각 순간 gNB가 볼 수 있는 신호(UE별 BSR, PDCP 관측 frontier)로 고른
   "가장 뒤처진 UE"가 실제 straggler와 일치하는 비율. 낮으면 descriptor(그룹·frame 번호)가 필요하다는 근거가 된다.
7. 이득의 상한(오프라인 반사실): 같은 총 PRB를 frontier-aware 순서로 재배분했을 때의 그룹 지연(간단한 재생 모델)과 개별
   카메라 지연 손실(공정성 비용).
8. IDR 배치(그룹 내 엇갈림 vs 정렬)에 따른 그룹 지연과 셀 피크.
9. 확장(N > 5): 측정된 카메라별 지연 분포(상관 포함)로 그룹 크기 N에 대한 그룹 지연 꼬리를 외삽하고, 독립 가정과 비교한다.

## 6. 송신기: libwebrtc를 계속 쓸 것인가

### 6.1 libwebrtc M120이 fps / 해상도 / bitrate / burst 모양을 스스로 바꾸는 모든 곳

| # | 메커니즘 | 바꾸는 것 | 위치(FACT) | 무력화 방법 | 규칙 영향 |
|---|---|---|---|---|---|
| 1 | GoogCC(지연·손실 BWE, probe, ALR) | bitrate 목표 → QP, 그리고 아래 3·5·6을 통해 fps | `modules/congestion_controller/goog_cc/` | **이미 쓰는 주입점**(`apps/common/webrtc_session.h:96-111`, `WebRTC-Bwe-InjectedCongestionController`)에 고정 rate `NetworkControllerInterface`를 넣는다. 목표 rate는 상수, probe·cwnd는 없음 | 우리 코드. libwebrtc 패치 0 |
| 2 | 혼잡 윈도 pushback, 네트워크 down → `encoder_target == 0` → EncoderPaused | 모든 프레임 drop | `video/video_stream_encoder.cc:1602, 1845` | 1의 제어기가 cwnd를 설정하지 않음 | — |
| 3 | degradation 적응(QualityScaler, 대역폭, CPU overuse) | 해상도, fps | resource adaptation | `--degradation disabled`(이미 있음) | — |
| 4 | DropDueToSize(시작 직후) | 첫 프레임들 drop | `video_stream_encoder.cc:1825, 2356` | 시작 rate ≥ floor(`--start-bitrate-kbps auto`, 이미 있음) | — |
| 5 | FrameDropper(leaky bucket, media optimization) | 인코더 overshoot 시 drop | `video_stream_encoder.cc:1870-1874`. OpenH264는 `has_trusted_rate_controller`를 설정하지 않아(기본 false) 켜진다 | field trial `WebRTC-FrameDropper/Disabled/`(`:1365`) | field-trial 문자열. 패치 없음 |
| 6 | OpenH264 내부 frame skip(`bEnableFrameSkip`) | 인코더 내부 skip. NOTES 2026-09-28의 "iContinualSkipFrames" | `h264_encoder_impl.cc:636` ← `media/engine/webrtc_video_engine.cc:2229` `frame_drop_enabled = true`("Ensure frame dropping is always enabled", 공개 API로는 끌 수 없음) | 우리 encoder-factory wrapper(`webrtc_tracing.h:691` InitEncode)가 `SetFrameDropEnabled(false)`를 적용한 사본을 넘긴다 | 우리 wrapper의 **동작 변경**. 기본 OFF CLI 플래그 필요(규칙 4) |
| 7 | pacer(pacing factor 2.5, burst interval 0) | burst 모양: 프레임이 시간에 걸쳐 퍼진다 | `goog_cc_network_control.cc:60`, `modules/pacing/task_queue_paced_sender.cc` | 1의 제어기가 `PacerConfig`를 정한다. `RTCConfiguration::pacer_burst_interval`(`api/peer_connection_interface.h:686`)도 있다 | 프로파일 파라미터로 노출 |
| 8 | encoder bitrate adjuster(**기본 ON**: `VideoRateControlConfig::bitrate_adjuster = true`, `rate_control_settings.h:41`) | 인코더가 overshoot하면 인코더에 주는 rate를 목표보다 낮춘다 → 프로파일 rate에서 이탈 | `rtc_base/experiments/rate_control_settings.cc:175`, `video_stream_encoder.cc:1372` | field trial `WebRTC-VideoRateControl/bitrate_adjuster:false/`; 편차는 `tx-encoder-rates.csv`로 관측 | field-trial 문자열 |
| 8b | pacer 비상정지: pacer 큐 예상 대기 > 2 s → `target_rate = 0` → EncoderPaused | 모든 프레임 drop | `modules/congestion_controller/rtp/control_handler.cc:63-67`, `pacing_controller.cc:47`(2000 ms) | 1의 제어기가 pacing rate를 충분히 높게 두면 큐가 쌓이지 않는다. 끄는 trial `WebRTC-DisablePacerEmergencyStop`은 **전역** `field_trial::IsEnabled`(`control_handler.cc:28`)라 PC별 `FieldTrialsView`가 아닌 전역 초기화가 필요하다 | — |
| 9 | 키프레임 정책: 간격 3000프레임, PLI 요청 시 | IDR 시점을 제어할 수 없음(카메라 간 IDR 엇갈림 불가) | `api/video_codecs/video_encoder.cc:55`. M120의 `RtpSenderInterface`에는 GenerateKeyFrame이 없다(grep) | wrapper의 `Encode(frame, frame_types)`에서 스케줄에 맞춰 key를 요구 | 동작 변경. opt-in |
| 10 | NACK/RTX, 수신 jitter buffer | UL 재전송 트래픽, 재생 지연 | receive stream | 유지하고 측정. RLC AM이 이미 ARQ를 한다. 필요하면 playout-delay 확장 min=max=0 | — |

pacer 규모 예시(계산): 720p30 2.5 Mbps에서 pacing rate는 6.25 Mbps다. 평균 P 프레임 10.4 KB는 약 13 ms,
I 프레임 약 42 KB(k=4)는 약 53 ms에 걸쳐 흘러나간다. RAN은 이를 한 번의 burst가 아니라 BSR 여러 개로 본다.
실제 IP 카메라는 보통 프레임을 한 번에 내보낸다. 그러므로 burst 모양은 RAN 연구에서 통제해야 할 변수다.

A안의 정직한 비용(2026-09-30 추가). 위 표의 5·6·8은 libwebrtc의 내부 결정을 밖에서 억누르는 것이다. 억누르는 지점마다
libwebrtc가 믿는 상태와 인코더의 실제 상태가 어긋날 수 있다. 예: 6번을 wrapper에서 바꾸면 `VideoStreamEncoder`는
`GetFrameDropEnabled()`로 재설정 여부를 판단(`video_stream_encoder.cc:108`)하지만 인코더는 다른 값으로 돌아간다.
5·8은 field trial 문자열에 의존하고, 8b의 trial은 전역이다. 이 지점들은 M120에 고정된 내부 동작이라 버전을 올리면 다시 감사해야 한다.
억누르는 지점을 줄이는 변형: OpenH264를 감싸는 **우리 자체 `VideoEncoder` 구현**을 공식 `VideoEncoderFactory`로 넣고
`EncoderInfo::has_trusted_rate_controller = true`를 보고하게 한다. 그러면 5번은 trial 없이 꺼지고(`:1870-1872`의 조건),
6번은 우리 인코더가 frame skip을 켜지 않으면 되며, 9번(IDR 시점)도 우리 인코더가 결정한다. 남는 override는
1(제어기 주입, 공식 주입점)·3(공개 API)·8(trial)뿐이다. 대가는 `h264_encoder_impl.cc`(약 600줄)의 로컬 포크와 그 헤더 표기다.
어느 변형이든 §6.3의 불변식을 run마다 검사하는 것이 전제다. 불변식 검사 없이 A를 신뢰하면 안 된다.

### 6.2 프로파일의 "실행 중 변경" 측면에서는 libwebrtc가 오히려 유리하다
`RtpEncodingParameters`에는 `max_bitrate_bps`, `min_bitrate_bps`, `max_framerate`, `scale_resolution_down_by`,
`requested_resolution`, `scalability_mode`가 있다(FACT: `api/rtp_parameters.h:473-496`). 이 값들은 재협상 없이
`SetParameters`로 바꿀 수 있다. edge가 프로파일을 바꾸면 송신기는 (1) 자체 캡처 격자의 fps를 바꾸고
(`video_source.h`는 우리 코드라 결정적이다), (2) 해상도와 rate를 `SetParameters`로 적용하고, (3) 고정 rate 제어기의 상수를 바꾼다.
`DISABLED` degradation에서 `max_framerate`가 적용되는지는 확인하지 않았다(HYPOTHESIS). 그래서 fps는 캡처 격자에서
바꾸는 쪽이 안전하다. 해상도가 바뀌면 코덱과 무관하게 IDR(SPS 변경)이 생긴다. 이것이 프로파일 전환 비용이며,
epoch에 맞춰 RAN에 미리 알려야 하는 이유다.

### 6.3 선택지 비교

| 기준 | 현재(GoogCC + maintain_resolution) | A. libwebrtc + 고정 rate 제어기(권장) | B. libwebrtc 없는 RTP 송신기(GStreamer/FFmpeg식) |
|---|---|---|---|
| fps·해상도 불변 | 아님(6번 skip으로 fps 하락, NOTES 2026-09-28) | 1~6 무력화 후 불변(검증 필요) | 불변 |
| bitrate 불변 | 아님(GoogCC) | 목표는 상수, 인코더 RC 편차는 남음 | 인코더 CBR/VBV 정확도만큼 |
| burst 모양 제어 | GoogCC pacer | PacerConfig와 burst interval로 가능 | 기본이 한 번에 내보냄. 필요하면 직접 구현 |
| 실행 중 프로파일 변경 | — | SetParameters + 격자 + 제어기 상수 | 인코더 재설정 직접 구현 |
| 다중 태스크 계층(L1T3, simulcast) | 있음 | 있음 | 인코더 라이브러리에 따라 다름 |
| 기존 추적 재사용 | 전부 | 전부. 그대로 `tx-cc.csv`에 고정값이 기록된다 | 다시 구현해야 함(RTP ledger, 인코더 ledger) |
| GoogCC와의 비교 실험 | — | **같은 스택에서 제어기 하나만 바꿈**(변수 1개) | 스택 전체가 다르므로 교란 변수 |
| 실제 산업용 카메라와의 유사성 | 낮음 | 중간 | 높음. 실제 카메라는 RTSP(RFC 7826)/RTP와 하드웨어 CBR 인코더를 쓰고, VMS가 ONVIF Media2 `SetVideoEncoderConfiguration`(해상도, FrameRateLimit, BitrateLimit, GovLength)으로 설정한다. 바로 "edge가 프로파일을 내려주는" 구조다 |
| 구현 비용 | 0 | 작음. 제어기 1개와 플래그 3~4개 | 큼. 새 송신기·수신기와 추적 |
| 프로젝트 규칙 | — | 규칙 4(기본 OFF 플래그)로 수용 가능 | 규칙 5(대체 스택 금지)와 충돌. ARCHITECTURE에 근거를 남겨야 함 |

**결론(2026-09-29 초안).** "프로파일을 고정해야 하므로 libwebrtc는 안 된다"는 것은 GoogCC와 인코더의 자율 drop에 대해서는 맞다.
하지만 libwebrtc 전체에 대해서는 과하다. A로 가되 아래 **불변식**을 합격 기준으로 삼는다.
불변식이 깨지는데 원인을 공식 확장점으로 제거할 수 없거나, 연구 질문이 "실제 카메라의 burst 거동"으로 옮겨가면
B로 전환한다(SMEC/ARMA/Tutti 계열이 FFmpeg/GStreamer/RTP를 쓰는 것과 같은 선택). B의 후보 framework 비교는
docs/RELATED_SYSTEMS.md의 2026-09-30 addendum에 있다.

**결정(2026-09-30): B, GStreamer.** §6.1의 8·8b(bitrate adjuster 기본 ON, pacer 비상정지)까지 더해지자 A는 여섯 지점을
버전마다 다시 감사해야 하는 구조가 되었고, 사용자는 "기본 x264 ABR + 고정 fps/해상도"를 원했다. libwebrtc 스택은
`webrtc/`로 동결(tag `webrtc-baseline-2026-09-30`), 활성 트리는 `gstreamer/`(코드 공유 없음, 파일 계약만). 구현·검증 내용은
`gstreamer/README.md`, `docs/ARCHITECTURE.md`(결정 표), `docs/TRACE_SCHEMA.md` §0-a, NOTES 2026-09-30. 불변식 1~4는 그대로
GStreamer 트리의 합격 기준이며, 첫 loopback·srsUE run에서 모두 성립했다(캡처=인코드=전달, 720p 상수, rate 상수, rtp_ts 직접 join).

A의 불변식(모두 기존 trace로 판정 가능):
1. `tx-frames.to_encoder` = 1인 행 수 = 캡처 슬롯 수, 그리고 `tx-encoded` 행 수 = 캡처 수(drop 0).
2. `tx-encoded` 폭·높이가 epoch 안에서 상수.
3. `tx-cc.target_bps`가 epoch 안에서 상수, `tx-encoder-rates` 목표도 상수(8번 adjuster 편차는 따로 보고).
4. `tx-rtp`의 프레임별 첫 패킷~마지막 패킷 간격이 설정한 pacer 모양과 일치.

## 7. 현재 testbed 대비 gap

| 요소 | 지금(FACT) | 필요한 것 |
|---|---|---|
| 송신기 제어 | GoogCC, `--degradation maintain_resolution`(`run_sender.sh:90`), OpenH264 skip 켜짐 | A 모드: `--cc fixed`, frame-drop off, FrameDropper field trial, pacer 모양 플래그(모두 기본 OFF) |
| 프로파일 출처 | `experiments/*.json`에 run 단위 정적 값 | 시간에 따라 바뀌는 프로파일 목록. 신호 중계(TCP JSON)에 `profile` 메시지를 추가해 orchestrator가 edge 역할을 대신한다 |
| 코어 QoS | 전 가입자 5QI 9, UE당 기본 QoS flow 하나. 폰 자체 트래픽도 같은 bearer를 공유(NOTES 2026-09-22) | 카메라 flow 분리(전용 QoS flow, 가능하면 GBR). Open5GS에서 가능한지 확인 필요 |
| gNB 스케줄러 | `time_qos`. UL은 마감을 모르고 CG가 없음. E2SM-RC slice 비율 제어 있음 | 단계적으로 표준 knob(GBR) → profile-aware 선제 grant와 그룹 EDF |
| 관측 | PDCP hook이 per-packet RTP 헤더를 기록하므로 **프레임별 RAN 완료 시각과 마감 충족률을 지금 trace로 오프라인 계산할 수 있다** | 프레임·그룹 마감 지표를 `analysis/`에 추가(run 경로 밖) |
| 프로젝트 규칙 | 규칙 2: hook은 관측만 하고 스케줄러를 바꾸지 않는다 | profile-aware 스케줄러는 정의상 동작 변경이다. 별도 opt-in 패치 계열(예: `patches/srsran_gnb/1xxx-*`, 빌드 플래그로만 적용)로 두고 stock을 기준선으로 유지할지 **결정이 필요하다** |
| 규모 | B210 20 MHz: 전송 완료 약 4~5 Mbps → 낮은 프로파일로 5대 안팎 | "수많은 카메라"는 이 셀로 재현할 수 없다. 더 넓은 대역(100 MHz, UL 비중이 큰 TDD)과 다른 SDR, 또는 보정된 에뮬레이션이 필요하다. srsUE/ZMQ는 측정용이 아니다(규칙 5) |
| UE | 노트북 + Pixel 테더링 | 테더링 경로가 만든 인공물이 있었다(NOTES: cam1 테더 끊김, cam4 시작 시 700 KB 큐). "가벼운 카메라"를 대표하지 않는다. 장기적으로는 5G 모뎀을 붙인 SBC가 필요하다(SPECULATION) |

## 8. 단계별 실험 계획 (단계마다 변수 하나)

| 단계 | 바꾸는 변수 | 질문 | 측정 |
|---|---|---|---|
| P0 | 송신기를 A 모드로(RAN stock) | 프로파일 고정 송신기가 불변식 1~4를 지키는가 | loopback에서 먼저, 그다음 OTA. §6.3 불변식 |
| P1 | 제어기: GoogCC ↔ fixed(같은 시나리오) | GoogCC가 RAN 위에서 무엇을 보태고 무엇을 망치는가 | 프레임 마감 충족률(D ∈ {50, 100, 200} ms), 그룹 완료율, fps, UL PRB 효율 |
| P2 | 실행 중 프로파일 전환 | 전환 과도현상(IDR burst, BSR 스파이크)의 크기와 길이 | 전환 전후 프레임 지연, BSR, 재전송 |
| P3 | 표준 knob: 카메라별 QoS flow + GBR, `time_qos` gbr 가중치 | 스케줄러 수정 없이 선언만으로 어디까지 되는가 | P1과 같은 지표. HYPOTHESIS: 몫(share)은 개선되지만 UL이 PDB를 안 보므로 마감은 개선되지 않는다 |
| P4 | profile-aware UL 스케줄러(opt-in 패치) | 선제 grant + 그룹 EDF의 이득과 padding 비용 | 마감 충족률 vs padding 비율 |
| P5 | RAN → edge 비용 피드백 + 조합 선택 | 닫힌 루프가 수렴하는가, 진동하는가 | 태스크 효용, 재프로파일 빈도 |
| PG | fusion 그룹 인지(frontier-aware) UL 스케줄링(opt-in 패치) | 그룹을 알면 그룹 지연·꼬리가 얼마나 줄고 무엇을 잃는가 | §5.6 분석 항목 1~9, 기준선(stock) 대비 |

P0/P1은 스케줄러를 건드리지 않으므로 현재 규칙 안에서 바로 할 수 있다.

### 8.1 5-UE 기준선 실험 매트릭스 (2026-10-01, stock RAN, ffmpeg 트리)

설계 원칙:
* **stock RAN은 그룹을 모른다 → 무선 트래픽은 프로파일에만 의존한다.** 프로파일이 같으면 그룹 구성(3+2, 2+2+1, 3+1+1 …)을
  바꿔도 트래픽은 같으므로, 그룹 구성만 다른 실험은 중복이다. 그룹 구성은 실험 한 번을 분석할 때 임의로 바꿔 평가한다
  (`fusion_report.py --group`). 실험마다 바꿀 것은 **태스크 구조가 정하는 프로파일(fps / 해상도 / bitrate)과 부하**뿐이다.
* 모든 카메라가 같은 MOT17-03 장면(1500프레임, 고정 카메라)을 보내고 frame 번호가 정렬된다. fps가 낮은 카메라는 30 fps 격자의
  부분집합 프레임을 보낸다(15 fps = 짝수 프레임). 한 fusion 그룹은 태스크 fps를 공유한다.
* 카메라 K의 IDR 시점 오프셋은 0 / 133 / 267 / 467 / 533 ms(30 fps 기준 origin 0 / 4 / 8 / 14 / 16, 15 fps 기준 0 / 2 / 4 / 7 / 8).
  **위상(k mod 3)을 섞었다**(0, 1, 2, 2, 1). 2-UE run에서 IDR origin 0과 3이 둘 다 유리한 위상에 있어, 보정 전에는 IDR 프레임이
  오히려 빨라 보였다(교란 변수). IDR 효과는 같은 위상의 비-IDR 프레임과 비교한다(fusion_report 8번).
* 길이 5분(300 s), 반복과 카메라-폰 회전으로 여러 번 실행(§8.2). 측정은 링크 안정 후(기본 t ≥ 5 s). 그룹 조건(구성원, 마감 D)은 시나리오 `groups`(edge descriptor)에 선언하고
  RAN에는 주지 않는다.

| 실험 | 그룹(마감) | 카메라 프로파일 | 총 부하 | 질문 |
|---|---|---|---|---|
| **A** `5ue-A-uniform1500` | 3+2 (100 / 100 ms) | 5 × 720p30 1500k | 7.5 Mbps | 기준선. 그룹 크기·구성 효과(오프라인으로 {5}, {3,2}, {2,2,1}, {3,1,1} 모두) |
| **B** `5ue-B-tasks-3x720p30-2x1080p15` | 3+2 (100 / 200 ms) | 3 × 720p30 1500k, 2 × 1080p15 2500k | 9.5 Mbps | 태스크 종류가 다른 두 그룹: 큰 burst·저 fps 그룹이 다른 그룹을 해치는가(그룹 간 간섭) |
| **C** `5ue-C-mixed-2-2-1` | 2+2+1 (100 / 150 / 100 ms) | 2 × 720p30 1500k; 1080p15 2500k + 720p15 1000k; 360p30 500k | 7.0 Mbps | A와 거의 같은 부하에서 **그룹 안의 이질적 프로파일**(구조적 straggler)의 영향 |
| **D** `5ue-D-uniform2000` | 3+1+1 (100 / 100 / 100 ms) | 5 × 720p30 2000k | 10 Mbps | 부하: A 대비 1.33배에서 그룹 마감이 어떻게 무너지는가 |

비교 축(기술적 비교이며 단일 변수 실험이 아니다; Codex 검토 2026-10-01): A↔D는 같은 프로파일 형태에서 부하만(7.5 → 10 Mbps)
다르다. A↔C(7.5 / 7.0 Mbps)와 D↔B(10 / 9.5 Mbps)는 비슷한 부하에서 **태스크 구조 전체**(프로파일 구성, 태스크 fps, 마감,
그룹 구성)가 함께 바뀐다. 이 시나리오에서는 태스크가 바뀌면 그 묶음이 함께 바뀌므로 의도한 비교지만, 차이를 그 중 한 요인
(예: 그룹 내 이질성, 저 fps burst)에 귀속하려면 그 요인만 바꾼 통제 실험이 따로 필요하다. 각 run 안에서는 그룹 크기
(|G| = 1..5, 모든 부분집합)를 오프라인으로 비교한다(같은 트래픽). 프로파일 파일은
`prepare_video.sh --source mot17-03 --size/--fps/--origins`로 만들고, 각 랩탑은 `prepare_client.sh K`로 모든 카메라의 파일(약
450 MB)을 받는다(회전 때문에 어느 랩탑이든 어느 카메라든 맡는다).

### 8.2 반복과 카메라-폰 회전 (2026-10-01)

폰 배치(위치, 개별 단말 특성)는 고정이다. 같은 프로파일이라도 어느 폰이 맡느냐에 따라 결과가 달라질 수 있으므로, 프로파일·그룹을
폰에 대해 **회전**시켜 반복한다. `run_experiment.sh --rotate R`이면 시나리오의 camK(프로파일, 그룹, stream id)가 cam((K+R) mod N)의
랩탑/폰에서 실행되고, `experiment.json.cam_host`에 기록된다. 배치는 한 gNB 세션 안에서 연속 실행되고(`ffmpeg/run_batch.sh`),
각 실험은 `results/<session>/runs/<ts>-<name>-r<R>/`에 들어간다.

압축안(`ffmpeg/experiments/batch-5ue.json`, 8 × 약 5.7분 ≈ 46분): 1회차 회전 0으로 A→B→C→D, 2회차 회전 2로 D→C→B→A.
회전 2는 모든 프로파일·그룹을 다른 폰으로 옮기고(5 위치 중 3칸 이동), 순서를 뒤집어 느린 변화(예: 2-UE run의 210–240 s 간섭 구간)가
특정 시나리오에 몰리지 않게 한다. 시간이 되면 `batch-5ue-round3.json`(회전 4, 약 23분)을 추가한다. 세 회차를 한 번에 돌리는 `batch-5ue-3rounds.json`(회전 0 / 2 / 4, 순서 ABCD / DCBA / BADC, 12 run, 약 69분)도 있다: 각 프로파일이 서로 다른 폰 3대에서 측정된다(같은 조건의 반복은 아니다). 분석:
`analysis/batch_summary.py`가 실험·그룹별 마감 충족률, 회전 간 범위(폰 배치 효과), 폰 × 프로파일 지연표를 낸다.

## 9. 열린 질문과 위험

1. **CBR vs 일정 화질.** CBR은 RAN이 예측하기 쉽지만 장면에 따라 화질(곧 태스크 정확도)이 흔들린다. 일정 QP는 반대다.
   프로파일이 bitrate를 고정할지 화질을 고정할지가 곧 RAN이 받는 burst 크기 분포를 결정한다. 이 시나리오의 핵심 교환 관계다.
2. **그룹 마감의 표준화 틈.** 3GPP QoS가 flow 단위라는 점(§3)이 확인되면 이것이 곧 기여점이다. multi-modal 조항을 원문으로 확인해야 한다.
3. **UL 가시성.** gNB는 프레임을 BSR 이후에야 안다. 선제 grant는 도착 지터(인코드 시간, 테더링)에 민감하고 padding과 맞바꾸게 된다.
4. **계층 위반.** gNB가 RTP 헤더를 읽는 것은 관측 목적이면 괜찮다. 하지만 스케줄링 입력으로 쓰면 비표준이다.
   표준 경로는 RTP 헤더 확장 → UPF → GTP-U PDU Set 정보다(DL 기준). UL PDU Set 처리는 UE 내부 동작이다 [확인 필요].
   SRTP는 페이로드만 암호화하고 헤더 확장은 RFC 6904나 RFC 9335(cryptex)를 쓰지 않는 한 평문이다(RFC 3711 §3).
5. **epoch 동기.** edge·카메라·RAN이 같은 시각에 전환하지 않으면 전환 구간에서 RAN 모델이 틀린다. 전환 시 IDR도 발생한다.
6. **fps 격자와 TDD 격자의 불일치**(§2). CG 주기, 선제 grant 위상, 비정수 주기 처리 방식이 달라진다.
7. **규모.** 이 testbed에서 얻은 결론을 "수많은 카메라"로 외삽하려면 셀 용량과 UE 수를 함께 늘려야 한다. 그 전까지는 메커니즘 수준의 결론으로 한정한다.

## 참고

* 3GPP: TS 23.501 §5.7(QoS 모델), §5.27.1–5.27.2(시각 동기, TSCAI), §5.37(XR·미디어, PDU Set, multi-modal);
  TS 38.321 §5.4.4(SR), §5.4.5(BSR), §5.8.2(UL configured grant), Rel-18 Delay Status Report; TS 38.331(SIB9);
  TS 26.522(RTP 헤더 확장의 PDU Set 표시); TS 38.415(PDU Session Container의 PDU Set 정보). [§5.37 이하 세부 조항 번호는 원문 대조 필요]
* IETF: RFC 3550(RTP), RFC 3711(SRTP), RFC 6904 / RFC 9335(헤더 확장 암호화), RFC 7826(RTSP 2.0).
* O-RAN: E2SM-RC, E2SM-KPM(srsRAN 구현: `lib/e2/e2sm/`).
* ONVIF Media2 Service Specification(VideoEncoder2Configuration).
* 관련 시스템(코드 수준 조사): docs/RELATED_SYSTEMS.md. ARMA §5.3(IDR 엇갈림), Tutti(PRB pre-grant), SMEC(고정 프로파일, 100 ms SLO),
  AWStream/ASTRA(애플리케이션이 이산 knob을 거친 시간 척도로 선택), Argus(3 ms 동기), AoC(그룹 완료 age).
