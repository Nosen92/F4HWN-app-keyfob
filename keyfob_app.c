/* Copyright 2026 Your Name
 * https://github.com/your-handle
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

/*
 * Keyfob - transmits a fixed-frequency FSK keyfob code (Lock/Unlock/Trunk)
 * from a Quansheng UV-K1 running F4HWN v6.0.0, using the BK4819's FSK TX
 * engine. Overlay app for the uv-k1-k5v3-firmware-custom app system.
 *
 * The codes below are placeholders (0x0123456789ABCDE1/E2/E3 and their
 * bitwise complements) - replace LOCK_WORDS/UNLOCK_WORDS/TRUNK_WORDS with
 * your own keyfob's codes, captured and Manchester-encoded the same way,
 * before this will do anything useful against a real car.
 *
 * Signal structure, per repeat: 64-bit code + 64-bit bitwise complement,
 * Manchester-encoded (0->01, 1->10, MSB-first), packed into 16 words
 * (256 bits) MSB-first. Transmission holds the FSK TX FIFO continuously
 * full for the whole PTT hold - not a series of separate bursts - so the
 * chip only needs to emit its own preamble/sync once per hold rather than
 * once per repeat.
 *
 * Keys: LEFT/RIGHT selects Lock/Unlock/Trunk. Hold PTT to send. EXIT quits.
 */

#include <stdint.h>
#include <stdbool.h>
#include "../app_api.h"

static const app_api_t *A;

static char *putu(char *o, uint32_t v){ char t[10]; int8_t n=0; do{t[n++]=(char)('0'+v%10);v/=10;}while(v&&n<10); while(n--)*o++=t[n]; return o; }
static char *put(char *o, const char *s){ while(*s)*o++=*s++; return o; }

/* ---- RF parameters ---- */
#define TARGET_FREQ_X10HZ  43392088u   /* transmit frequency, in units of 10 Hz - set to your keyfob's frequency */
#define DEV_GAIN            0x00CCu    /* REG_70<6:0>: FSK tuning gain (0=min...0x7F=max), plus bit7 (Enable TONE2/FSK
                                         * gain path). Sets FSK deviation - tune to your keyfob's swing. */
#define PA_BIAS             0u         /* REG_36<15:8>: PA bias, 0=0V...0xFF=3.2V. Sets output power - tune to taste. */

/* ---- Commands: Manchester-encoded (0->01, 1->10), MSB-first, packed into
 * 16-bit words MSB-first. Each word table is your keyfob's 64-bit code
 * followed by its 64-bit bitwise complement, Manchester-doubled to 256
 * bits / 16 words. Replace with your own captured codes. ---- */
enum { CMD_LOCK=0, CMD_UNLOCK, CMD_TRUNK, CMD_COUNT };
static uint8_t selectedCmd = CMD_LOCK;

/* Placeholder code: 0x0123456789ABCDE1 */
static const uint16_t LOCK_WORDS[16] = {
    0x5556,0x595A,0x6566,0x696A,0x9596,0x999A,0xA5A6,0xA956,
    0xAAA9,0xA6A5,0x9A99,0x9695,0x6A69,0x6665,0x5A59,0x56A9
};
/* Placeholder code: 0x0123456789ABCDE2 */
static const uint16_t UNLOCK_WORDS[16] = {
    0x5556,0x595A,0x6566,0x696A,0x9596,0x999A,0xA5A6,0xA959,
    0xAAA9,0xA6A5,0x9A99,0x9695,0x6A69,0x6665,0x5A59,0x56A6
};
/* Placeholder code: 0x0123456789ABCDE3 */
static const uint16_t TRUNK_WORDS[16] = {
    0x5556,0x595A,0x6566,0x696A,0x9596,0x999A,0xA5A6,0xA95A,
    0xAAA9,0xA6A5,0x9A99,0x9695,0x6A69,0x6665,0x5A59,0x56A5
};

/* Safety cap on how long a single PTT hold will transmit for, per command -
 * a backstop against a stuck key, not a feature. Trunk commonly needs a
 * longer continuous hold than Lock/Unlock; adjust both to your keyfob's
 * own timing if needed. */
static uint32_t maxTxMsFor(uint8_t cmd){ return (cmd==CMD_TRUNK) ? 6000u : 3000u; }

static const uint16_t *wordsFor(uint8_t cmd)
{
    if(cmd==CMD_UNLOCK) return UNLOCK_WORDS;
    if(cmd==CMD_TRUNK)  return TRUNK_WORDS;
    return LOCK_WORDS;
}

/* One-time FSK TX configuration, called once at app start. */
static void fskTxSetup(void)
{
    A->bk_write(0x70, DEV_GAIN);
    A->bk_write(0x72, 0x3065);   /* 1200 baud */
    A->bk_write(0x58, 0x00C1);   /* FSK enable, 1.2K bandwidth */
    A->bk_write(0x5C, 0x5625);   /* CRC disabled (REG_5C<6>=0) */
    /* REG_5D: FSK data length, N+1 encoding split across <15:8> (low 8
     * bits) and <7:5> (high 3 bits). Set to the field's maximum (2048
     * bytes) so the chip's own configured burst length is never what
     * ends transmission - the app's own safety-cap timing (see
     * maxTxMsFor()) decides when to stop instead. */
    A->bk_write(0x5D, 0xFFE0);
}

