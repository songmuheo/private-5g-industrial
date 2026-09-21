# Trace schema — 결과 파일 상세 (현재 버전)

이 문서는 `results/<run>/` 아래에 생기는 모든 파일에 대해 **어느 프로세스가, 소스의 어느 지점에서, 무엇을**
기록하는지와 파일 간 연결 방법을 정리한다. 모든 파일은 실행 중인 프로세스가 **실시간으로** 쓴 것이며,
후처리로 생성되는 파일은 없다. 코드가 바뀌면 이 문서를 같은 커밋에서 갱신한다.

```
results/<run>/
  run.json          실행 파라미터·버전·측정 창(wall ns)          (run_local_e2e.sh 또는 수동 기록)
  summary.json      analysis/verify_run.py 의 완전성 검사 결과   (행 수·손실 수 요약, 파생값 없음)
  app/              video_sender(<stream>-tx-*), video_receiver(<stream>-rx-*), signaling.log
  gnb/              gNB tracer gnb_*.csv, gnb.log, gnb_stdout.log, gnb_metrics.jsonl [, *.pcap]
  core/             open5gs.log
  ue/               (코드 테스트만) srsUE ue.log, ue_metrics.csv, ue_mac_nr.pcap
```

## 0. 공통 형식

* CSV: 1행 헤더, 마지막에 `#`로 시작하는 footer(`# rows=N clock_domain=<boot_id>`; 문제가 있었을 때만
  `# overflow=… late=… unpublished=…`). 파서는 `#` 줄을 건너뛴다.
* `<file>.ERROR` 사이드카가 있으면 그 파일은 **불완전**하다(ring overflow, 쓰기 실패). `verify_run.py`가 FAIL 처리.
* 기록 방식: 핫패스(캡처/인코더 콜백/pacer/network/decode 스레드, gNB 스케줄러·MAC·RLC·PDCP 스레드)는 POD 한 행을
  고정 원형 ring에 복사만 하고(할당·락·IO 없음), nice 19 백그라운드 스레드가 500 ms마다 CSV로 flush한다.
  ring이 가득 차면 행을 버리고 **개수를 센다**(footer/`.ERROR`).

## 1. 두 개의 시계 — `mono_ns` 와 `wall_ns`

모든 시각 컬럼은 같은 순간을 두 시계로 찍은 값이다.

| | `*_mono_ns` | `*_wall_ns` |
|---|---|---|
| 시계 | `CLOCK_MONOTONIC` | `CLOCK_REALTIME` |
| 0점 | 그 머신의 부팅 시점 | 1970-01-01 UTC |
| 성질 | 뒤로 가지 않고 점프 없음 | NTP/수동 조정으로 점프 가능 |
| 쓰는 곳 | **같은 호스트** 두 프로세스/파일 사이의 지연(오차 ≈ 0). libwebrtc 내부 시계(`rtc::TimeMicros`)와 동일 | **다른 호스트** 사이의 지연. 오차 = 두 호스트의 시각 동기 오차(chrony/PTP 필요) |

파일 footer의 `clock_domain`(커널 boot_id)이 같으면 같은 머신·같은 부팅 → `mono` 비교 가능. 다르면 `wall`만 유효.
한 파일의 mono와 다른 파일의 wall을 섞어 빼면 안 된다(둘의 차이는 NTP 조정으로 조금씩 변한다).

## 2. 송신 측 `app/<stream>-tx-*` (video_sender)

전부 libwebrtc(M120 순정) 안의 **공식 확장점**에서 얻는다. libwebrtc 소스 패치는 없다.

### `-tx-frames.csv` — 캡처 슬롯마다 1행
출처: 우리 `VideoTrackSource`(`apps/common/video_source.h`, `test/test_video_capturer.cc` 패턴)가 캡처 그리드 시각에
프레임을 만드는 지점, 인코더로 넘기기 **전**.

| 컬럼 | 의미 |
|---|---|
| `frame_idx` | 송신 프로세스의 캡처 순번(수신측 `frame_idx`와 무관) |
| `grid_slot` | 캡처 그리드 슬롯 정수. 명목 캡처 시각 = `grid_slot × 1e6/fps` µs (mono) |
| `src_frame_idx` | 재생한 소스(yuv/패턴) 프레임 번호 |
| `rtp_ts` | libwebrtc가 이 프레임에 붙일 RTP timestamp, **송신 공간**(`90 × (uint32)ntp_time_ms`). 프레임 신원 키 |
| `capture_wall_ns`, `capture_mono_ns` | 캡처 시각 |
| `width`, `height` | 인코더로 넘긴 해상도(`to_encoder=0`이면 캡처 해상도) |
| `to_encoder` | 1 = 인코더로 전달, 0 = 인코더가 요청한 프레임률 제한으로 소스가 버림(순정 VideoAdapter 동작) |

