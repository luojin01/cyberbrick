/*
 * Minimal HTTP + WebSocket server for Cyberbrick L-ONE.
 *
 * Endpoints:
 *   GET  /          – control UI (HTML)
 *   GET  /api/status – JSON status (compatibility)
 *   POST /api/servo  – {id, value}  (compatibility)
 *   POST /api/motor  – {direction}  (compatibility)
 *   GET  /ws        – WebSocket upgrade; carries JSON command messages:
 *                       {"cmd":"servo","id":N,"value":V}
 *                       {"cmd":"motor","direction":"forward|reverse|stop"}
 *                       {"cmd":"status"}   -> server replies with status JSON
 *
 * WebSocket framing implemented inline; SHA-1 + base64 for the handshake
 * are vendored below so we don't pull in any extra modules.
 */

#include "web_server.h"
#include "servo.h"
#include "dc_motor.h"

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

LOG_MODULE_REGISTER(web_server, LOG_LEVEL_INF);

#define WEB_STACK_SIZE  8192
#define WEB_PRIORITY    5
#define RECV_BUF_SIZE   1024
#define WS_MAX_PAYLOAD  256

/* ── Embedded HTML / JS ─────────────────────────────────────────────────────── */

static const char html_page[] =
"<!DOCTYPE html>"
"<html lang='en'><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Cyberbrick L-ONE</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#111;color:#eee;font-family:system-ui,sans-serif;padding:16px}"
"h1{text-align:center;color:#4fc3f7;margin-bottom:24px;font-size:1.4rem;letter-spacing:2px}"
".card{background:#1e1e1e;border-radius:12px;padding:20px;margin-bottom:16px}"
".card h2{color:#80cbc4;font-size:.9rem;letter-spacing:1px;margin-bottom:16px;text-transform:uppercase}"
".servo-row{display:flex;align-items:center;gap:12px;margin-bottom:12px}"
".servo-row label{width:60px;font-size:.85rem;color:#aaa}"
".servo-row input[type=range]{flex:1;accent-color:#4fc3f7}"
".servo-row .val{width:36px;text-align:right;font-size:.85rem;color:#4fc3f7}"
".motor-row{display:flex;gap:8px;justify-content:center}"
"button{padding:10px 20px;border:none;border-radius:8px;cursor:pointer;font-size:.9rem;font-weight:600;transition:opacity .15s}"
"button:active{opacity:.7}"
".btn-fwd{background:#4fc3f7;color:#000}"
".btn-stop{background:#555;color:#eee}"
".btn-rev{background:#ef9a9a;color:#000}"
".status{text-align:center;font-size:.75rem;color:#666;margin-top:8px}"
"#motor-status{color:#80cbc4;font-weight:600}"
"#ws-status{position:fixed;top:6px;right:8px;font-size:.7rem;color:#888}"
"#ws-status.up{color:#80cbc4}"
"#ws-status.down{color:#ef9a9a}"
"</style>"
"</head><body>"
"<div id='ws-status' class='down'>connecting…</div>"
"<h1>CYBERBRICK L-ONE</h1>"
"<div class='card'>"
"<h2>Servos</h2>"
"<div class='servo-row'>"
"<label>Servo 1</label>"
"<input type='range' min='-100' max='100' value='0' oninput=\"setServo(0,this.value);document.getElementById('s0v').textContent=this.value\">"
"<span class='val' id='s0v'>0</span>"
"</div>"
"<div class='servo-row'>"
"<label>Servo 2</label>"
"<input type='range' min='-100' max='100' value='0' oninput=\"setServo(1,this.value);document.getElementById('s1v').textContent=this.value\">"
"<span class='val' id='s1v'>0</span>"
"</div>"
"<div class='servo-row'>"
"<label>Servo 3</label>"
"<input type='range' min='0' max='180' value='90' oninput=\"setServo(2,this.value);document.getElementById('s2v').textContent=this.value+'°'\">"
"<span class='val' id='s2v'>90°</span>"
"</div>"
"</div>"
"<div class='card'>"
"<h2>DC Motor</h2>"
"<div class='motor-row'>"
"<button class='btn-fwd' onclick=\"setMotor('forward')\">&#9650; Forward</button>"
"<button class='btn-stop' onclick=\"setMotor('stop')\">&#9632; Stop</button>"
"<button class='btn-rev' onclick=\"setMotor('reverse')\">&#9660; Reverse</button>"
"</div>"
"<div class='status'>Motor: <span id='motor-status'>STOP</span></div>"
"</div>"
"<script>"
"let ws=null;"
"let last={};"
"const ind=document.getElementById('ws-status');"
"function connect(){"
"  ws=new WebSocket('ws://'+location.host+'/ws');"
"  ws.onopen=()=>{ind.textContent='connected';ind.className='up'};"
"  ws.onclose=()=>{ind.textContent='disconnected';ind.className='down';setTimeout(connect,1000)};"
"  ws.onerror=()=>{ind.textContent='error';ind.className='down'};"
"}"
"connect();"
"function send(o){if(ws&&ws.readyState===1)ws.send(JSON.stringify(o))}"
"function setServo(id,v){"
"  const now=Date.now();"
"  if(now-(last[id]||0)<25)return;"
"  last[id]=now;"
"  send({cmd:'servo',id,value:parseInt(v)});"
"}"
"function setMotor(dir){"
"  send({cmd:'motor',direction:dir});"
"  document.getElementById('motor-status').textContent=dir.toUpperCase();"
"}"
"</script>"
"</body></html>";

