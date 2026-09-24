/* How a program actually uses this library: a UART driver talking to a device that is not there.
 *
 *   make example && ./build/uart_device
 *
 * The shape is the one a real driver has. `uart_init()` loads the definition once and resolves every
 * field name it will ever need; `uart_receive_pkt()` is the RX callback, and it finds out what
 * arrived by decoding rather than by being told; the handlers read the command and update the
 * device's state; the senders build a reply and hand the bytes to `uart_transmit()`. Swap that one
 * function for a write to a real file descriptor and the rest of this file is unchanged.
 *
 * What it is worth reading for:
 *
 *   - **Names are resolved once.** The `ids` struct is filled in at init, so the RX path is field
 *     ids and no string comparisons. A field renamed in the .fdsl fails at startup, in one place,
 *     rather than in the callback at three in the morning.
 *   - **One frame, both directions.** The arena has a slot for every field of every packet, so the
 *     command being answered and the reply being built never share storage.
 *   - **Bytes that are not a packet are an ordinary outcome.** `framedsl_decode()` returns NULL and
 *     says why; a link carries noise, and chasing that down is half of why a simulator exists.
 *   - **The definition outlives everything.** The result owns it and the frame points at it, so all
 *     three are freed together in `uart_deinit()` and not before.
 *
 * To take it apart and play with it, copy this file somewhere and build against the library:
 *
 *   make lib
 *   cc -std=c11 -Wall -Wextra -Iinclude my_device.c -Lbuild -l:libframedsl.a -o my_device
 *
 * Try: change a width in examples/uart_device.fdsl and re-run without recompiling; add a packet and
 * a handler for it; feed uart_receive_pkt() a frame with a byte missing and watch what it says.
 */
#include "framedsl.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  uint16_t sequence;
  uint8_t active_channel;
  uint32_t limit;
} Device_t;

static Device_t device = { 0 };

/* The result owns the definition, and the frames point at it -- so all three live as long as the
 * driver does. Freeing the result once the definition is in hand would leave `definition` -- and the
 * frame that points at it -- dangling.
 */
static FramedslResult *result = NULL;
static const FramedslDefinition *definition = NULL;

/* One frame, both directions. The arena has a dedicated slot for every field of every packet, so the
 * command that arrived and the reply being built never share memory: TC_SET_LIMIT.limit still reads
 * back 4200 after TM_ACK.sequence has been written.
 *
 * A frame remembers the last value of each packet. Nothing here has to be read in a particular
 * order, and a field nobody has written reads back zero rather than refusing.
 */
static FramedslFrame *storage = NULL;

/* Resolved once, at init. This is the pattern the library is designed around: framedsl_find() walks
 * the definition comparing strings, so a driver pays for it at startup rather than per packet. The
 * struct is what replaces the find-on-every-line helper.
 */
static struct {
  FramedslFieldId hk_req_sequence;

  FramedslFieldId set_limit_sequence;
  FramedslFieldId set_limit_channel;
  FramedslFieldId set_limit_limit;

  FramedslFieldId write_size;
  FramedslFieldId write_data;
  FramedslFieldId write_crc;

  FramedslFieldId ack_sequence;
  FramedslFieldId ack_result;

  FramedslFieldId hk_sequence;
  FramedslFieldId hk_channel;
  FramedslFieldId hk_reading;
  FramedslFieldId hk_limit;

} ids;

/* Stands in for the peripheral. */
static void uart_transmit(const uint8_t *bytes, size_t length) {
  printf("  uart_transmit:");
  for (size_t i = 0; i < length; ++i) {
    printf(" %02X", bytes[i]);
  }
  printf("\n");
}

static bool resolve(const char *dotted, FramedslFieldId *out) {
  if (!framedsl_find(definition, dotted, out)) {
    fprintf(stderr, "uart: the definition has no field '%s'\n", dotted);
    return false;
  }
  return true;
}

void uart_deinit(void) {
  framedsl_frame_free(storage);
  framedsl_result_free(result);
  storage = NULL;
  definition = NULL;
  result = NULL;
}

/* Returns 0 on success. A load that fails is the one thing here that realistically does: a typo in
 * the .fdsl reaches this and nothing else can, which is the argument for loading at startup rather
 * than the first time a packet arrives.
 *
 * `path` is NULL for the default, and taken from the command line so the file can be pointed at a
 * copy you are editing. */