### `-tx-encoded.csv` — 인코딩된 프레임마다 1행
출처: `VideoEncoderFactory` 래퍼(`apps/common/webrtc_tracing.h`)가 인코더의 `OnEncodedImage` 콜백을 가로채는 지점
(패킷화 직전). libwebrtc 문서(`modules/video_coding/g3doc/index.md`)가 명시한 코덱 주입점.

| 컬럼 | 의미 |
|---|---|
| `rtp_ts` | 송신 공간(SSRC 오프셋 붙기 전) — `tx-frames.rtp_ts`와 같은 값 |
| `encode_done_mono_ns`, `encode_done_wall_ns` | 인코딩 완료(콜백 진입) 시각 |
| `bytes` | 인코딩된 크기 |
| `width`, `height` | 실제 인코딩 해상도(적응 후) |
| `frame_type` | `VideoFrameType`: 3 key, 4 delta |
| `qp` | 인코더 QP(-1 미보고) |
| `temporal_idx`, `spatial_idx`, `simulcast_idx` | 계층 인덱스(-1 없음) |
| `capture_time_ms`, `ntp_time_ms` | EncodedImage에 실린 캡처 시각들 |
| `codec` | `VideoCodecType`: 1 VP8, 2 VP9, 3 AV1, 4 H264 |
| `is_idr` | H264 IDR 여부(-1 타 코덱) |
| `at_target_quality` | 인코더가 목표 품질에 도달했다고 표시했는지 |

`tx-frames`에 `to_encoder=1`인데 여기에 같은 `rtp_ts`가 없으면 **인코더가 버린 프레임**이다(시작 직후 300 kbps
start bitrate 구간에서 순정 동작으로 몇 장 발생).

### `-tx-rtp.csv` — 송신 RTP 패킷마다 1행
출처: libwebrtc가 패킷마다 `RtcEventLog::Log()`에 넘기는 `RtpPacketOutgoing` 이벤트(`RtpSenderEgress`, UDP 소켓
write 직후). 우리 `RtcEventLog` 구현을 `PeerConnectionFactoryDependencies::event_log_factory`로 주입.

| 컬럼 | 의미 |
|---|---|
| `log_mono_ns`, `log_wall_ns` | 소켓에 넘긴 직후 시각(커널이 NIC로 보낸 시각은 아님) |
| `dir` | `out`(송신) / `in`(이 프로세스가 받은 RTP — 상대가 보낸 것) |
| `seq`, `rtp_ts`, `ssrc`, `pt`, `marker` | RTP 헤더(RFC 3550 §5.1). `rtp_ts`는 **전선 공간**(송신 공간 + SSRC별 상수 K) |
| `pkt_bytes`, `hdr_bytes`, `pad_bytes`, `payload_bytes` | 크기 분해. `payload_bytes=0` = 패딩(프로브) |
| `csrc_count`, `has_ext` | CC 필드, 헤더 확장 존재 비트(RFC 8285) |
| `probe_cluster_id` | GCC 대역 프로브 클러스터 번호(프로브가 아니면 -1) |

**seq가 뒤섞여 보이는 이유**: 파일에 SSRC가 둘 있다. 미디어 SSRC(pt 127 H.264)와 RTX SSRC(pt 113/103). RTX SSRC의
행들은 대부분 GCC 프로브 패딩이다(`probe_cluster_id ≥ 0`). seq는 SSRC마다 독립이고 SSRC별로 보면 단조 증가이며,
`log_mono_ns`도 전 행에서 단조 증가다. SSRC로 나눠서 읽는다: `awk -F, '$6==<ssrc>'`.