/* ── JSON helpers ───────────────────────────────────────────────────────────── */

/* Find "key" then return the integer that follows ':'. Returns 0 on success. */
static int json_get_int(const char *body, const char *key, int *out)
{
    char search[32];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(body, search);
    if (!p) {
        return -1;
    }
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

/* Return pointer to value string inside a "key":"value" pair (not terminated). */
static const char *json_get_str(const char *body, const char *key, size_t *out_len)
{
    char search[32];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(body, search);
    if (!p) {
        return NULL;
    }
    p += strlen(search);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') {
        return NULL;
    }
    p++;
    const char *start = p;
    while (*p && *p != '"') p++;
    *out_len = p - start;
    return start;
}

/* ── Command dispatch ───────────────────────────────────────────────────────── */

/*
 * Returns negative on hard error (caller may close), 0 on success.
 * On success, fills resp_out (NUL-terminated) with a small JSON reply.
 */
static int do_servo(const char *body, char *resp_out, size_t resp_sz)
{
    int id = -1, value = 0;
    bool have_value;

    if (json_get_int(body, "id", &id) < 0 || id < 0 || id >= SERVO_COUNT) {
        snprintf(resp_out, resp_sz, "{\"error\":\"bad id\"}");
        return 0;
    }

    have_value = (json_get_int(body, "value", &value) == 0) ||
                 (json_get_int(body, "angle", &value) == 0) ||
                 (json_get_int(body, "speed", &value) == 0);
    if (!have_value) {
        snprintf(resp_out, resp_sz, "{\"error\":\"missing value\"}");
        return 0;
    }

    if (servo_get_type((uint8_t)id) == SERVO_TYPE_CONTINUOUS) {
        if (value < -100) value = -100;
        if (value >  100) value =  100;
        if (servo_set_speed((uint8_t)id, (int8_t)value) < 0) {
            snprintf(resp_out, resp_sz, "{\"error\":\"set failed\"}");
            return 0;
        }
        snprintf(resp_out, resp_sz, "{\"id\":%d,\"speed\":%d}", id, value);
    } else {
        if (value < 0)   value = 0;
        if (value > 180) value = 180;
        if (servo_set_angle((uint8_t)id, (uint8_t)value) < 0) {
            snprintf(resp_out, resp_sz, "{\"error\":\"set failed\"}");
            return 0;
        }
        snprintf(resp_out, resp_sz, "{\"id\":%d,\"angle\":%d}", id, value);
    }
    return 0;
}