static void applyPaBias(void)
{
    const uint8_t gain = (uint8_t)((4u<<3)|(2u<<0));
    A->bk_write(0x36, (uint16_t)((PA_BIAS<<8)|(1u<<7)|gain));
}

/* Keys up once per PTT press: carrier+PA, frequency/deviation/power,
 * sub-tone and scramble explicitly disabled, PLL relock. */
static void keyUp(void)
{
    A->tx_set_params();
    A->bk_write(0x51, 0x0000);   /* force TX CTCSS/CDCSS off, regardless of the VFO's own config */
    A->bk_write(0x38, (uint16_t)(TARGET_FREQ_X10HZ & 0xFFFFu));
    A->bk_write(0x39, (uint16_t)((TARGET_FREQ_X10HZ >> 16) & 0xFFFFu));
    A->bk_write(0x70, DEV_GAIN);
    applyPaBias();
    {
        uint16_t r31 = A->bk_read(0x31);
        A->bk_write(0x31, (uint16_t)(r31 & ~(uint16_t)(1u<<1)));   /* force scramble off */
    }
    A->bk_write(0x30, 0x0000);
    A->bk_write(0x30, 0xC1FE);
    A->delay_ms(25);
}

/* Running index into the selected command's word table, persisting across
 * the whole hold so the 16-word cycle repeats seamlessly. Reset on each
 * fresh PTT press. */
static uint32_t payloadCursor = 0;

static uint16_t rawWordAt(uint32_t idx)
{
    return wordsFor(selectedCmd)[idx % 16u];
}

/* The BK4819's FSK TX FIFO (REG_5F) latches its high byte one write-cycle
 * late: what's read back on the wire is (previous write's high byte |
 * current write's low byte). Countered by writing each word pre-shifted -
 * pairing the CURRENT word's low byte with the NEXT word's high byte - so
 * the chip's own one-write lag cancels out and the correct word appears
 * on air. */
static uint16_t payloadWord(void)
{
    uint32_t idx = payloadCursor;
    payloadCursor++;
    uint16_t cur = rawWordAt(idx);
    uint16_t next = rawWordAt(idx+1u);
    return (uint16_t)((next & 0xFF00u) | (cur & 0x00FFu));
}

/* Keeps the FSK TX FIFO continuously fed for the duration of a PTT hold,
 * rather than sending discrete bursts that each need their own restart
 * (and therefore their own preamble/sync). The FIFO holds up to 128 words;
 * REG_02<14> ("FSK FIFO Almost Empty") signals when it's time to top up,
 * with REG_5E<9:3> as the configurable threshold (default: 64 of 128
 * words). Refilling before the FIFO empties means the chip's FSK-TX-enable
 * bit (REG_59<11>) never needs to toggle again after the initial key-up,
 * so only one preamble/sync is emitted for the whole hold.
 *
 * Polls the raw Almost-Empty level and refills a fixed batch each time it
 * reads as set, rather than depending on edge/read-clear semantics that
 * aren't documented for this bit - self-correcting regardless: if the
 * flag is still set next poll, there's still room (writes stay well
 * inside the space available when the threshold first trips); once
 * occupancy is back above the threshold, the flag reads clear on its own.
 *
 * If TX_FINISHED (REG_02<15>) is ever observed instead, something ended
 * the burst despite the refill - re-key on the next call rather than
 * leaving the radio silently off the air for the rest of the hold. */
static bool streamStep(bool needsSetup, uint32_t *msOut)
{
    uint32_t ms=0;
    if(needsSetup){
        A->delay_ms(20); ms+=20;
        A->bk_write(0x3F, 0xC000);   /* enable TX_FINISHED + FIFO Almost Empty interrupts */
        A->bk_write(0x59, 0x8000);   /* clear TX FIFO */
        A->bk_write(0x59, 0x0000);
        /* Fill the FIFO completely (128 words = 8 repeats of the 16-word
         * payload) so Almost Empty's default 64-word threshold doesn't
         * trip immediately, before transmission even starts. */
        for(uint8_t i=0;i<128u;i++) A->bk_write(0x5F, payloadWord());
        A->delay_ms(20); ms+=20;
        A->bk_write(0x59, 0x0800);   /* key FSK TX - starts the stream */
        *msOut = ms;
        return false;
    }

    uint16_t status = A->bk_read(0x02);
    if(status & (1u<<15)){   /* TX_FINISHED - stream stopped unexpectedly */
        A->bk_write(0x02, 0);
        *msOut = ms;
        return true;   /* signal: needs a fresh setup next call */
    }
    if(status & (1u<<14)){   /* Almost Empty - top up before it runs dry */
        for(uint8_t i=0;i<32u;i++) A->bk_write(0x5F, payloadWord());
    }
    A->delay_ms(1); ms+=1;
    *msOut = ms;
    return false;
}