### `-tx-rtcp.csv` — RTCP compound 패킷마다 1행 (양방향)
출처: 같은 `RtcEventLog`의 `RtcpPacketIncoming/Outgoing`.
`log_mono_ns, log_wall_ns, dir, bytes, sender_ssrc, num_parts, parts`. `parts`는 `PT/FMT;…`:
`200` SR, `201` RR, `202` SDES, `205/1` NACK, `205/15` transport-cc(TWCC, 수신측이 보낸 도착시각 피드백),
`206/1` PLI, `206/4` FIR, `206/15` REMB (RFC 3550 §6, RFC 4585 §6, RFC 5104).

### `-tx-events.csv` — libwebrtc가 내보내는 그 외 모든 이벤트
출처: 같은 `RtcEventLog`. 한 행 = 이벤트 1개, `a..f`는 정수 필드(미사용 -1).

| event | 뜻 | a | b | c | d |
|---|---|---|---|---|---|
| `bwe_delay` | GoogCC 지연 기반 추정 갱신 | 추정 bps | 감지 상태 0 normal / 1 underusing / 2 overusing | | |
| `bwe_loss` | GoogCC 손실 기반 추정 갱신 | 추정 bps | 손실률 0..255 | 대상 패킷 수 | |
| `probe_created` | 대역 프로브 클러스터 시작 | cluster id | 목표 bps | 최소 패킷 수 | 최소 바이트 |
| `probe_success` | 프로브 성공 | cluster id | 측정 bps | | |
| `probe_failure` | 프로브 실패 | cluster id | 이유 0 invalid interval / 1 invalid ratio / 2 timeout | | |
| `alr_state` | Application-Limited Region 진입(1)/이탈(0): 인코더가 대역을 다 못 쓰는 상태 | in_alr | | | |
| `route_change` | 네트워크 경로 변경 | connected | overhead bytes | | |
| `ice_pair_config` | ICE 후보쌍 0 added / 1 updated / 2 destroyed / **3 selected** | type | pair id | | |
| `ice_pair_event` | ICE 연결성 체크 0 sent / 1 received / 2 response sent / 3 response received | type | pair id | transaction id | |
| `dtls_state` | DTLS 상태 0 new / 1 connecting / 2 connected / 3 closed / 4 failed | state | | | |
| `dtls_writable` | DTLS 쓰기 가능 여부 | writable | | | |
| `stream_config` | 스트림 설정 이벤트 발생 표시(내용은 미기록) | 1 send / 0 receive | | | |

기록하지 않는 타입: 오디오 관련(`Audio*`, `NetEq`), datagram transport용 `Generic*`(이 워크로드에서 미사용),
`RemoteEstimate`(M120에 공개 접근자 없음), 로그 시작/끝 마커. 위 표 외에 M120 `RtcEvent::Type`에 있는 타입은
`FrameDecoded`인데 M120 소스 어디에서도 발생시키지 않아(전수 확인) 넣지 않았다. **프레임 단위 인코딩/디코딩 이벤트는
RtcEventLog에 없으며** 코덱 팩토리 래퍼(`-tx-encoded`, `-rx-decoded`)가 그 역할을 한다.

### `-tx-stats.jsonl` — W3C getStats 1초 샘플
출처: `PeerConnection::GetStats()`(W3C webrtc-stats). `--stats-period-ms`(기본 1000, 0 = 끔)마다 1줄:
`{"mono_ns":…, "wall_ns":…, "stats":[RTCStatsReport 전체]}`.
여기만 있는 값: `outbound-rtp.targetBitrate`(인코더 목표 비트레이트), `qualityLimitationReason`(bandwidth/cpu/none),
`framesEncoded`, `frameWidth/Height`, `framesPerSecond`, `encoderImplementation`; `candidate-pair.currentRoundTripTime`,
`availableOutgoingBitrate`(GCC 최종 추정); `remote-inbound-rtp`(상대가 보고한 loss/jitter); `codec`(협상된 코덱과
fmtp). 카운터는 누적(W3C §4.1)이라 구간 값은 두 샘플의 차이다.

## 3. 수신 측 `app/<stream>-rx-*` (video_receiver)

### 수신 경로와 기록 지점

```
네트워크 → UDP 소켓 → SRTP 복호 → 패킷 조립·jitter buffer(대기) → 디코더 → 앱 sink
           (기록 없음)   ① rx-rtp.csv            ② rx-decoded.csv (start / done)   ③ rx-frames.csv
```

