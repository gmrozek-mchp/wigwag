/*
 * RNWF02 AT wire vocabulary — the only place in the firmware that knows the module's syntax.
 *
 * Every string, parameter ID and length limit below is taken from:
 *
 *   AT Command Specification, Network Controller 3.1.0
 *   Revision 58a15dc2, August 19, 2025
 *   ww1.microchip.com/downloads/aemDocuments/documents/WSG/ProductDocuments/
 *     SupportingCollateral/AT-Command-Specification-v3.1.0.pdf
 *
 * cross-checked against the transcript in RNWF02 Supplemental User Guide v3.0.0.
 *
 * Nothing here is inferred from the Harmony 3 C wrapper (RNWF_MQTT_SrvCtrl and friends), which
 * documents semantics but not wire text, and nothing is borrowed from the RNWF11 guide. If a
 * value has no citation it does not belong in this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RNWF_AT_CMDS_H
#define RNWF_AT_CMDS_H

/*
 * Framing — spec "Commands and Responses" and "Asynchronous Event Codes".
 *
 * Four details that are easy to get wrong and were all verified rather than assumed:
 *
 *  1. A command line is completed by CR LF, not CR alone.
 *  2. Every response is <RESPONSE> CR LF.
 *  3. An AEC is prefixed with a *leading* CR: <CR>+AECNAME:INFO<CR><LF>. The leading CR exists
 *     "to clearly identify the start of the AEC", so the line assembler must treat a bare CR as
 *     a delimiter and tolerate empty lines rather than mistaking one for a malformed response.
 *  4. The success/error text depends on the ATV verbosity level, so "ERROR" is *not* a safe
 *     thing to match on until verbosity is pinned. RNWF_AT_SET_VERBOSITY does that first.
 *
 * Also load-bearing for the state machine: AECs are never sent *during* command execution, but
 * may arrive while the host is transmitting. And a command may return OK (accepted) and then
 * fail asynchronously as "+CMDNAME:ERROR:<code>" — e.g. "+SOCKBR:ERROR:4" in the spec. So
 * success on a request is not success of the operation.
 */
#define RNWF_AT_EOL		"\r\n"
#define RNWF_AT_OK		"OK"
#define RNWF_AT_ERROR		"ERROR"	/* prefix only; ATV3 appends ":<STATUS_CODE>" */

/*
 * Verbosity level 3 = "OK for success, ERROR:<STATUS_CODE> for error" (spec, ATV).
 * Chosen over level 2 so failures carry a machine-readable code, and over levels 4 and 5 so we
 * never have to parse vendor prose. Set explicitly at startup: the default is not specified.
 */
#define RNWF_AT_SET_VERBOSITY	"ATV3"

/*
 * Turn off command echo (spec "Serial Interface / Basic Commands / E": ATE<N>, 0 = "Turn off
 * character echo", page 18).
 *
 * Sent before anything else, because a module with echo *on* — which is the state after every reset,
 * as the setting does not persist — replays each command we write straight back at us. Verified on
 * hardware on both 2.0.0 and 3.1.0. Three consequences, worst first:
 *
 *  1. Every echoed command arrives as an unparseable line, so lines_dropped climbs on a *healthy*
 *     run. That counter exists to mean "something is wrong" (D77), and echo made it lie.
 *  2. It doubles receive traffic during a command burst, on the ring this firmware sizes carefully.
 *  3. It is what produces the bare ">" prompt character the module emits after each response, with
 *     no CR or LF, which otherwise prefixes the *next* assembled line. That prompt appears nowhere
 *     in the specification — searched, not assumed — so nothing should be written to depend on it.
 */
#define RNWF_AT_ECHO_OFF	"ATE0"

/* System (spec: +RST, +GMR, +BOOT). */
#define RNWF_AT_RESET		"AT+RST"
#define RNWF_AT_GET_VERSION	"AT+GMR"	/* D62: confirm firmware >= 3.0 for +CFGCP */

/*
 * Bare "AT" as a liveness probe: the cheapest command that must produce a final result code. Used
 * as the keepalive while linked, because a module that stops answering is the only way to detect a
 * silent death over a UART with no carrier signal (D75).
 */
#define RNWF_AT_PING		"AT"