static int do_motor(const char *body, char *resp_out, size_t resp_sz)
{
    size_t vlen;
    const char *val = json_get_str(body, "direction", &vlen);
    if (!val) {
        snprintf(resp_out, resp_sz, "{\"error\":\"missing direction\"}");
        return 0;
    }

    motor_direction_t dir;
    if (vlen == 7 && strncmp(val, "forward", 7) == 0) {
        dir = MOTOR_FORWARD;
    } else if (vlen == 7 && strncmp(val, "reverse", 7) == 0) {
        dir = MOTOR_REVERSE;
    } else if (vlen == 4 && strncmp(val, "stop", 4) == 0) {
        dir = MOTOR_STOP;
    } else {
        snprintf(resp_out, resp_sz, "{\"error\":\"bad direction\"}");
        return 0;
    }

    if (dc_motor_set(dir) < 0) {
        snprintf(resp_out, resp_sz, "{\"error\":\"motor failed\"}");
        return 0;
    }

    const char *dname = (dir == MOTOR_FORWARD) ? "forward" :
                        (dir == MOTOR_REVERSE) ? "reverse" : "stop";
    snprintf(resp_out, resp_sz, "{\"direction\":\"%s\"}", dname);
    return 0;
}

static void build_status_json(char *out, size_t out_sz)
{
    const char *dir_str;
    switch (dc_motor_get()) {
    case MOTOR_FORWARD: dir_str = "forward"; break;
    case MOTOR_REVERSE: dir_str = "reverse"; break;
    default:            dir_str = "stop";    break;
    }
    int v0 = (servo_get_type(0) == SERVO_TYPE_CONTINUOUS)
                 ? servo_get_speed(0) : servo_get_angle(0);
    int v1 = (servo_get_type(1) == SERVO_TYPE_CONTINUOUS)
                 ? servo_get_speed(1) : servo_get_angle(1);
    int v2 = (servo_get_type(2) == SERVO_TYPE_CONTINUOUS)
                 ? servo_get_speed(2) : servo_get_angle(2);

    snprintf(out, out_sz,
        "{\"servos\":[%d,%d,%d],\"motor\":\"%s\"}",
        v0, v1, v2, dir_str);
}

/*
 * Process a JSON control message (e.g. from a WebSocket text frame or HTTP body)
 * and write the JSON reply into resp_out.
 */
static void dispatch_json(const char *body, char *resp_out, size_t resp_sz)
{
    size_t clen;
    const char *cmd = json_get_str(body, "cmd", &clen);
    if (cmd) {
        if (clen == 5 && strncmp(cmd, "servo", 5) == 0) {
            do_servo(body, resp_out, resp_sz);
            return;
        }
        if (clen == 5 && strncmp(cmd, "motor", 5) == 0) {
            do_motor(body, resp_out, resp_sz);
            return;
        }
        if (clen == 6 && strncmp(cmd, "status", 6) == 0) {
            build_status_json(resp_out, resp_sz);
            return;
        }
    }
    snprintf(resp_out, resp_sz, "{\"error\":\"unknown cmd\"}");
}

/* ── HTTP helpers ───────────────────────────────────────────────────────────── */

static void send_response(int sock, int code, const char *ctype,
                          const char *body, size_t body_len)
{
    char hdr[256];
    const char *code_str = (code == 200) ? "OK" : "Bad Request";

    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, code_str, ctype, body_len);

    send(sock, hdr, hlen, 0);
    if (body && body_len) {
        send(sock, body, body_len, 0);
    }
}

static void send_json(int sock, int code, const char *json)
{
    send_response(sock, code, "application/json", json, strlen(json));
}

/* ── SHA-1 (RFC 3174) — used for the WebSocket handshake ────────────────────── */

