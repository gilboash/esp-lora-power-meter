#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// RF configuration
//
// 868 MHz SRD band. Israel's short-range-device allocation follows the CEPT/EU
// plan for this band, so the EU parameters are used: 868.0-868.6 MHz, 25 mW
// (14 dBm) ERP, 1% duty cycle. Confirm against current MoC rules before a
// permanent install; the duty-cycle limit is enforced in software below.
// ---------------------------------------------------------------------------
#define LORA_FREQ_MHZ      868.1f
#define LORA_TX_DBM        14      // 14 dBm = 25 mW, the EU 868 ceiling
#define LORA_SYNC_WORD     0x12    // private network (not the 0x34 LoRaWAN word)
#define LORA_PREAMBLE_LEN  8
#define LORA_TCXO_VOLTAGE  1.8f    // Wio-SX1262 clocks the SX1262 from a TCXO on DIO3
#define LORA_USE_LDO       false   // module has the DC-DC inductor fitted
#define LORA_DUTY_CYCLE    0.01f   // 1% -> used to derive the minimum TX interval

// Spreading factor / bandwidth / coding rate presets. Selected with a build
// flag in platformio.ini. Start BALANCED; if the walk test is marginal, move to
// ROBUST (slower, ~6 dB more link budget) before adding a relay node.
#if defined(LORA_PROFILE_FAST)
  #define LORA_SF 7
  #define LORA_BW 125.0f
  #define LORA_CR 5
  #define LORA_PROFILE_NAME "FAST (SF7/BW125/CR4:5)"
#elif defined(LORA_PROFILE_ROBUST)
  #define LORA_SF 12
  #define LORA_BW 125.0f
  #define LORA_CR 8
  #define LORA_PROFILE_NAME "ROBUST (SF12/BW125/CR4:8)"
#else  // LORA_PROFILE_BALANCED
  #define LORA_SF 9
  #define LORA_BW 125.0f
  #define LORA_CR 5
  #define LORA_PROFILE_NAME "BALANCED (SF9/BW125/CR4:5)"
#endif

// ---------------------------------------------------------------------------
// Walk-test wire format
//
// Deliberately tiny and versioned. The real meter payload will be a different
// message type on the same framing, so the gateway can tell them apart from
// the magic byte alone.
// ---------------------------------------------------------------------------
#define MSG_MAGIC_PING  0x57  // 'W' - camera node -> gateway
#define MSG_MAGIC_ACK   0x41  // 'A' - gateway -> camera node
#define MSG_MAGIC_READ  0x52  // 'R' - meter node -> gateway, a decoded reading
#define MSG_VERSION     1

#pragma pack(push, 1)
struct PingPacket {
    uint8_t  magic;     // MSG_MAGIC_PING
    uint8_t  version;   // MSG_VERSION
    uint16_t seq;       // wraps at 65535, gateway tracks gaps
    uint16_t vbat_mv;   // 0 when no battery divider is fitted
    uint8_t  flags;     // reserved
};

struct AckPacket {
    uint8_t  magic;     // MSG_MAGIC_ACK
    uint8_t  version;   // MSG_VERSION
    uint16_t seq;       // echoed, so the sender can match it to its ping
    int16_t  rssi_dbm;  // uplink RSSI as measured at the gateway
    int16_t  snr_ddb;   // uplink SNR at the gateway, in tenths of a dB
};
// The actual telemetry. Deliberately tiny: at SF12 this is ~1 s of airtime,
// so it fits comfortably inside the 1% duty cycle even every minute.
struct ReadingPacket {
    uint8_t  magic;      // MSG_MAGIC_READ
    uint8_t  version;    // MSG_VERSION
    uint16_t seq;        // wake counter, so the gateway can spot missed cycles
    uint32_t value;      // meter reading in units of 0.1 kWh (avoids floats on the wire)
    uint16_t vbat_mv;    // 0 when no battery divider is fitted
    uint8_t  digits;     // how many digits were decoded
    uint8_t  confidence; // 0-100, min per-digit segment confidence
    uint8_t  flags;      // bit0: decode succeeded. A node that cannot read the
                         // meter still transmits, so "blind" stays
                         // distinguishable from "dead".
};
#define READING_FLAG_DECODE_OK 0x01
#pragma pack(pop)

static_assert(sizeof(PingPacket) == 7, "PingPacket must stay 7 bytes");
static_assert(sizeof(ReadingPacket) == 13, "ReadingPacket must stay 13 bytes");
static_assert(sizeof(AckPacket) == 8, "AckPacket must stay 8 bytes");

// Timing
#define PING_INTERVAL_MS   5000  // raised automatically if the duty cycle needs it
#define ACK_TIMEOUT_MS      800  // generous: covers SF12 turnaround
#define ACK_TURNAROUND_MS    20  // gateway waits this long so the sender can switch to RX