int uart_init(const char *path) {
  if (path == NULL) {
    path = "examples/uart_device.fdsl";
  }

  result = framedsl_load_file(path);
  if (!framedsl_ok(result)) {
    for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
      const FramedslDiagnostic *diag = framedsl_diagnostic(result, i);
      fprintf(stderr, "%s:%d:%d: %s\n", path, diag->line, diag->column, diag->message);
    }
    framedsl_result_free(result);
    result = NULL;
    return -1;
  }

  definition = framedsl_definition(result);

  storage = framedsl_frame_new(definition);
  if (storage == NULL) {
    uart_deinit();
    return -1;
  }

  /* Every name in the protocol, checked once. A field renamed in the .fdsl fails here, at startup,
   * rather than in the RX callback at three in the morning. */
  if (!resolve("TC_HK_REQ.sequence", &ids.hk_req_sequence)

      || !resolve("TC_SET_LIMIT.sequence", &ids.set_limit_sequence)
      || !resolve("TC_SET_LIMIT.channel", &ids.set_limit_channel)
      || !resolve("TC_SET_LIMIT.limit", &ids.set_limit_limit)

      || !resolve("TC_WRITE.size", &ids.write_size)
      || !resolve("TC_WRITE.data", &ids.write_data)
      || !resolve("TC_WRITE.crc", &ids.write_crc)

      || !resolve("TM_ACK.sequence", &ids.ack_sequence)
      || !resolve("TM_ACK.result", &ids.ack_result)

      || !resolve("TM_HK.sequence", &ids.hk_sequence)
      || !resolve("TM_HK.channel", &ids.hk_channel)
      || !resolve("TM_HK.reading", &ids.hk_reading)
      || !resolve("TM_HK.limit", &ids.hk_limit)


      ) {
    uart_deinit();
    return -1;
  }

  return 0;
}

/* ---- Handlers: read out of the frame, update the device state ------------------------------ */

static void __handle_tc_hk_req(void) {
  uint64_t sequence = 0;
  if (framedsl_get_u64(storage, ids.hk_req_sequence, &sequence) != FRAMEDSL_OK) {
    return;
  }
  device.sequence = (uint16_t)sequence;
}

static void __handle_tc_set_limit(void) {
  uint64_t sequence = 0;
  uint64_t channel = 0;
  uint64_t limit = 0;

  if (framedsl_get_u64(storage, ids.set_limit_sequence, &sequence) != FRAMEDSL_OK
      || framedsl_get_u64(storage, ids.set_limit_channel, &channel) != FRAMEDSL_OK
      || framedsl_get_u64(storage, ids.set_limit_limit, &limit) != FRAMEDSL_OK) {
    return;
  }

  device.sequence = (uint16_t)sequence;
  device.active_channel = (uint8_t)channel;
  device.limit = (uint32_t)limit;
  printf("  channel %u limit set to %u\n", device.active_channel, device.limit);
}

static void __handle_tc_write(void) {
  uint64_t size = 0;
  uint64_t crc = 0;
  size_t len = 0;

  /* No buffer of our own. framedsl_get_bytes() points into the frame's arena, where the payload
   * already is -- good until this same packet is decoded again, which is long enough to look at it
   * or hand it to a write(). framedsl_get_bytes_into() is the one that copies, for a payload that
   * has to outlive the next frame. */
  const uint8_t *data = framedsl_get_bytes(storage, ids.write_data, &len, NULL);

  if (framedsl_get_u64(storage, ids.write_size, &size) != FRAMEDSL_OK || data == NULL
      || framedsl_get_u64(storage, ids.write_crc, &crc) != FRAMEDSL_OK) {
    return;
  }

  printf("  size: %llu\n", (unsigned long long)size);
  printf("  data:");
  for (size_t i = 0; i < len; ++i) {
    printf(" %02X", data[i]);
  }
  printf(" (%zu bytes)\n", len);
  printf("  crc: %04llX\n", (unsigned long long)crc);
}

/* ---- Replies: write into the same frame, encode, transmit ---------------------------------- */

static void __send_ack(uint8_t code) {
  uint8_t out[16];
  size_t length = 0;

  if (framedsl_set_u64(storage, ids.ack_sequence, device.sequence) != FRAMEDSL_OK
      || framedsl_set_u64(storage, ids.ack_result, code) != FRAMEDSL_OK) {
    return;
  }

  /* The packet name is a string here and an id everywhere else -- encode resolves it per call. */
  if (framedsl_encode_into(storage, "TM_ACK", out, sizeof(out), &length) != FRAMEDSL_OK) {
    return;
  }
  uart_transmit(out, length);
}