/*
 * Scan for networks (spec: +WSCN, module ID 19). "AT+WSCN=<ACT_PASV>", 1 = active scanning.
 *
 * Results arrive as +WSCNIND AECs, one per network, each carrying
 * "<RSSI>,<SEC_TYPE>,<CHANNEL>,<BSSID>,<SSID>", and the scan ends with +WSCNDONE. OK to the command
 * means only that scanning started, which is why the step waits for the AEC.
 *
 * Worth knowing before reading a scan: this module is **2.4 GHz only**. The datasheet says 802.11
 * b/g/n, and the specification confirms it structurally -- the only channel masks that exist are
 * <CHANMASK24> (+WSCNC ID 11) and <REGDOMAIN_CHANMASK24> (ID 12). A 5 GHz network cannot appear here.
 */
#define RNWF_AT_SCAN		"AT+WSCN"
#define RNWF_AEC_SCAN_RESULT	"+WSCNIND"	/* :<RSSI>,<SEC_TYPE>,<CHANNEL>,<BSSID>,<SSID> */
#define RNWF_AEC_SCAN_DONE	"+WSCNDONE"

/* Wi-Fi station config, "AT+WSTAC=<ID>,<VAL>" (spec: +WSTAC). */
#define RNWF_AT_WSTAC		"AT+WSTAC"
#define RNWF_AT_WSTA_ENABLE	"AT+WSTA=1"	/* 1 = use configuration from +WSTAC */
#define RNWF_AT_WSTA_DISABLE	"AT+WSTA=0"

enum rnwf_wstac_id {
	RNWF_WSTAC_SSID		= 1,	/* String,  max 32  */
	RNWF_WSTAC_SEC_TYPE	= 2,	/* Integer, see below */
	RNWF_WSTAC_CREDENTIALS	= 3,	/* String,  max 128 */
	RNWF_WSTAC_CHANNEL	= 4,	/* Integer, 0-13, 0 = any */
	RNWF_WSTAC_BSSID	= 5,	/* MAC address */
	RNWF_WSTAC_CONN_TIMEOUT	= 7,	/* Integer, milliseconds */
	RNWF_WSTAC_NETIF_IDX	= 8,	/* Unsigned */
};

/* +WSTAC ID 2 values. Note 1 is absent from the spec's table — do not invent it. */
enum rnwf_sec_type {
	RNWF_SEC_OPEN			= 0,
	RNWF_SEC_WPA2_MIXED		= 2,
	RNWF_SEC_WPA2_PERSONAL		= 3,
	RNWF_SEC_WPA3_TRANSITION	= 4,
	RNWF_SEC_WPA3_PERSONAL		= 5,
};

/* MQTT config, "AT+MQTTC=<ID>,<VAL>" (spec: +MQTTC, module ID 8). */
#define RNWF_AT_MQTTC		"AT+MQTTC"
#define RNWF_AT_MQTT_CONNECT	"AT+MQTTCONN=1"	/* <CLEAN>: 1 = new session */
#define RNWF_AT_MQTT_DISCONNECT	"AT+MQTTDISCONN"
#define RNWF_AT_MQTT_SUB	"AT+MQTTSUB"	/* =<TOPIC_NAME>,<MAX_QOS> */
#define RNWF_AT_MQTT_PUB	"AT+MQTTPUB"	/* =<DUP>,<QOS>,<RETAIN>,<TOPIC>,<PAYLOAD> */
#define RNWF_AT_MQTT_LWT	"AT+MQTTLWT"	/* =<QOS>,<RETAIN>,<TOPIC>,<PAYLOAD> */

enum rnwf_mqttc_id {
	RNWF_MQTTC_BROKER_ADDR	= 1,	/* String,  max 64. ADR-0013: a hostname, not an IP */
	RNWF_MQTTC_BROKER_PORT	= 2,	/* Integer, uint16 */
	RNWF_MQTTC_CLIENT_ID	= 3,	/* String,  max 48 */
	RNWF_MQTTC_USERNAME	= 4,	/* String,  max 128 */
	RNWF_MQTTC_PASSWORD	= 5,	/* String,  max 256 */
	RNWF_MQTTC_KEEP_ALIVE	= 6,	/* Integer, 0..0x7FFF seconds */
	RNWF_MQTTC_TLS_CONF	= 7,	/* Integer, 0..2; 0 disables TLS (see +TLSC) */
	RNWF_MQTTC_PROTO_VER	= 8,	/* 3 = v3.1.1, 5 = v5 */
	RNWF_MQTTC_READ_THRESHOLD = 9,	/* Integer, uint16 */
};