| 지점 | 파일·컬럼 | 의미 |
|---|---|---|
| ① 패킷 도착 | `rx-rtp.csv` `log_mono_ns` (패킷마다, `marker=1`이 프레임 마지막 조각) | libwebrtc가 소켓에서 읽어 SRTP를 푼 직후(`Call::DeliverRtpPacket`). 공개 API로 얻는 가장 이른 수신 시각, 커널 도착보다 수십 µs 늦음 |
| ② 디코딩 시작 | `rx-decoded.csv` `decode_start_mono_ns` | jitter buffer가 프레임을 내보내 디코더 `Decode()`에 넣은 순간. ①의 마지막 패킷과의 차 = **jitter buffer 대기** |
| ② 디코딩 완료 | `rx-decoded.csv` `decode_done_mono_ns` | 디코더가 픽셀을 돌려준 순간. `done − start` = **디코딩 시간** |
| ③ 앱 도착 | `rx-frames.csv` `recv_mono_ns` | 디코딩된 프레임이 track sink에 전달된 순간 = 앱이 쓸 수 있는 시각 |

세 파일은 같은 `rtp_ts`(전선값)로 이어진다. 커널 소켓 도착 시각 자체는 libwebrtc가 노출하지 않으므로 필요하면 호스트에서
`tcpdump`를 별도로 돌린다.

### `-rx-rtp.csv`, `-rx-rtcp.csv`, `-rx-events.csv`
송신측과 같은 메커니즘·같은 컬럼. `rx-rtp`의 `dir=in` 행이 수신 미디어이며 `tx-rtp`의 `dir=out` 행과 `(ssrc, seq)`로
1:1 대응한다(없는 seq = 손실). `rx-rtcp`의 `out 205/15`는 receiver가 보낸 TWCC 피드백. `rx-events`는 수신
프로세스가 미디어를 보내지 않으므로 GoogCC 이벤트가 거의 없고, ICE 체크·후보쌍 선택(`ice_pair_config type=3`)·DTLS
상태가 주 내용이다.

### `-rx-decoded.csv` — 디코더를 거친 프레임마다 1행
출처: `VideoDecoderFactory` 래퍼(`apps/common/webrtc_tracing.h`). `Decode()` 진입과 `DecodedImageCallback` 반환을 가로챔.

| 컬럼 | 의미 |
|---|---|
| `rtp_ts` | 전선 공간 |
| `decode_start_mono_ns` | jitter buffer에서 나와 디코더에 들어간 시각 |
| `decode_done_mono_ns`, `decode_done_wall_ns` | 디코더가 프레임을 돌려준 시각 |
| `input_bytes`, `frame_type` | 디코더 입력 크기, 3 key / 4 delta |
| `render_time_ms` | libwebrtc가 계획한 렌더 시각(내부 시계 ms) |
| `decoder_decode_time_ms` | 디코더가 스스로 보고한 디코딩 시간(-1 미보고; FFmpeg H.264는 미보고) |
| `qp` | 디코더가 보고한 QP(-1 미보고) |
| `width`, `height` | 디코딩된 해상도 |

### `-rx-frames.csv` — 앱에 전달된 디코딩 프레임마다 1행
출처: 트랙에 붙인 `VideoSinkInterface::OnFrame`(`apps/receiver/video_receiver.cc`).

| 컬럼 | 의미 |
|---|---|
| `frame_idx` | 수신 프로세스의 순번. **송신 `frame_idx`와 무관**(인코더가 버린 프레임은 수신 번호를 받지 않음) |
| `rtp_ts` | 전선 공간(`VideoFrame::timestamp()`) |
| `abs_capture_ntp_ms` | abs-capture-time 헤더 확장으로 실려온 **송신측 캡처 NTP(ms)**(-1 없음) |
| `sender_rtp_ts_est` | `90 × (uint32)abs_capture_ntp_ms` — 실시간으로 복원한 **송신 공간 rtp_ts**. `tx-frames.rtp_ts`와 ±90 tick(1 ms 반올림) 안에서 일치 |
| `wire_offset_est` | `rtp_ts − sender_rtp_ts_est` = SSRC별 상수 K(전 행 동일해야 정상) |
| `recv_wall_ns`, `recv_mono_ns` | 앱 도착 시각 |
| `width`, `height` | 디코딩 해상도 |
| `num_packets` | 이 프레임을 이룬 RTP 패킷 수 |
| `first_pkt_mono_ns`, `last_pkt_mono_ns` | libwebrtc가 그 패킷들의 페이로드를 처리한 시각(①보다 늦음; 패킷 도착은 `rx-rtp`를 볼 것) |
| `ssrc` | 미디어 SSRC |

