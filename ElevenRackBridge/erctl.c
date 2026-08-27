/**
 * @file erctl.c
 * @brief Probe: read/write the Eleven Rack's Rig Input via CoreMIDI SysEx.
 *
 * The goal is to prove we can SET the Rig Input source (in particular "Re-Amp")
 * over the Eleven Rack's USB editor/control interface, which the built-in
 * class-compliant USB-MIDI driver exposes as CoreMIDI ports named
 * "Eleven Rack ...". This is the one input option MIDI CC can't reach.
 *
 * Protocol (reverse-engineered; see bender615 EditorProtocolNotes.md and
 * schmidg/elevenhack SysEx.java):
 *
 *   Frame:      F0 13 0B 0F <function> <object> [payload...] F7
 *   Functions:  01 = request/read, 12 = response, 00 = set
 *   Objects:    07 = edit-buffer rig volume, 3D = rig input, 01 = edit buffer
 *
 * Rig Input is object 3D. VERIFIED on connected hardware 2026-08-14: it reads
 * and writes as a SINGLE raw byte (not packed), and the hardware honours writes.
 * The value is a compact index (its own table, NOT the elevenhack "WorB" bulk
 * enum). Values 0..8 are valid; 9 or higher crashes the unit:
 *
 *   0 Guitar   1 Re-Amp   2 Mic   3 Line L   4 Line R
 *   5 Line L+R   6 Digital L   7 Digital R   8 Digital L+R
 *
 * So setting Rig Input to Re-Amp is:  F0 13 0B 0F 00 3D 01 F7
 *
 * Usage:
 *   ./erctl                 identity + read rig volume (07, sanity) + read input (3D)
 *   ./erctl read            read rig input (3D) only
 *   ./erctl set <0-8>       write input index (clamped to 0..8), re-read to confirm
 *   ./erctl reamp           shortcut for Re-Amp (1)
 *   ./erctl guitar          shortcut for Guitar (0)
 *   ./erctl sweep [max]     step 0..max (default 8) for mapping, then restore
 *   ./erctl raw "F0 13 .."  send arbitrary SysEx hex, dump any reply
 *
 * Build:
 * @code
 *   clang -o erctl erctl.c -framework CoreFoundation -framework CoreMIDI \
 *       -Wno-deprecated-declarations
 * @endcode
 *
 * Safe to run while the audio engine streams: the control SysEx rides the MIDI
 * endpoint (USB-MIDI cable 0, interface 2), separate from the audio interfaces.
 * A write only changes a hardware setting; worst case it reverts on rig change.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

/* ---- Eleven Rack SysEx constants ---- */
#define ER_START    0xF0
#define ER_VENDOR   0x13
#define ER_DEVICE   0x0B
#define ER_MODEL    0x0F
#define ER_FN_SET   0x00   /* SNDSET  */
#define ER_FN_REQ   0x01   /* REQU    */
#define ER_FN_RESP  0x12   /* RESPOND */
#define ER_END      0xF7

#define ER_OBJ_VOLUME 0x07
#define ER_OBJ_INPUT  0x3D

#define ER_NAME "Eleven Rack"

/* ---- Rig-input value enumeration for object 3D ----
 * Verified on connected hardware 2026-08-14 by write-sweep. This is the compact
 * index used by object 3D on the wire; it is NOT the elevenhack "WorB" bulk enum
 * (60=Guitar, 18=Re-Amp, ...). Values 0..8 are valid; 9+ crashes the unit. */
#define IN_GUITAR    0
#define IN_REAMP     1
#define IN_MIC       2
#define IN_LINE_L    3
#define IN_LINE_R    4
#define IN_LINE_LR   5
#define IN_DIG_L     6
#define IN_DIG_R     7
#define IN_DIG_LR    8
#define IN_MAX       8   /* highest safe value; do not exceed */

/* ---- Incoming SysEx accumulator (written on the MIDI thread) ---- */
static uint8_t  g_rx[1024];
static volatile size_t g_rxLen = 0;      /* bytes of the last complete message */
static volatile int    g_rxReady = 0;    /* set when a F7-terminated msg arrives */
static uint8_t  g_acc[4096];
static size_t   g_accLen = 0;
static int      g_inSysex = 0;

static const char *inputName(int v) {
    switch (v) {
        case IN_GUITAR:  return "Guitar";
        case IN_MIC:     return "Mic";
        case IN_LINE_L:  return "Line L";
        case IN_LINE_R:  return "Line R";
        case IN_LINE_LR: return "Line L+R";
        case IN_DIG_L:   return "Digital L";
        case IN_DIG_R:   return "Digital R";
        case IN_DIG_LR:  return "Digital L+R";
        case IN_REAMP:   return "Re-Amp";
        default:         return "?";
    }
}