static inline uint32_t rol32(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1_buf(const uint8_t *msg, size_t len, uint8_t out[20])
{
    /* Handshake input is ~60 bytes — pad to one or two 64-byte blocks. */
    uint8_t buf[128];
    size_t padded = ((len + 9 + 63) / 64) * 64;
    if (padded > sizeof(buf)) {
        memset(out, 0, 20);
        return;
    }

    memcpy(buf, msg, len);
    buf[len] = 0x80;
    memset(buf + len + 1, 0, padded - len - 1);

    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        buf[padded - 1 - i] = (uint8_t)(bits >> (8 * i));
    }

    uint32_t h[5] = {
        0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0
    };

    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)buf[off + 4*i]     << 24) |
                   ((uint32_t)buf[off + 4*i + 1] << 16) |
                   ((uint32_t)buf[off + 4*i + 2] <<  8) |
                   ((uint32_t)buf[off + 4*i + 3]);
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol32(b, 30);
            b = a;
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    for (int i = 0; i < 5; i++) {
        out[4*i]   = (uint8_t)(h[i] >> 24);
        out[4*i+1] = (uint8_t)(h[i] >> 16);
        out[4*i+2] = (uint8_t)(h[i] >>  8);
        out[4*i+3] = (uint8_t)(h[i]);
    }
}

/* ── base64 encoder (no padding stripping) ──────────────────────────────────── */

static void base64_encode(const uint8_t *in, size_t len, char *out)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) |
                     ((uint32_t)in[i+1] << 8) |
                     ((uint32_t)in[i+2]);
        out[o++] = tbl[(v >> 18) & 0x3F];
        out[o++] = tbl[(v >> 12) & 0x3F];
        out[o++] = tbl[(v >>  6) & 0x3F];
        out[o++] = tbl[v & 0x3F];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i+1] << 8;
        out[o++] = tbl[(v >> 18) & 0x3F];
        out[o++] = tbl[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
}

/* ── WebSocket transport ────────────────────────────────────────────────────── */

#define WS_MAGIC "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* Case-insensitive header extraction: copy header value into dst (NUL-terminated). */
static int header_get(const char *req, const char *name, char *dst, size_t dst_sz)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\r\n%s:", name);

    const char *p = strcasestr(req, needle);
    if (!p) {
        return -1;
    }
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;

    const char *end = strstr(p, "\r\n");
    if (!end) {
        return -1;
    }
    size_t n = (size_t)(end - p);
    if (n >= dst_sz) {
        n = dst_sz - 1;
    }
    memcpy(dst, p, n);
    dst[n] = '\0';
    return 0;
}

/* Returns true if request wants a WebSocket upgrade on the given path. */
static bool is_ws_upgrade(const char *req, const char *path)
{
    char upgrade[32], conn[64];

    if (strcmp(path, "/ws") != 0) {
        return false;
    }
    if (header_get(req, "Upgrade", upgrade, sizeof(upgrade)) < 0 ||
        header_get(req, "Connection", conn, sizeof(conn)) < 0) {
        return false;
    }
    if (strcasecmp(upgrade, "websocket") != 0) {
        return false;
    }
    /* "Connection" may be a comma-separated list */
    return strcasestr(conn, "upgrade") != NULL;
}

static int ws_send_handshake(int sock, const char *req)
{
    char key[64];
    if (header_get(req, "Sec-WebSocket-Key", key, sizeof(key)) < 0) {
        LOG_WRN("WS handshake: missing Sec-WebSocket-Key");
        return -1;
    }

    /* concat = base64key + GUID */
    char concat[sizeof(key) + sizeof(WS_MAGIC)];
    size_t klen = strlen(key);
    size_t mlen = strlen(WS_MAGIC);
    if (klen + mlen >= sizeof(concat)) {
        return -1;
    }
    memcpy(concat, key, klen);
    memcpy(concat + klen, WS_MAGIC, mlen);

    uint8_t sha[20];
    sha1_buf((const uint8_t *)concat, klen + mlen, sha);

    char accept[32];
    base64_encode(sha, 20, accept);

    char resp[200];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n",
        accept);
    if (send(sock, resp, rlen, 0) != rlen) {
        return -1;
    }
    LOG_INF("WS client connected");
    return 0;
}

