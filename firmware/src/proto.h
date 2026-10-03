#pragma once
#include <stdbool.h>

// Line protocol over USB Serial/JTAG. All device->host lines start with '@'.
//
//   host: PING                         dev: @PONG v1 <0|1 provisioned>
//   host: PROV <64 hex>                dev: @PROVOK | @PROVERR <why>     (only while unprovisioned)
//   host: REQ <nonce32hex> <secs> <b64(user\ncwd\ncommand)>
//   dev : @OK <nonce> <hmac-sha256-hex>   approved
//         @NO <nonce> <reason>            denied / timeout / busy / unprovisioned / badreq
//   hmac = HMAC-SHA256(secret, "sudo-btn-v1\n" + nonce + "\n" + decoded payload)

void proto_init(void); // loads secret, starts serial task; call after ui_start
void proto_result(bool approved, const char *reason); // ui result callback
void proto_forget(void);                              // ui forget callback