/* Ends TX cleanly and returns to RX. */
static void keyDown(void)
{
    A->delay_ms(20);
    A->bk_write(0x3F, 0x0000);
    A->bk_write(0x59, 0x0068);
    A->delay_ms(30);
    A->bk_write(0x30, 0x0000);
    A->tx_end();
}

static void draw(bool txActive, uint32_t txMs, bool timedOut)
{
    A->display_clear();
    A->status_clear();
    A->draw_battery();
    A->print_bold("KEYFOB",1,0,0);
    /* Command row: LOCK / UNLOCK / TRUNK, bold marking the current
     * selection. Positions are pre-computed to match print_normal's own
     * internal text-centering formula (equal ~42px-wide columns across a
     * 128px screen), rather than relying on its [start,end] auto-centering
     * directly - keeps LOCK/UNLOCK/TRUNK visually aligned in a way that
     * would otherwise need per-string-length adjustment. */
    if(selectedCmd==CMD_LOCK)   A->print_bold(  "LOCK",  8,0,2);
    else                        A->print_normal("LOCK",  8,0,2);
    if(selectedCmd==CMD_UNLOCK) A->print_bold(  "UNLOCK",43,0,2);
    else                        A->print_normal("UNLOCK",43,0,2);
    if(selectedCmd==CMD_TRUNK)  A->print_bold(  "TRUNK", 89,0,2);
    else                        A->print_normal("TRUNK", 89,0,2);
    if(txActive){
        char r[24]; char *o=put(r,"Sending... "); o=putu(o,txMs); o=put(o,"ms"); *o='\0';
        A->print_normal(r,1,0,3);
    } else if(timedOut){
        A->print_normal("Stopped (safety cap)",1,0,3);
    } else {
        A->print_normal("Hold PTT to send",1,0,3);
    }
    if(selectedCmd==CMD_TRUNK){
        A->print_normal("(hold for 5+ seconds)",1,0,4);
    }
    A->print_normal("LEFT/RIGHT: select cmd",1,0,6);
    A->blit_status();
    A->blit_full();
}

__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api)
{
    A = api;
    A->backlight_on();

    fskTxSetup();

    bool running=true;
    bool txActive=false;
    bool timedOut=false;
    uint32_t txMs=0;
    uint32_t lastDrawMs=0;
    bool needsSetup=true;
    uint8_t prevKey=APP_KEY_INVALID;

    draw(txActive, txMs, timedOut);

    while(running){
        uint8_t key=A->get_key();
        if(key==APP_KEY_SAVER){
            if(txActive){ keyDown(); txActive=false; timedOut=false; draw(txActive, txMs, timedOut); }
            prevKey=APP_KEY_INVALID; A->delay_ms(10); A->backlight_update(); continue;
        }
        if(key==APP_KEY_WAKE) key=APP_KEY_INVALID;

        if(key==APP_KEY_PTT){
            if(!txActive){
                keyUp();
                txActive=true;
                txMs=0;
                lastDrawMs=0;
                payloadCursor=0;
                needsSetup=true;
                timedOut=false;
                draw(txActive, txMs, timedOut);
            }
            if(txMs < maxTxMsFor(selectedCmd)){
                uint32_t stepMs=0;
                needsSetup = streamStep(needsSetup, &stepMs);
                txMs += stepMs;
                /* Redraw at most every ~250ms during an active hold, not
                 * after every poll - a full display_clear + blit is real,
                 * non-trivial time, and the "Sending...Nms" counter
                 * doesn't need millisecond-precision refreshes. */
                if(txMs-lastDrawMs >= 250u){
                    draw(txActive, txMs, timedOut);
                    lastDrawMs=txMs;
                }
            } else {
                keyDown();
                txActive=false;
                timedOut=true;
                draw(txActive, txMs, timedOut);
            }
        } else {
            if(txActive){
                keyDown();
                txActive=false;
                timedOut=false;
                draw(txActive, txMs, timedOut);
            }
            if(key!=prevKey && key!=APP_KEY_INVALID){
                A->backlight_on();
                if(key==APP_KEY_EXIT){
                    running=false;
                } else if(key==APP_KEY_DOWN){   /* physical RIGHT on the UV-K1 */
                    selectedCmd=(uint8_t)((selectedCmd+1u)%CMD_COUNT);
                    draw(txActive, txMs, timedOut);
                } else if(key==APP_KEY_UP){   /* physical LEFT on the UV-K1 */
                    selectedCmd=(uint8_t)((selectedCmd+CMD_COUNT-1u)%CMD_COUNT);
                    draw(txActive, txMs, timedOut);
                }
            }
        }
        prevKey=key;

        A->backlight_update();
        if(!txActive) A->delay_ms(100);
    }

    if(txActive) keyDown();
}