static void __send_hk(void) {
  static uint32_t reading = 0;
  FramedslStatus status;
  size_t length = 0;

  reading += 17; /* whatever the ADC would say */

  if (framedsl_set_u64(storage, ids.hk_sequence, device.sequence) != FRAMEDSL_OK
      || framedsl_set_u64(storage, ids.hk_channel, device.active_channel) != FRAMEDSL_OK
      || framedsl_set_u64(storage, ids.hk_reading, reading) != FRAMEDSL_OK
      || framedsl_set_u64(storage, ids.hk_limit, device.limit) != FRAMEDSL_OK) {
    return;
  }

  const uint8_t *packet = framedsl_encode(storage, "TM_HK", &length, &status);
  if (status != FRAMEDSL_OK) {
    return;
  }

  uart_transmit(packet, length);
}

/* ---- The RX callback ------------------------------------------------------------------------ */

int uart_receive_pkt(const uint8_t *data, uint32_t size) {
  FramedslStatus status = FRAMEDSL_OK;
  size_t consumed = 0;

  const char *pkt_name = framedsl_decode(storage, data, size, &consumed, &status);

  /* NULL is the ordinary case on a real link: noise, a truncated frame, a sender ahead of this
   * .fdsl. Your sketch went straight into strcmp(), which segfaults on the first bad byte. */
  if (pkt_name == NULL) {
    fprintf(stderr, "  uart: %u bytes that are not a packet (status %d)\n", size, (int)status);
    return -1;
  }

  printf("  received %s (%zu of %u bytes)\n", pkt_name, consumed, size);

  if (strcmp(pkt_name, "TC_HK_REQ") == 0) {
    __handle_tc_hk_req();
    __send_hk();
  } else if (strcmp(pkt_name, "TC_SET_LIMIT") == 0) {
    __handle_tc_set_limit();
    __send_ack(0);
  } else if (strcmp(pkt_name, "TC_WRITE") == 0) {
    __handle_tc_write();
    __send_ack(1);
  } else {
    /* A packet this build has no handler for. Not acked: the sequence in `device` belongs to some
     * earlier command, and acking with it is worse than staying quiet. */
    fprintf(stderr, "  uart: no handler for %s\n", pkt_name);
    return -1;
  }

  /* More than one frame can arrive in one read. Walking them is this call in a loop, advancing
   * `data` by `consumed` each time -- which is the only way to read a packet whose length is a
   * field inside it. */
  if (consumed < size) {
    printf("  uart: %zu bytes left over, another frame is waiting\n", size - consumed);
  }

  return 0;
}

/* ---- The pretend link ------------------------------------------------------------------------
 *
 * Everything above is the driver. This is the part a real program does not have: three frames handed
 * to the RX callback by hand, standing in for bytes arriving on a wire. Delete it, call uart_init()
 * from your own startup, and feed uart_receive_pkt() from a real read().
 */

int main(int argc, char **argv) {
  /* So the error lines land between the right two stdout lines when the output is piped. */
  setvbuf(stdout, NULL, _IONBF, 0);

  if (uart_init(argc > 1 ? argv[1] : NULL) != 0) {
    return 1;
  }

  /* TC_SET_LIMIT: id 0x02, sequence 1, channel 3, limit 4200 (0x1068). */
  static const uint8_t SET_LIMIT[] = { 0x02, 0x00, 0x01, 0x03, 0x00, 0x00, 0x10, 0x68 };

  /* TC_SET_LIMIT: id 0x02, sequence 1, channel 3, limit 4200 (0x1068). */
  static const uint8_t WRITE[] = {
    0x03,// id
    0x04,// size
    0x01, 0x02, 0x03, 0x04,// data
    0xAA, 0xBB,// crc
  };

  /* TC_HK_REQ: id 0x01, sequence 2. */
  static const uint8_t HK_REQ[] = { 0x01, 0x00, 0x02 };
  static const uint8_t NOISE[] = { 0xFF, 0xEE, 0xDD };

  printf("TC_SET_LIMIT:\n");
  uart_receive_pkt(SET_LIMIT, sizeof(SET_LIMIT));

  printf("TC_WRITE:\n");
  uart_receive_pkt(WRITE, sizeof(WRITE));

  printf("TC_HK_REQ:\n");
  uart_receive_pkt(HK_REQ, sizeof(HK_REQ));

  printf("noise:\n");
  uart_receive_pkt(NOISE, sizeof(NOISE));

  uart_deinit();
  return 0;
}