/* Blocking read of exactly n bytes. */
static int recv_n(int sock, void *buf, size_t n)
{
    size_t got = 0;
    uint8_t *p = (uint8_t *)buf;
    while (got < n) {
        int r = recv(sock, p + got, n - got, 0);
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return (int)got;
}

#define WS_OP_CONT   0x0
#define WS_OP_TEXT   0x1
#define WS_OP_BIN    0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

static int ws_send_frame(int sock, uint8_t opcode, const uint8_t *data, size_t len);

/* Reads one frame. Returns payload length (text/bin) or:
 *   0  if frame was handled internally (ping → pong, etc.)
 *  -1  on I/O / protocol error
 *  -2  on close frame
 *  -3  if payload exceeded buf
 */
static int ws_recv_frame(int sock, uint8_t *buf, size_t bufsize, uint8_t *opcode_out)
{
    uint8_t hdr[2];
    if (recv_n(sock, hdr, 2) < 0) {
        return -1;
    }

    bool fin    = (hdr[0] & 0x80) != 0;
    uint8_t op  =  hdr[0] & 0x0F;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7F;

    if (!fin) {
        LOG_WRN("WS: fragmented frames not supported");
        return -1;
    }

    if (len == 126) {
        uint8_t e[2];
        if (recv_n(sock, e, 2) < 0) return -1;
        len = ((uint64_t)e[0] << 8) | e[1];
    } else if (len == 127) {
        uint8_t e[8];
        if (recv_n(sock, e, 8) < 0) return -1;
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | e[i];
        }
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (recv_n(sock, mask, 4) < 0) return -1;
    }

    if (len > bufsize) {
        LOG_WRN("WS: payload %llu > buf %zu", (unsigned long long)len, bufsize);
        /* Drain and reject */
        uint8_t junk[64];
        while (len > 0) {
            size_t chunk = len > sizeof(junk) ? sizeof(junk) : (size_t)len;
            if (recv_n(sock, junk, chunk) < 0) return -1;
            len -= chunk;
        }
        return -3;
    }

    if (len > 0) {
        if (recv_n(sock, buf, (size_t)len) < 0) return -1;
        if (masked) {
            for (uint64_t i = 0; i < len; i++) {
                buf[i] ^= mask[i & 3];
            }
        }
    }

    *opcode_out = op;

    if (op == WS_OP_CLOSE) {
        ws_send_frame(sock, WS_OP_CLOSE, NULL, 0);
        return -2;
    }
    if (op == WS_OP_PING) {
        ws_send_frame(sock, WS_OP_PONG, buf, (size_t)len);
        return 0;
    }
    if (op != WS_OP_TEXT && op != WS_OP_BIN) {
        return 0;
    }

    return (int)len;
}

static int ws_send_frame(int sock, uint8_t opcode, const uint8_t *data, size_t len)
{
    uint8_t hdr[10];
    size_t hlen;

    hdr[0] = 0x80 | (opcode & 0x0F);   /* FIN + opcode */
    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hlen = 2;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len);
        hlen = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++) {
            hdr[2 + i] = (uint8_t)(len >> (56 - 8 * i));
        }
        hlen = 10;
    }

    if (send(sock, hdr, hlen, 0) != (int)hlen) {
        return -1;
    }
    if (len > 0 && send(sock, data, len, 0) != (int)len) {
        return -1;
    }
    return 0;
}

static int ws_send_text(int sock, const char *s)
{
    return ws_send_frame(sock, WS_OP_TEXT, (const uint8_t *)s, strlen(s));
}