/*
 * Asynchronous event codes we act on. Names and field orders are verbatim from the spec's AEC
 * sections; the parser matches on the name and never on field position alone.
 */
#define RNWF_AEC_BOOT		"+BOOT"		/* :<BANNER> */
#define RNWF_AEC_WSTA_LINK_UP	"+WSTALU"	/* :<ASSOC_ID>,<BSSID>,<CHANNEL> */
#define RNWF_AEC_WSTA_LINK_DOWN	"+WSTALD"	/* :<ASSOC_ID> */
#define RNWF_AEC_WSTA_ERROR	"+WSTAERR"	/* :<ERROR_CODE> */
#define RNWF_AEC_WSTA_GOT_IP	"+WSTAAIP"	/* :<ASSOC_ID>,<IP_ADDRESS> */
#define RNWF_AEC_MQTT_CONNACK	"+MQTTCONNACK"	/* :<CONNACK_FLAGS>,<CONN_REASON_CODE> */
#define RNWF_AEC_MQTT_SUBRX	"+MQTTSUBRX"	/* :<DUP>,<QOS>,<RETAIN>,<TOPIC>,<PAYLOAD> */

/*
 * "+MQTTSUB:<REASON_CODE>" — the **SUBACK**, and the reason a subscribe is not finished when its OK
 * arrives (spec AEC reference: 0/1/2 granted QoS, 128 unspecified error).
 *
 * Learned on hardware, with the broker's log as the witness: sending the next AT+MQTTSUB while the
 * previous SUBACK is still outstanding gets the second command refused by the *module* with
 * ERROR:8.0, and it never reaches the broker at all — mosquitto logged one SUBSCRIBE, one SUBACK,
 * then nothing until the keep-alive PINGREQ. Which subscribe loses depends on broker latency, so the
 * same firmware failed at the third subscribe on one run and the second on the next.
 *
 * Shares a prefix with +MQTTSUBRX, so matching must require ':' or end-of-line after the name,
 * exactly as +MQTTCONN does against +MQTTCONNACK.
 */
#define RNWF_AEC_MQTT_SUBACK	"+MQTTSUB"

/* A granted subscription echoes the QoS it was granted; 128 is the documented failure. */
#define RNWF_MQTT_SUBACK_MAX_QOS	2


/*
 * "+MQTTCONN:<CONN_STATE>" — 0 not connected, 1 connected. Shares a prefix with +MQTTCONNACK, so
 * matching must require ':' or end-of-line after the name. This is the module's report that the
 * broker or network went away while the module itself stayed healthy — the one failure the
 * keepalive poll cannot see.
 */
#define RNWF_AEC_MQTT_CONN_STATE	"+MQTTCONN"
#define RNWF_MQTT_NOT_CONNECTED		0

/*
 * +MQTTCONNACK <CONN_REASON_CODE>: 0 = success; 128 unspecified, 129 malformed packet,
 * 130 protocol error, 131 implementation specific error (spec lists more).
 */
#define RNWF_MQTT_CONN_SUCCESS	0

/*
 * Buffer sizing comes from the spec's own maxima rather than from guesswork (Rule 5, ADR-0008).
 * The longest single command we emit is a broker password set:
 *   "AT+MQTTC=5,\"" + 256 + "\"" + CRLF = 273 bytes.
 * The longest we must *receive* is a +MQTTSUBRX carrying a wigwag/state payload, which is a few
 * dozen bytes (CONTEXT.md), so the RX line buffer is sized for headroom, not for the theoretical
 * maximum topic+payload the module could deliver. A line longer than the buffer is dropped with
 * an explicit overflow flag; it is never allowed to wrap and be parsed as two lines.
 */
#define RNWF_AT_MAX_SSID_LEN		32
#define RNWF_AT_MAX_CREDENTIALS_LEN	128
#define RNWF_AT_MAX_BROKER_ADDR_LEN	64
#define RNWF_AT_MAX_CLIENT_ID_LEN	48

#endif /* RNWF_AT_CMDS_H */