### `-rx-stats.jsonl`
송신측과 동일 형식. 여기만 있는 값: `inbound-rtp.jitterBufferDelay`(누적 jitter buffer 대기), `jitter`,
`framesDecoded/framesDropped`, `totalDecodeTime`, `freezeCount`, `packetsLost`, `nackCount`, `pliCount`,
`decoderImplementation`.

## 4. gNB `gnb/` (srsRAN gNB + `patches/srsran_gnb/p5g_gnb_tracer.h`)

`P5G_GNB_TRACE_DIR`가 설정될 때만 활성. 모든 행은 `mono_ns, wall_ns`로 시작. `sfn` 0..1023, `slot`은 프레임 내 슬롯
(15 kHz 10개, 30 kHz 20개), `rnti`는 C-RNTI(10진), `ue_index`는 DU 내부 인덱스. hook 위치는
`patches/srsran_gnb/README.md`.

| 파일 | 1행 = | 컬럼(시각 뒤) |
|---|---|---|
| `gnb_sched_dl.csv` | 슬롯 내 UE 하나의 PDSCH grant | `sfn, slot, k_offset(k1), ue_index, rnti, harq_id, new_data(1 신규/0 재전송), nof_retxs, rv, mcs, mcs_table, rb_start, rb_count, sym_start, sym_count, nof_layers, tbs_bytes, buffer_occupancy(대기 DL 바이트), olla_offset, nof_grants_in_slot` |
| `gnb_sched_ul.csv` | 슬롯 내 PUSCH grant(그 슬롯에 실제 전송; DCI는 `k2` 슬롯 전) | 같음(`buffer_occupancy`=0) |
| `gnb_ul_crc.csv` | PUSCH CRC 결과 | `sfn, slot, ue_index, rnti, harq_id, crc_ok, ul_sinr_db, ul_rsrp_dbfs, ta_us`(NaN = PHY 미보고) |
| `gnb_dl_harq_ack.csv` | DL HARQ-ACK 비트 | `sfn, slot, ue_index, rnti, harq_id, ack(0 nack/1 ack/2 dtx), tbs_bytes, status(0 acked/1 nacked/2 no_update/3 error), pucch_sinr_db` |
| `gnb_bsr.csv` | BSR MAC CE의 LCG 항목 | `sfn, slot, ue_index, rnti, bsr_format(0 short/1 long/2 short-trunc/3 long-trunc), lcg_id, buffer_size_index, buffer_bytes`(TS 38.321 §6.1.3.1 상한) |
| `gnb_sr.csv` | 감지된 Scheduling Request | `sfn, slot, ue_index, rnti, pucch_format(1 F0/F1, 2 F2/F3/F4)` |
| `gnb_csi.csv` | CSI 보고 | `sfn, slot, ue_index, rnti, cqi, ri, valid` |
| `gnb_mac_ul_pdu.csv` | 수신 MAC UL PDU | `sfn, slot, ue_index, rnti, harq_id, pdu_bytes` |
| `gnb_rlc_ul.csv` | RLC AM UL 이벤트 | `ue_index, rb_is_drb, rb_id, event(pdu_rx / status_pdu_rx / sdu_delivered / reassembly_expired), sn, bytes, rx_next, rx_highest_status` |
| `gnb_pdcp_ul.csv` / `gnb_pdcp_dl.csv` | RAN을 나가는 / 들어오는 PDCP SDU(= IP 패킷) | `ue_index, rb_is_drb, rb_id, count, sdu_bytes, ip_proto, src_ip, dst_ip, src_port, dst_port, rtp_like, rtp_pt, rtp_seq, rtp_ts, rtp_ssrc, rtp_marker` |

`rtp_like=1` 행의 `(rtp_ssrc, rtp_seq)`는 앱 `tx-rtp/rx-rtp`의 `(ssrc, seq)`와 1:1이다(SRTP는 헤더를 암호화하지 않음).
같은 슬롯의 `gnb_sched_ul`과 `gnb_ul_crc`는 `(rnti, sfn, slot, harq_id)`로 이어진다.