/* Run a WebSocket session until the peer closes or an error occurs. */
static void ws_session(int sock)
{
    /* No recv timeout for persistent connections. */
    struct timeval tv = {0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Send initial status snapshot so client UI can sync. */
    char status[160];
    build_status_json(status, sizeof(status));
    ws_send_text(sock, status);

    uint8_t payload[WS_MAX_PAYLOAD + 1];
    char    reply[160];

    while (true) {
        uint8_t op;
        int n = ws_recv_frame(sock, payload, sizeof(payload) - 1, &op);
        if (n < 0) {
            if (n == -2) {
                LOG_INF("WS client closed");
            } else {
                LOG_WRN("WS rx error %d", n);
            }
            break;
        }
        if (n == 0 || op != WS_OP_TEXT) {
            /* Ping/pong/etc. handled inside ws_recv_frame, or non-text frame. */
            continue;
        }
        payload[n] = '\0';
        LOG_DBG("WS rx: %s", (char *)payload);

        dispatch_json((const char *)payload, reply, sizeof(reply));
        if (ws_send_text(sock, reply) < 0) {
            LOG_WRN("WS send failed");
            break;
        }
    }
}

/* ── Plain HTTP request handlers (compat / page load) ───────────────────────── */

static void handle_servo_http(int sock, const char *body)
{
    char resp[64];
    do_servo(body, resp, sizeof(resp));
    send_json(sock, 200, resp);
}

static void handle_motor_http(int sock, const char *body)
{
    char resp[48];
    do_motor(body, resp, sizeof(resp));
    send_json(sock, 200, resp);
}

static void handle_status_http(int sock)
{
    char resp[160];
    build_status_json(resp, sizeof(resp));
    send_json(sock, 200, resp);
}

/* ── Connection dispatcher ──────────────────────────────────────────────────── */

/*
 * Returns:
 *   true  – caller should close the socket after we return (normal HTTP)
 *   false – caller MUST NOT close (we already handled lifecycle, e.g. WS)
 *           — but we still close on our own once the WS session ends.
 */
static void handle_client(int sock)
{
    static char buf[RECV_BUF_SIZE];
    int received = recv(sock, buf, sizeof(buf) - 1, 0);
    if (received <= 0) {
        return;
    }
    buf[received] = '\0';

    char method[8], path[64];
    if (sscanf(buf, "%7s %63s", method, path) < 2) {
        return;
    }

    /* WebSocket upgrade? */
    if (is_ws_upgrade(buf, path)) {
        if (ws_send_handshake(sock, buf) < 0) {
            send_json(sock, 400, "{\"error\":\"bad ws handshake\"}");
            return;
        }
        ws_session(sock);
        return;
    }

    const char *body = strstr(buf, "\r\n\r\n");
    body = body ? body + 4 : "";

    LOG_DBG("%s %s", method, path);

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        send_response(sock, 200, "text/html; charset=utf-8",
                      html_page, sizeof(html_page) - 1);
    } else if (strcmp(path, "/api/servo") == 0 && strcmp(method, "POST") == 0) {
        handle_servo_http(sock, body);
    } else if (strcmp(path, "/api/motor") == 0 && strcmp(method, "POST") == 0) {
        handle_motor_http(sock, body);
    } else if (strcmp(path, "/api/status") == 0) {
        handle_status_http(sock);
    } else {
        send_json(sock, 404, "{\"error\":\"not found\"}");
    }
}

/* ── Server thread ──────────────────────────────────────────────────────────── */

static void web_server_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    int server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock < 0) {
        LOG_ERR("socket() failed: %d", errno);
        return;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(CONFIG_CYBERBRICK_WEB_PORT),
        .sin_addr   = { INADDR_ANY },
    };

    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("bind() failed: %d", errno);
        close(server_sock);
        return;
    }

    if (listen(server_sock, 3) < 0) {
        LOG_ERR("listen() failed: %d", errno);
        close(server_sock);
        return;
    }

    LOG_INF("HTTP/WebSocket server listening on port %d",
            CONFIG_CYBERBRICK_WEB_PORT);

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client = accept(server_sock, (struct sockaddr *)&client_addr,
                            &client_len);
        if (client < 0) {
            LOG_WRN("accept() failed: %d", errno);
            k_sleep(K_MSEC(10));
            continue;
        }

        /* 2 s recv timeout for the initial request line.
         * ws_session() clears this once the connection is upgraded. */
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handle_client(client);
        close(client);
    }
}

/* Defer thread start until web_server_start() is called (after DHCP). */
K_THREAD_STACK_DEFINE(web_stack, WEB_STACK_SIZE);
static struct k_thread web_thread_data;
static k_tid_t web_tid;

int web_server_start(void)
{
    if (web_tid) {
        LOG_WRN("Web server already started");
        return 0;
    }

    web_tid = k_thread_create(&web_thread_data, web_stack,
                              K_THREAD_STACK_SIZEOF(web_stack),
                              web_server_thread, NULL, NULL, NULL,
                              WEB_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(web_tid, "web_server");
    LOG_INF("Web server thread started");
    return 0;
}