/** Decode the 7/7/7/7/4-packed scalar used by rig volume (object 07). */
static uint32_t unpack7774(const uint8_t *p) {
    return ((uint32_t)(p[0] & 0x7F) << 25) |
           ((uint32_t)(p[1] & 0x7F) << 18) |
           ((uint32_t)(p[2] & 0x7F) << 11) |
           ((uint32_t)(p[3] & 0x7F) <<  4) |
           ((uint32_t)(p[4] & 0x0F));
}

static void hexdump(const char *label, const uint8_t *b, size_t n) {
    printf("%s (%zu bytes):", label, n);
    for (size_t i = 0; i < n; i++) printf(" %02X", b[i]);
    printf("\n");
    printf("        ascii:");
    for (size_t i = 0; i < n; i++)
        printf(" %2c", (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.');
    printf("\n");
}

/** CoreMIDI read callback (runs on MIDI's own thread). Reassemble SysEx. */
static void readProc(const MIDIPacketList *pktlist, void *refCon, void *srcConn) {
    (void)refCon; (void)srcConn;
    const MIDIPacket *p = &pktlist->packet[0];
    for (unsigned i = 0; i < pktlist->numPackets; i++) {
        for (unsigned j = 0; j < p->length; j++) {
            uint8_t b = p->data[j];
            if (b == ER_START) { g_inSysex = 1; g_accLen = 0; }
            if (g_inSysex && g_accLen < sizeof(g_acc)) g_acc[g_accLen++] = b;
            if (b == ER_END && g_inSysex) {
                g_inSysex = 0;
                size_t n = g_accLen < sizeof(g_rx) ? g_accLen : sizeof(g_rx);
                memcpy(g_rx, g_acc, n);
                g_rxLen = n;
                g_rxReady = 1;
            }
        }
        p = MIDIPacketNext(p);
    }
}

/** Find every source/destination whose display name contains ER_NAME. */
static int endpointNameHas(MIDIEndpointRef ep, const char *needle) {
    CFStringRef cf = NULL;
    if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &cf) != noErr || !cf)
        return 0;
    char buf[256] = {0};
    CFStringGetCString(cf, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(cf);
    return strstr(buf, needle) != NULL;
}

static void endpointName(MIDIEndpointRef ep, char *out, size_t cap) {
    CFStringRef cf = NULL;
    out[0] = 0;
    if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &cf) == noErr && cf) {
        CFStringGetCString(cf, out, (CFIndex)cap, kCFStringEncodingUTF8);
        CFRelease(cf);
    }
}

/** Send a SysEx buffer and wait up to timeout_ms for a F7-terminated reply. */
static int sendAndWait(MIDIPortRef out, MIDIEndpointRef dst,
                       const uint8_t *msg, size_t len, int timeout_ms) {
    hexdump("  -> sent", msg, len);
    g_rxReady = 0;

    Byte packetBuf[512];
    MIDIPacketList *pl = (MIDIPacketList *)packetBuf;
    MIDIPacket *pkt = MIDIPacketListInit(pl);
    pkt = MIDIPacketListAdd(pl, sizeof(packetBuf), pkt, 0, len, msg);
    if (!pkt) { printf("  !! packet too large\n"); return -1; }
    OSStatus s = MIDISend(out, dst, pl);
    if (s != noErr) { printf("  !! MIDISend failed: %d\n", (int)s); return -1; }

    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (g_rxReady) {
            hexdump("  <- reply", g_rx, g_rxLen);
            return (int)g_rxLen;
        }
        usleep(10 * 1000);
    }
    printf("  <- (no reply within %d ms)\n", timeout_ms);
    return 0;
}

/** Interpret a reply to a read of object 3D and print a best-effort value. */
static void interpretInput(const uint8_t *r, size_t n) {
    /* Expected: F0 13 0B 0F 12 3D <payload...> F7 */
    if (n < 8 || r[0] != ER_START || r[4] != ER_FN_RESP || r[5] != ER_OBJ_INPUT) {
        printf("  (unrecognised rig-input reply shape)\n");
        return;
    }
    size_t payload = n - 7; /* minus F0 13 0B 0F 12 3D ... F7 */
    if (payload == 1) {
        int v = r[6];
        printf("  Rig Input (1-byte)  = %d (%s)\n", v, inputName(v));
    } else if (payload == 5) {
        uint32_t v = unpack7774(&r[6]);
        printf("  Rig Input (packed)  = %u (%s)\n", v, inputName((int)v));
    } else {
        printf("  Rig Input payload is %zu bytes — inspect hex above.\n", payload);
    }
}