순정 srsRAN 출력도 함께 둔다: `gnb.log`(srslog), `gnb_stdout.log`(1 s UE별 표: cqi, mcs, brate, ok/nok, bsr, ta),
`gnb_metrics.jsonl`(remote-control WebSocket JSON 메트릭, `{"recv_wall_ns","recv_mono_ns","metrics":{…}}`),
`P5G_GNB_PCAP=1`일 때 `gnb_mac.pcap`, `gnb_ngap.pcap`, `gnb_n3_gtpu.pcap`(기본 OFF — gNB 호스트 부하).

## 5. `core/`, `ue/`

* `core/open5gs.log` — `docker logs p5g_open5gs`(등록, PDU 세션, AMF/SMF/UPF 이벤트).
* `ue/`(코드 테스트만, 순정 srsUE): `ue.log`, `ue_metrics.csv`(1 s, `;` 구분: rsrp, dl/ul mcs, brate, bler, ta, ul_buff …),
  `ue_mac_nr.pcap`.

## 6. 파일을 잇는 키

```
tx-frames.rtp_ts == tx-encoded.rtp_ts ≈ rx-frames.sender_rtp_ts_est (±90)          송신 공간
tx-rtp.rtp_ts == rx-rtp.rtp_ts == rx-decoded.rtp_ts == rx-frames.rtp_ts               전선 공간 = 송신 공간 + K
tx-rtp.(ssrc,seq) == rx-rtp.(ssrc,seq) == gnb_pdcp_{ul,dl}.(rtp_ssrc,rtp_seq)
gnb_sched_ul.(rnti,sfn,slot,harq_id) ~ gnb_ul_crc.(rnti,sfn,slot,harq_id)
```

한 프레임의 전 구간: `tx-frames.capture_mono_ns` → `tx-encoded.encode_done_mono_ns` → `tx-rtp.log_mono_ns`(패킷들)
→ [UE 스택 · 무선 · `gnb_sched_ul/ul_crc/mac_ul_pdu/rlc_ul/pdcp_ul` · 코어] → `rx-rtp.log_mono_ns`(패킷들)
→ `rx-decoded.decode_start/done` → `rx-frames.recv_mono_ns`.

## 7. 실제 예시 (`results/20260921-205116-decoded`, 로컬 코드 테스트, 720p H.264)

수신 측 한 프레임(`rtp_ts` 2526758393):

```
rx-rtp      seq 13751   540800464.33 ms   첫 패킷 도착
rx-rtp      seq 13753   540800480.80 ms   마지막 패킷(marker=1)          ← 조립 가능 시점
rx-decoded  start       540800514.9  ms   → jitter buffer 대기 ≈ 34 ms  (순정 libwebrtc 재생 평활화)
rx-decoded  done        540800515.8  ms   → 디코딩 ≈ 0.9 ms
rx-frames   recv        540800516.14 ms   → 앱 전달 ≈ 0.3 ms
```

같은 run: RTP 5149/5149 수신(손실 0), `rx-decoded` 824행 = `rx-frames` 824행, 디코딩 시간 p50 0.9 ms / p99 1.7 ms,
gNB `sched_ul` 8222행(재전송 93, BLER 1.35 %), `tx-events`에 `bwe_delay` 214 / `bwe_loss` 52 / probe 4 생성.

## 8. 읽을 때 헷갈리기 쉬운 점

* `tx-frames`와 `rx-frames`의 `frame_idx`는 서로 다른 카운터다. 매칭은 항상 `rtp_ts`로 한다.
* 시작 직후 몇 프레임은 인코더가 버리고(`tx-encoded`에 없음), 해상도가 1280→320으로 내려간 뒤 GCC가 올라오며
  720p로 복귀한다 — 순정 libwebrtc(300 kbps start bitrate, `MAINTAIN_FRAMERATE`) 동작이다.
  `tx-stats.jsonl`의 `qualityLimitationReason`이 그 이유를 말해 준다.
* `tx-rtp/rx-rtp`의 seq 섞임은 두 SSRC(미디어 + RTX/프로브 패딩)가 interleave된 것이다(§2).
* `rx-frames.first/last_pkt_mono_ns`는 패킷 도착 시각이 아니다. 패킷 도착은 `rx-rtp`를 쓴다.
* `ul_rsrp_dbfs`가 0, PUSCH에 실린 HARQ-ACK의 `pucch_sinr_db`가 NaN인 것은 ZMQ PHY(코드 테스트)의 특성이다.
* footer의 `overflow`가 0이 아니거나 `.ERROR`가 있으면 그 파일로는 결론을 내지 않는다.