int main(int argc, char **argv) {
    printf("erctl — Eleven Rack rig-input probe (CoreMIDI SysEx)\n\n");

    MIDIClientRef client = 0;
    if (MIDIClientCreate(CFSTR("erctl"), NULL, NULL, &client) != noErr) {
        fprintf(stderr, "MIDIClientCreate failed\n"); return 1;
    }
    MIDIPortRef inPort = 0, outPort = 0;
    MIDIInputPortCreate(client, CFSTR("erctl in"), readProc, NULL, &inPort);
    MIDIOutputPortCreate(client, CFSTR("erctl out"), &outPort);

    /* Connect input to every matching source; pick a destination. */
    printf("── MIDI endpoints ──\n");
    ItemCount nsrc = MIDIGetNumberOfSources();
    int connected = 0;
    for (ItemCount i = 0; i < nsrc; i++) {
        MIDIEndpointRef src = MIDIGetSource(i);
        char nm[256]; endpointName(src, nm, sizeof(nm));
        int match = endpointNameHas(src, ER_NAME);
        printf("  source %lu: %-30s %s\n", (unsigned long)i, nm, match ? "[listen]" : "");
        if (match) { MIDIPortConnectSource(inPort, src, NULL); connected++; }
    }

    ItemCount ndst = MIDIGetNumberOfDestinations();
    MIDIEndpointRef dst = 0; char dstName[256] = {0};
    for (ItemCount i = 0; i < ndst; i++) {
        MIDIEndpointRef d = MIDIGetDestination(i);
        char nm[256]; endpointName(d, nm, sizeof(nm));
        int match = endpointNameHas(d, ER_NAME);
        printf("  dest   %lu: %-30s %s\n", (unsigned long)i, nm, match ? "[candidate]" : "");
        if (match) {
            /* Prefer a port whose name mentions "Rig"; else take the first. */
            if (dst == 0 || strstr(nm, "Rig")) { dst = d; strncpy(dstName, nm, sizeof(dstName) - 1); }
        }
    }
    printf("\n");

    if (!connected) printf("!! No 'Eleven Rack' MIDI source found — is it plugged in?\n");
    if (!dst)       { printf("!! No 'Eleven Rack' MIDI destination — cannot send.\n"); return 2; }
    printf("Sending to: %s\n\n", dstName);

    /* Give the freshly-connected source a moment to settle. */
    usleep(150 * 1000);

    const char *cmd = argc > 1 ? argv[1] : "";

    if (strcmp(cmd, "sweep") == 0) {
        int maxv = argc > 2 ? (int)strtol(argv[2], NULL, 0) : 8;
        int holdms = 5000;
        uint8_t rd[] = { ER_START, ER_VENDOR, ER_DEVICE, ER_MODEL, ER_FN_REQ, ER_OBJ_INPUT, ER_END };

        printf("── read ORIGINAL value (will restore at end) ──\n");
        int orig = -1;
        int n = sendAndWait(outPort, dst, rd, sizeof(rd), 1000);
        if (n >= 8 && g_rx[4] == ER_FN_RESP && g_rx[5] == ER_OBJ_INPUT && (n - 7) == 1) orig = g_rx[6];
        printf("  original 3D value = %d\n\n", orig);

        printf("=============================================================\n");
        printf(" WATCH THE ELEVEN RACK's RIG INPUT DISPLAY.\n");
        printf(" For each number below, note which input name the unit shows.\n");
        printf(" (read-back != written value  => hardware ignored that write)\n");
        printf("=============================================================\n\n");

        for (int v = 0; v <= maxv; v++) {
            uint8_t wr[] = { ER_START, ER_VENDOR, ER_DEVICE, ER_MODEL, ER_FN_SET, ER_OBJ_INPUT,
                             (uint8_t)(v & 0xFF), ER_END };
            printf(">>>>>>>>>>>>>>>>  NOW WRITING 3D = %d  <<<<<<<<<<<<<<<<\n", v);
            g_rxReady = 0;
            Byte pb[64]; MIDIPacketList *pl = (MIDIPacketList *)pb;
            MIDIPacket *pk = MIDIPacketListInit(pl);
            pk = MIDIPacketListAdd(pl, sizeof(pb), pk, 0, sizeof(wr), wr);
            MIDISend(outPort, dst, pl);
            usleep(250 * 1000);
            int rn = sendAndWait(outPort, dst, rd, sizeof(rd), 800);
            if (rn >= 8 && (rn - 7) == 1)
                printf("    read-back = %d  %s\n", g_rx[6],
                       g_rx[6] == v ? "(write took)" : "(** differs — ignored/clamped **)");
            printf("    ^ look at the unit now. Holding %d for %d ms...\n\n", v, holdms);
            usleep(holdms * 1000);
        }

        if (orig >= 0) {
            printf("── restoring original value %d ──\n", orig);
            uint8_t wr[] = { ER_START, ER_VENDOR, ER_DEVICE, ER_MODEL, ER_FN_SET, ER_OBJ_INPUT,
                             (uint8_t)(orig & 0xFF), ER_END };
            sendAndWait(outPort, dst, wr, sizeof(wr), 800);
        }
        printf("\nSweep done. Tell me the input name shown for each number 0..%d.\n", maxv);
        return 0;
    }

    if (strcmp(cmd, "listen") == 0) {
        int secs = argc > 2 ? atoi(argv[2]) : 8;
        printf("── listening %d s — change the control on the hardware now ──\n", secs);
        g_rxReady = 0;
        for (int i = 0; i < secs * 100; i++) {
            if (g_rxReady) { hexdump("  <- rx", g_rx, g_rxLen); g_rxReady = 0; }
            usleep(10 * 1000);
        }
        printf("── done listening ──\n");
        return 0;
    }

    if (strcmp(cmd, "raw") == 0 && argc > 2) {
        uint8_t buf[512]; size_t n = 0;
        char *tok = strtok(argv[2], " ");
        while (tok && n < sizeof(buf)) { buf[n++] = (uint8_t)strtol(tok, NULL, 16); tok = strtok(NULL, " "); }
        printf("── raw send ──\n");
        sendAndWait(outPort, dst, buf, n, 1000);
        return 0;
    }

    if (strcmp(cmd, "set") == 0 || strcmp(cmd, "reamp") == 0 || strcmp(cmd, "guitar") == 0) {
        int val;
        if (strcmp(cmd, "reamp") == 0)       val = IN_REAMP;
        else if (strcmp(cmd, "guitar") == 0) val = IN_GUITAR;
        else if (argc > 2)                   val = (int)strtol(argv[2], NULL, 0);
        else { fprintf(stderr, "usage: %s set <0-%d>\n", argv[0], IN_MAX); return 1; }

        if (val < 0 || val > IN_MAX) {
            fprintf(stderr, "refusing value %d: rig input must be 0..%d (9+ crashes the unit)\n",
                    val, IN_MAX);
            return 1;
        }

        printf("── read BEFORE ──\n");
        uint8_t rd[] = { ER_START, ER_VENDOR, ER_DEVICE, ER_MODEL, ER_FN_REQ, ER_OBJ_INPUT, ER_END };
        int n = sendAndWait(outPort, dst, rd, sizeof(rd), 1000);
        if (n > 0) interpretInput(g_rx, g_rxLen);

        printf("\n── write input = %d (%s), single byte ──\n", val, inputName(val));
        uint8_t wr[] = { ER_START, ER_VENDOR, ER_DEVICE, ER_MODEL, ER_FN_SET, ER_OBJ_INPUT,
                         (uint8_t)(val & 0xFF), ER_END };
        sendAndWait(outPort, dst, wr, sizeof(wr), 1000);

        usleep(200 * 1000);
        printf("\n── read AFTER ──\n");
        n = sendAndWait(outPort, dst, rd, sizeof(rd), 1000);
        if (n > 0) interpretInput(g_rx, g_rxLen);
        return 0;
    }

    /* Default / "read": identity, rig volume (sanity), rig input. */
    printf("── identity request (F0 7E 7F 06 01 F7) ──\n");
    uint8_t idreq[] = { 0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7 };
    sendAndWait(outPort, dst, idreq, sizeof(idreq), 1000);

    if (strcmp(cmd, "read") != 0) {
        printf("\n── read rig volume (object 07, known-good sanity) ──\n");
        uint8_t volreq[] = { ER_START, ER_VENDOR, ER_DEVICE, ER_MODEL, ER_FN_REQ, ER_OBJ_VOLUME, ER_END };
        int n = sendAndWait(outPort, dst, volreq, sizeof(volreq), 1000);
        if (n >= 12 && g_rx[4] == ER_FN_RESP && g_rx[5] == ER_OBJ_VOLUME) {
            uint32_t raw = unpack7774(&g_rx[6]);
            double dB = -24.0 + 24.0 * ((double)raw) / 4294967295.0;
            printf("  Rig Volume raw=0x%08X  ~ %.1f dB\n", raw, dB);
        }
    }

    printf("\n── read rig input (object 3D) ──\n");
    uint8_t inreq[] = { ER_START, ER_VENDOR, ER_DEVICE, ER_MODEL, ER_FN_REQ, ER_OBJ_INPUT, ER_END };
    int n = sendAndWait(outPort, dst, inreq, sizeof(inreq), 1000);
    if (n > 0) interpretInput(g_rx, g_rxLen);

    printf("\nDone. If the rig-input reply shape is clear above, we know the encoding\n"
           "to use for the write. Try:  ./erctl reamp   then check the hardware.\n");
    return 0;
}
