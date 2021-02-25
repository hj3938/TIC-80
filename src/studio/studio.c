// MIT License

// Copyright (c) 2017 Vadim Grigoruk @nesbox // grigoruk@gmail.com

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "studio.h"

#include "editors/code.h"
#include "editors/sprite.h"
#include "editors/map.h"
#include "editors/world.h"
#include "editors/sfx.h"
#include "editors/music.h"
#include "screens/start.h"
#include "screens/console.h"
#include "screens/run.h"
#include "screens/menu.h"
#include "screens/surf.h"
#include "screens/dialog.h"
#include "ext/history.h"
#include "config.h"
#include "project.h"

#include "fs.h"
#include "net.h"

#include "ext/gif.h"
#include "ext/md5.h"
#include "wave_writer.h"
#include "argparse.h"

#include <ctype.h>
#include <math.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define FRAME_SIZE (TIC80_FULLWIDTH * TIC80_FULLHEIGHT * sizeof(u32))
#define POPUP_DUR (TIC80_FRAMERATE*2)

#if defined(TIC80_PRO)
#define TIC_EDITOR_BANKS (TIC_BANKS)
#else
#define TIC_EDITOR_BANKS 1
#endif

#define MD5_HASHSIZE 16
#define BG_ANIMATION_COLOR tic_color_dark_grey

static const char VideoGif[] = "video%i.gif";
static const char ScreenGif[] = "screen%i.gif";

typedef struct
{
    u8 data[MD5_HASHSIZE];
} CartHash;

typedef struct
{
    bool down;
    bool click;

    tic_point start;
    tic_point end;

} MouseState;

static const EditorMode Modes[] =
{
    TIC_CODE_MODE,
    TIC_SPRITE_MODE,
    TIC_MAP_MODE,
    TIC_SFX_MODE,
    TIC_MUSIC_MODE,
};

static const EditorMode BankModes[] =
{
    TIC_SPRITE_MODE,
    TIC_MAP_MODE,
    TIC_SFX_MODE,
    TIC_MUSIC_MODE,
};

static struct
{
    Studio studio;

    tic80_local* tic80local;

    struct
    {
        CartHash hash;
        u64 mdate;
    }cart;

    EditorMode mode;
    EditorMode prevMode;
    EditorMode dialogMode;

    struct
    {
        MouseState state[3];
    } mouse;

    tic_key keycodes[KEYMAP_COUNT];

    struct
    {
        bool show;
        bool chained;

        union
        {
            struct
            {
                s8 sprites;
                s8 map;
                s8 sfx;
                s8 music;
            } index;

            s8 indexes[COUNT_OF(BankModes)];
        };

    } bank;

    struct
    {
        s32 counter;
        char message[STUDIO_TEXT_BUFFER_WIDTH];
    } popup;

    struct
    {
        char text[STUDIO_TEXT_BUFFER_WIDTH];
    } tooltip;

    struct
    {
        bool record;

        u32* buffer;
        s32 frames;
        s32 frame;

    } video;

    struct
    {
        Code*       code;

        struct
        {
            Sprite* sprite[TIC_EDITOR_BANKS];
            Map*    map[TIC_EDITOR_BANKS];
            Sfx*    sfx[TIC_EDITOR_BANKS];
            Music*  music[TIC_EDITOR_BANKS];
        } banks;

        Start*      start;
        Console*    console;
        Run*        run;
        World*      world;
        Config*     config;
        Dialog*     dialog;
        Menu*       menu;
        Surf*       surf;
    };

    tic_fs* fs;
    tic_net* net;

    s32 samplerate;
    tic_font systemFont;

} impl =
{
    .tic80local = NULL,
    .cart = 
    {
        .mdate = 0,
    },

    .mode = TIC_START_MODE,
    .prevMode = TIC_CODE_MODE,
    .dialogMode = TIC_CONSOLE_MODE,

    .keycodes =
    {
        tic_key_up,
        tic_key_down,
        tic_key_left,
        tic_key_right,

        tic_key_z, // a
        tic_key_x, // b
        tic_key_a, // x
        tic_key_s, // y
    },

    .bank = 
    {
        .show = false,
        .chained = true,
    },

    .popup =
    {
        .counter = 0,
        .message = "\0",
    },

    .tooltip =
    {
        .text = "\0",
    },

    .video =
    {
        .record = false,
        .buffer = NULL,
        .frames = 0,
    },
};

void map2ram(tic_ram* ram, const tic_map* src)
{
    memcpy(ram->map.data, src, sizeof ram->map);
}

void tiles2ram(tic_ram* ram, const tic_tiles* src)
{
    memcpy(ram->tiles.data, src, sizeof ram->tiles * TIC_SPRITE_BANKS);
}

static inline void sfx2ram(tic_ram* ram, const tic_sfx* src)
{
    memcpy(&ram->sfx, src, sizeof ram->sfx);
}

static inline void music2ram(tic_ram* ram, const tic_music* src)
{
    memcpy(&ram->music, src, sizeof ram->music);
}

s32 calcWaveAnimation(tic_mem* tic, u32 offset, s32 channel)
{
    const tic_sound_register* reg = &tic->ram.registers[channel];

    s32 val = tic_tool_is_noise(&reg->waveform)
        ? (rand() & 1) * MAX_VOLUME
        : tic_tool_peek4(reg->waveform.data, ((offset * reg->freq) >> 7) % WAVE_VALUES);

    return val * reg->volume;
}

static const tic_sfx* getSfxSrc()
{
    tic_mem* tic = impl.studio.tic;
    return &tic->cart.banks[impl.bank.index.sfx].sfx;
}

static const tic_music* getMusicSrc()
{
    tic_mem* tic = impl.studio.tic;
    return &tic->cart.banks[impl.bank.index.music].music;
}

const char* studioExportSfx(s32 index, const char* filename)
{
    tic_mem* tic = impl.studio.tic;

    const char* path = tic_fs_path(impl.fs, filename);

    if(wave_open( impl.samplerate, path ))
    {

#if TIC_STEREO_CHANNELS == 2
        wave_enable_stereo();
#endif

        const tic_sfx* sfx = getSfxSrc();

        sfx2ram(&tic->ram, sfx);
        music2ram(&tic->ram, getMusicSrc());

        {
            const tic_sample* effect = &sfx->samples.data[index];

            enum{Channel = 0};
            sfx_stop(tic, Channel);
            tic_api_sfx(tic, index, effect->note, effect->octave, -1, Channel, MAX_VOLUME, MAX_VOLUME, SFX_DEF_SPEED);

            for(s32 ticks = 0, pos = 0; pos < SFX_TICKS; pos = tic_tool_sfx_pos(effect->speed, ++ticks))
            {
                tic_core_tick_start(tic);
                tic_core_tick_end(tic);

                wave_write(tic->samples.buffer, tic->samples.size / sizeof(s16));
            }

            sfx_stop(tic, Channel);
            memset(tic->ram.registers, 0, sizeof(tic_sound_register));
        }

        wave_close();

        return path;
    }

    return NULL;
}

const char* studioExportMusic(s32 track, const char* filename)
{
    tic_mem* tic = impl.studio.tic;

    const char* path = tic_fs_path(impl.fs, filename);

    if(wave_open( impl.samplerate, path ))
    {
#if TIC_STEREO_CHANNELS == 2
        wave_enable_stereo();
#endif

        const tic_sfx* sfx = getSfxSrc();
        const tic_music* music = getMusicSrc();

        sfx2ram(&tic->ram, sfx);
        music2ram(&tic->ram, music);

        const tic_sound_state* state = &tic->ram.sound_state;
        const Music* editor = impl.banks.music[impl.bank.index.music];

        tic_api_music(tic, track, -1, -1, false, editor->sustain);

        while(state->flag.music_state == tic_music_play)
        {
            tic_core_tick_start(tic);

            for (s32 i = 0; i < TIC_SOUND_CHANNELS; i++)
                if(!editor->on[i])
                    tic->ram.registers[i].volume = 0;

            tic_core_tick_end(tic);

            wave_write(tic->samples.buffer, tic->samples.size / sizeof(s16));
        }

        wave_close();        
        return path;
    }

    return NULL;
}

void sfx_stop(tic_mem* tic, s32 channel)
{
    tic_api_sfx(tic, -1, 0, 0, -1, channel, MAX_VOLUME, MAX_VOLUME, SFX_DEF_SPEED);
}

// BG animation based on DevEd code
void drawBGAnimation(tic_mem* tic, s32 ticks)
{
    tic_api_cls(tic, TIC_COLOR_BG);

    double rx = sin(ticks / 64.0) * 4.5;
    double tmp;
    double mod = modf(ticks / 16.0, &tmp);

    enum{Gap = 72};

    for(s32 x = 0; x <= 16; x++)
    {
        s32 ly = (s32)(Gap - (8 / (x - mod)) * 32);

        tic_api_line(tic, 0, (s32)(ly + rx), TIC80_WIDTH, (s32)(ly - rx), BG_ANIMATION_COLOR);
        tic_api_line(tic, 0, (s32)((TIC80_HEIGHT - ly) - rx), TIC80_WIDTH, 
            (s32)((TIC80_HEIGHT - ly) + rx), BG_ANIMATION_COLOR);
    }

    double yp = (Gap - (8 / (16 - mod)) * 32) - rx;

    for(s32 x = -32; x <= 32; x++)
    {
        s32 yf = (s32)(yp + rx * x / 32 + rx);

        tic_api_line(tic, (s32)((TIC80_WIDTH / 2) - ((x - (rx / 8)) * 4)), yf,
            (s32)((TIC80_WIDTH / 2) - ((x + (rx / 16)) * 24)), -16, BG_ANIMATION_COLOR);

        tic_api_line(tic, (s32)((TIC80_WIDTH / 2) - ((x - (rx / 8)) * 4)), TIC80_HEIGHT - yf,
            (s32)((TIC80_WIDTH / 2) - ((x + (rx / 16)) * 24)), TIC80_HEIGHT + 16, BG_ANIMATION_COLOR);
    }
}

static void modifyColor(tic_mem* tic, s32 x, u8 r, u8 g, u8 b)
{
    s32 addr = offsetof(tic_ram, vram.palette) + ((x % 16) * 3);
    tic_api_poke(tic, addr, r);
    tic_api_poke(tic, addr + 1, g);
    tic_api_poke(tic, addr + 2, b);
}

void drawBGAnimationScanline(tic_mem* tic, s32 row)
{
    s32 dir = row < TIC80_HEIGHT / 2 ? 1 : -1;
    s32 val = (s32)(dir * (TIC80_WIDTH - row * 3.5f));
    modifyColor(tic, BG_ANIMATION_COLOR, (s32)(val * 0.75), (s32)(val * 0.8), val);
}

char getKeyboardText()
{
    char text;
    if(!tic_sys_keyboard_text(&text))
    {
        tic_mem* tic = impl.studio.tic;
        tic80_input* input = &tic->ram.input;

        static const char Symbols[] =   " abcdefghijklmnopqrstuvwxyz0123456789-=[]\\;'`,./ ";
        static const char Shift[] =     " ABCDEFGHIJKLMNOPQRSTUVWXYZ)!@#$%^&*(_+{}|:\"~<>? ";

        enum{Count = sizeof Symbols};

        for(s32 i = 0; i < TIC80_KEY_BUFFER; i++)
        {
            tic_key key = input->keyboard.keys[i];

            if(key > 0 && key < Count && tic_api_keyp(tic, key, KEYBOARD_HOLD, KEYBOARD_PERIOD))
            {
                bool caps = tic_api_key(tic, tic_key_capslock);
                bool shift = tic_api_key(tic, tic_key_shift);

                return caps
                    ? key >= tic_key_a && key <= tic_key_z 
                        ? shift ? Symbols[key] : Shift[key]
                        : shift ? Shift[key] : Symbols[key]
                    : shift ? Shift[key] : Symbols[key];
            }
        }

        return '\0';
    }

    return text;
}

bool keyWasPressed(tic_key key)
{
    tic_mem* tic = impl.studio.tic;
    return tic_api_keyp(tic, key, KEYBOARD_HOLD, KEYBOARD_PERIOD);
}

bool anyKeyWasPressed()
{
    tic_mem* tic = impl.studio.tic;

    for(s32 i = 0; i < TIC80_KEY_BUFFER; i++)
    {
        tic_key key = tic->ram.input.keyboard.keys[i];

        if(tic_api_keyp(tic, key, KEYBOARD_HOLD, KEYBOARD_PERIOD))
            return true;
    }

    return false;
}

tic_tiles* getBankTiles()
{
    return &impl.studio.tic->cart.banks[impl.bank.index.sprites].tiles;
}

tic_map* getBankMap()
{
    return &impl.studio.tic->cart.banks[impl.bank.index.map].map;
}

tic_palette* getBankPalette(bool ovr)
{
    tic_bank* bank = &impl.studio.tic->cart.banks[impl.bank.index.sprites];
    return ovr ? &bank->palette.ovr : &bank->palette.scn;
}

tic_flags* getBankFlags()
{
    return &impl.studio.tic->cart.banks[impl.bank.index.sprites].flags;
}

void playSystemSfx(s32 id)
{
    const tic_sample* effect = &impl.config->cart.bank0.sfx.samples.data[id];
    tic_api_sfx(impl.studio.tic, id, effect->note, effect->octave, -1, 0, MAX_VOLUME, MAX_VOLUME, effect->speed);
}

static void md5(const void* voidData, s32 length, u8 digest[MD5_HASHSIZE])
{
    enum {Size = 512};

    const u8* data = voidData;

    MD5_CTX c;
    MD5_Init(&c);

    while (length > 0)
    {
        MD5_Update(&c, data, length > Size ? Size: length);

        length -= Size;
        data += Size;
    }

    MD5_Final(digest, &c);
}

const char* md5str(const void* data, s32 length)
{
    static char res[MD5_HASHSIZE * 2 + 1];

    u8 digest[MD5_HASHSIZE];

    md5(data, length, digest);

    for (s32 n = 0; n < MD5_HASHSIZE; ++n)
        snprintf(res + n*2, sizeof("ff"), "%02x", digest[n]);

    return res;
}

static u8* getSpritePtr(tic_tile* tiles, s32 x, s32 y)
{
    enum { SheetCols = (TIC_SPRITESHEET_SIZE / TIC_SPRITESIZE) };
    return tiles[x / TIC_SPRITESIZE + y / TIC_SPRITESIZE * SheetCols].data;
}


void setSpritePixel(tic_tile* tiles, s32 x, s32 y, u8 color)
{
    // TODO: check spritesheet rect
    tic_tool_poke4(getSpritePtr(tiles, x, y), (x % TIC_SPRITESIZE) + (y % TIC_SPRITESIZE) * TIC_SPRITESIZE, color);
}

u8 getSpritePixel(tic_tile* tiles, s32 x, s32 y)
{
    // TODO: check spritesheet rect
    return tic_tool_peek4(getSpritePtr(tiles, x, y), (x % TIC_SPRITESIZE) + (y % TIC_SPRITESIZE) * TIC_SPRITESIZE);
}

void toClipboard(const void* data, s32 size, bool flip)
{
    if(data)
    {
        enum {Len = 2};

        char* clipboard = (char*)malloc(size*Len + 1);

        if(clipboard)
        {
            char* ptr = clipboard;

            for(s32 i = 0; i < size; i++, ptr+=Len)
            {
                sprintf(ptr, "%02x", ((u8*)data)[i]);

                if(flip)
                {
                    char tmp = ptr[0];
                    ptr[0] = ptr[1];
                    ptr[1] = tmp;
                }
            }

            tic_sys_clipboard_set(clipboard);
            free(clipboard);
        }
    }
}

static void removeWhiteSpaces(char* str)
{
    s32 i = 0;
    s32 len = (s32)strlen(str);

    for (s32 j = 0; j < len; j++)
        if(!isspace(str[j]))
            str[i++] = str[j];

    str[i] = '\0';
}

bool fromClipboard(void* data, s32 size, bool flip, bool remove_white_spaces)
{
    if(data)
    {
        if(tic_sys_clipboard_has())
        {
            char* clipboard = tic_sys_clipboard_get();

            if(clipboard)
            {
                if (remove_white_spaces)
                    removeWhiteSpaces(clipboard);
                            
                bool valid = strlen(clipboard) == size * 2;

                if(valid) tic_tool_str2buf(clipboard, (s32)strlen(clipboard), data, flip);

                tic_sys_clipboard_free(clipboard);

                return valid;
            }
        }
    }

    return false;
}

void showTooltip(const char* text)
{
    strncpy(impl.tooltip.text, text, sizeof impl.tooltip.text - 1);
}

static void drawExtrabar(tic_mem* tic)
{
    enum {Size = 7};

    s32 x = (COUNT_OF(Modes) + 1) * Size + 17 * TIC_FONT_WIDTH;
    s32 y = 0;

    static const u8 Icons[] =
    {
        0b00000000,
        0b00101000,
        0b00101000,
        0b00010000,
        0b01101100,
        0b01101100,
        0b00000000,
        0b00000000,

        0b00000000,
        0b01111000,
        0b01001000,
        0b01011100,
        0b01110100,
        0b00011100,
        0b00000000,
        0b00000000,

        0b00000000,
        0b00111000,
        0b01000100,
        0b01111100,
        0b01101100,
        0b01111100,
        0b00000000,
        0b00000000,

        0b00000000,
        0b00011000,
        0b00110000,
        0b01111100,
        0b00110000,
        0b00011000,
        0b00000000,
        0b00000000,

        0b00000000,
        0b00110000,
        0b00011000,
        0b01111100,
        0b00011000,
        0b00110000,
        0b00000000,
        0b00000000,
    };

    static const StudioEvent Events[] = {TIC_TOOLBAR_CUT, TIC_TOOLBAR_COPY, TIC_TOOLBAR_PASTE,  TIC_TOOLBAR_UNDO, TIC_TOOLBAR_REDO};
    static const char* Tips[] = {"CUT [ctrl+x]", "COPY [ctrl+c]", "PASTE [ctrl+v]", "UNDO [ctrl+z]", "REDO [ctrl+y]"};

    for(s32 i = 0; i < sizeof Icons / BITS_IN_BYTE; i++)
    {
        tic_rect rect = {x + i*Size, y, Size, Size};

        u8 bgcolor = tic_color_white;
        u8 color = tic_color_light_grey;

        if(checkMousePos(&rect))
        {
            setCursor(tic_cursor_hand);

            color = tic_color_red + i;
            showTooltip(Tips[i]);

            if(checkMouseDown(&rect, tic_mouse_left))
            {
                bgcolor = color;
                color = tic_color_white;
            }
            else if(checkMouseClick(&rect, tic_mouse_left))
            {
                setStudioEvent(Events[i]);
            }
        }

        tic_api_rect(tic, x + i * Size, y, Size, Size, bgcolor);
        drawBitIcon(x + i * Size, y, Icons + i*BITS_IN_BYTE, color);
    }
}

const StudioConfig* getConfig()
{
    return &impl.config->data;
}

#if defined (TIC80_PRO)

static void drawBankIcon(s32 x, s32 y)
{
    tic_mem* tic = impl.studio.tic;

    tic_rect rect = {x, y, TIC_FONT_WIDTH, TIC_FONT_HEIGHT};

    static const u8 Icon[] =
    {
        0b00000000,
        0b01111100,
        0b01000100,
        0b01000100,
        0b01111100,
        0b01111000,
        0b00000000,
        0b00000000,
    };

    bool over = false;
    EditorMode mode = 0;

    for(s32 i = 0; i < COUNT_OF(BankModes); i++)
        if(BankModes[i] == impl.mode)
        {
            mode = i;
            break;
        }

    if(checkMousePos(&rect))
    {
        setCursor(tic_cursor_hand);

        over = true;

        showTooltip("SWITCH BANK");

        if(checkMouseClick(&rect, tic_mouse_left))
            impl.bank.show = !impl.bank.show;
    }

    if(impl.bank.show)
    {
        drawBitIcon(x, y, Icon, tic_color_red);

        enum{Size = TOOLBAR_SIZE};

        for(s32 i = 0; i < TIC_EDITOR_BANKS; i++)
        {
            tic_rect rect = {x + 2 + (i+1)*Size, 0, Size, Size};

            bool over = false;
            if(checkMousePos(&rect))
            {
                setCursor(tic_cursor_hand);
                over = true;

                if(checkMouseClick(&rect, tic_mouse_left))
                {
                    if(impl.bank.chained) 
                        memset(impl.bank.indexes, i, sizeof impl.bank.indexes);
                    else impl.bank.indexes[mode] = i;
                }
            }

            if(i == impl.bank.indexes[mode])
                tic_api_rect(tic, rect.x, rect.y, rect.w, rect.h, tic_color_red);

            tic_api_print(tic, (char[]){'0' + i, '\0'}, rect.x+1, rect.y+1, i == impl.bank.indexes[mode] ? tic_color_white : over ? tic_color_red : tic_color_light_grey, false, 1, false);

        }

        {
            static const u8 PinIcon[] =
            {
                0b00000000,
                0b00111000,
                0b00101000,
                0b01111100,
                0b00010000,
                0b00010000,
                0b00000000,
                0b00000000,
            };

            tic_rect rect = {x + 4 + (TIC_EDITOR_BANKS+1)*Size, 0, Size, Size};

            bool over = false;

            if(checkMousePos(&rect))
            {
                setCursor(tic_cursor_hand);

                over = true;

                if(checkMouseClick(&rect, tic_mouse_left))
                {
                    impl.bank.chained = !impl.bank.chained;

                    if(impl.bank.chained)
                        memset(impl.bank.indexes, impl.bank.indexes[mode], sizeof impl.bank.indexes);
                }
            }

            drawBitIcon(rect.x, rect.y, PinIcon, impl.bank.chained ? tic_color_red : over ? tic_color_grey : tic_color_light_grey);
        }
    }
    else
    {
        drawBitIcon(x, y, Icon, over ? tic_color_red : tic_color_light_grey);
    }
}

#endif

void drawToolbar(tic_mem* tic, bool bg)
{
    if(bg)
        tic_api_rect(tic, 0, 0, TIC80_WIDTH, TOOLBAR_SIZE, tic_color_white);

    static const u8 TabIcon[] =
    {
        0b11111110,
        0b11111110,
        0b11111110,
        0b11111110,
        0b11111110,
        0b11111110,
        0b11111110,
        0b00000000,
    };

    static const u8 Icons[] =
    {
        0b00000000,
        0b01101100,
        0b01000100,
        0b01000100,
        0b01000100,
        0b01101100,
        0b00000000,
        0b00000000,

        0b00000000,
        0b00111000,
        0b01010100,
        0b01111100,
        0b01111100,
        0b01010100,
        0b00000000,
        0b00000000,

        0b00000000,
        0b01101100,
        0b01101100,
        0b00000000,
        0b01101100,
        0b01101100,
        0b00000000,
        0b00000000,

        0b00000000,
        0b00011000,
        0b00110100,
        0b01110100,
        0b00110100,
        0b00011000,
        0b00000000,
        0b00000000,

        0b00000000,
        0b00111100,
        0b00100100,
        0b00100100,
        0b01101100,
        0b01101100,
        0b00000000,
        0b00000000,
    };

    enum {Size = 7};

    static const char* Tips[] = {"CODE EDITOR [f1]", "SPRITE EDITOR [f2]", "MAP EDITOR [f3]", "SFX EDITOR [f4]", "MUSIC EDITOR [f5]",};

    s32 mode = -1;

    for(s32 i = 0; i < COUNT_OF(Modes); i++)
    {
        tic_rect rect = {i * Size, 0, Size, Size};

        bool over = false;

        if(checkMousePos(&rect))
        {
            setCursor(tic_cursor_hand);

            over = true;

            showTooltip(Tips[i]);

            if(checkMouseClick(&rect, tic_mouse_left))
                setStudioMode(Modes[i]);
        }

        if(getStudioMode() == Modes[i]) mode = i;

        if (mode == i)
        {
            drawBitIcon(i * Size, 0, TabIcon, tic_color_grey);
            drawBitIcon(i * Size, 1, Icons + i * BITS_IN_BYTE, tic_color_black);
        }

        drawBitIcon(i * Size, 0, Icons + i * BITS_IN_BYTE, mode == i ? tic_color_white : (over ? tic_color_grey : tic_color_light_grey));
    }

    if(mode >= 0) drawExtrabar(tic);

    static const char* Names[] =
    {
        "CODE EDITOR",
        "SPRITE EDITOR",
        "MAP EDITOR",
        "SFX EDITOR",
        "MUSIC EDITOR",
    };

#if defined (TIC80_PRO)
    enum {TextOffset = (COUNT_OF(Modes) + 2) * Size - 2};
    if(mode >= 1)
        drawBankIcon(COUNT_OF(Modes) * Size + 2, 0);
#else
    enum {TextOffset = (COUNT_OF(Modes) + 1) * Size};
#endif

    if(mode == 0 || (mode >= 1 && !impl.bank.show))
    {
        if(strlen(impl.tooltip.text))
        {
            tic_api_print(tic, impl.tooltip.text, TextOffset, 1, tic_color_dark_grey, false, 1, false);
        }
        else
        {
            tic_api_print(tic, Names[mode], TextOffset, 1, tic_color_grey, false, 1, false);
        }
    }
}

void setStudioEvent(StudioEvent event)
{
    switch(impl.mode)
    {
    case TIC_CODE_MODE:     
        {
            Code* code = impl.code;
            code->event(code, event);           
        }
        break;
    case TIC_SPRITE_MODE:   
        {
            Sprite* sprite = impl.banks.sprite[impl.bank.index.sprites];
            sprite->event(sprite, event); 
        }
    break;
    case TIC_MAP_MODE:
        {
            Map* map = impl.banks.map[impl.bank.index.map];
            map->event(map, event);
        }
        break;
    case TIC_SFX_MODE:
        {
            Sfx* sfx = impl.banks.sfx[impl.bank.index.sfx];
            sfx->event(sfx, event);
        }
        break;
    case TIC_MUSIC_MODE:
        {
            Music* music = impl.banks.music[impl.bank.index.music];
            music->event(music, event);
        }
        break;
    default: break;
    }
}

ClipboardEvent getClipboardEvent()
{
    tic_mem* tic = impl.studio.tic;

    bool shift = tic_api_key(tic, tic_key_shift);
    bool ctrl = tic_api_key(tic, tic_key_ctrl);

    if(ctrl)
    {
        if(keyWasPressed(tic_key_insert) || keyWasPressed(tic_key_c)) return TIC_CLIPBOARD_COPY;
        else if(keyWasPressed(tic_key_x)) return TIC_CLIPBOARD_CUT;
        else if(keyWasPressed(tic_key_v)) return TIC_CLIPBOARD_PASTE;
    }
    else if(shift)
    {
        if(keyWasPressed(tic_key_delete)) return TIC_CLIPBOARD_CUT;
        else if(keyWasPressed(tic_key_insert)) return TIC_CLIPBOARD_PASTE;
    }

    return TIC_CLIPBOARD_NONE;
}

static void showPopupMessage(const char* text)
{
    impl.popup.counter = POPUP_DUR;
    memset(impl.popup.message, '\0', sizeof impl.popup.message);
    strncpy(impl.popup.message, text, sizeof(impl.popup.message) - 1);

    for(char* c = impl.popup.message; c < impl.popup.message + sizeof impl.popup.message; c++)
        if(*c) *c = toupper(*c);
}

static void exitConfirm(bool yes, void* data)
{
    impl.studio.quit = yes;
}

void exitStudio()
{
    if(impl.mode != TIC_START_MODE && studioCartChanged())
    {
        static const char* Rows[] =
        {
            "YOU HAVE",
            "UNSAVED CHANGES",
            "",
            "DO YOU REALLY WANT",
            "TO EXIT?",
        };

        showDialog(Rows, COUNT_OF(Rows), exitConfirm, NULL);
    }
    else exitConfirm(true, NULL);
}

void drawBitIcon(s32 x, s32 y, const u8* ptr, u8 color)
{
    for(s32 i = 0; i < TIC_SPRITESIZE; i++, ptr++)
        for(s32 col = 0; col < TIC_SPRITESIZE; col++)
            if(*ptr & 1 << col)
                tic_api_pix(impl.studio.tic, x - col + (TIC_SPRITESIZE - 1), y + i, color, false);
}

void drawBitIcon16(tic_mem* tic, s32 x, s32 y, const u16* ptr, u8 color)
{
    for(s32 i = 0; i < TIC_SPRITESIZE*2; i++, ptr++)
        for(s32 col = 0; col < TIC_SPRITESIZE*2; col++)
            if(*ptr & 1 << col)
                tic_api_pix(tic, x - col + (TIC_SPRITESIZE*2 - 1), y + i, color, false);
}

static void initWorldMap()
{
    initWorld(impl.world, impl.studio.tic, impl.banks.map[impl.bank.index.map]);
}

static void initRunMode()
{
    initRun(impl.run, impl.console, impl.studio.tic);
}

static void initSurfMode()
{
    initSurf(impl.surf, impl.studio.tic, impl.console);
}

void gotoSurf()
{
    initSurfMode();
    setStudioMode(TIC_SURF_MODE);
}

void gotoCode()
{
    setStudioMode(TIC_CODE_MODE);
}

static void initMenuMode()
{
    initMenu(impl.menu, impl.studio.tic, impl.fs);
}

void runGameFromSurf()
{
    tic_api_reset(impl.studio.tic);
    setStudioMode(TIC_RUN_MODE);
    impl.prevMode = TIC_SURF_MODE;
}

void exitGameMenu()
{
    if(impl.prevMode == TIC_SURF_MODE)
    {
        setStudioMode(TIC_SURF_MODE);
    }
    else
    {
        setStudioMode(TIC_CONSOLE_MODE);
    }

    impl.console->showGameMenu = false;
}

void resumeRunMode()
{
    impl.mode = TIC_RUN_MODE;
}

void setStudioMode(EditorMode mode)
{
    if(mode != impl.mode)
    {
        EditorMode prev = impl.mode;

        if(prev == TIC_RUN_MODE)
            tic_core_pause(impl.studio.tic);

        if(mode != TIC_RUN_MODE)
            tic_api_reset(impl.studio.tic);

        switch (prev)
        {
        case TIC_START_MODE:
        case TIC_CONSOLE_MODE:
        case TIC_RUN_MODE:
        case TIC_DIALOG_MODE:
        case TIC_MENU_MODE:
            break;
        case TIC_SURF_MODE:
            impl.prevMode = TIC_CODE_MODE;
            break;
        default: impl.prevMode = prev; break;
        }

        switch(mode)
        {
        case TIC_WORLD_MODE: initWorldMap(); break;
        case TIC_RUN_MODE: initRunMode(); break;
        case TIC_SURF_MODE: impl.surf->resume(impl.surf); break;
        default: break;
        }

        impl.mode = mode;
    }
}

EditorMode getStudioMode()
{
    return impl.mode;
}

void changeStudioMode(s32 dir)
{
    const size_t modeCount = sizeof(Modes)/sizeof(Modes[0]);
    for(size_t i = 0; i < modeCount; i++)
    {
        if(impl.mode == Modes[i])
        {
            setStudioMode(Modes[(i+dir+modeCount) % modeCount]);
            return;
        }
    }
}

void showGameMenu()
{
    tic_core_pause(impl.studio.tic);
    tic_api_reset(impl.studio.tic);

    initMenuMode();
    impl.mode = TIC_MENU_MODE;
}

void hideGameMenu()
{
    tic_core_resume(impl.studio.tic);
    impl.mode = TIC_RUN_MODE;
}

static inline bool pointInRect(const tic_point* pt, const tic_rect* rect)
{
    return (pt->x >= rect->x) 
        && (pt->x < (rect->x + rect->w)) 
        && (pt->y >= rect->y)
        && (pt->y < (rect->y + rect->h));
}

bool checkMousePos(const tic_rect* rect)
{
    tic_point pos = tic_api_mouse(impl.studio.tic);
    return pointInRect(&pos, rect);
}

bool checkMouseClick(const tic_rect* rect, tic_mouse_btn button)
{
    MouseState* state = &impl.mouse.state[button];

    bool value = state->click
        && pointInRect(&state->start, rect)
        && pointInRect(&state->end, rect);

    if(value) state->click = false;

    return value;
}

bool checkMouseDown(const tic_rect* rect, tic_mouse_btn button)
{
    MouseState* state = &impl.mouse.state[button];

    return state->down && pointInRect(&state->start, rect);
}

void setCursor(tic_cursor id)
{
    tic_mem* tic = impl.studio.tic;

    tic->ram.vram.vars.cursor.sprite = id;
}

void hideDialog()
{
    if(impl.dialogMode == TIC_RUN_MODE)
    {
        tic_core_resume(impl.studio.tic);
        impl.mode = TIC_RUN_MODE;
    }
    else setStudioMode(impl.dialogMode);
}

void showDialog(const char** text, s32 rows, DialogCallback callback, void* data)
{
    if(impl.mode != TIC_DIALOG_MODE)
    {
        initDialog(impl.dialog, impl.studio.tic, text, rows, callback, data);
        impl.dialogMode = impl.mode;
        setStudioMode(TIC_DIALOG_MODE);
    }
}

static void resetBanks()
{
    memset(impl.bank.indexes, 0, sizeof impl.bank.indexes);
}

static void initModules()
{
    tic_mem* tic = impl.studio.tic;

    resetBanks();

    initCode(impl.code, impl.studio.tic, &tic->cart.code);

    for(s32 i = 0; i < TIC_EDITOR_BANKS; i++)
    {
        initSprite(impl.banks.sprite[i], impl.studio.tic, &tic->cart.banks[i].tiles);
        initMap(impl.banks.map[i], impl.studio.tic, &tic->cart.banks[i].map);
        initSfx(impl.banks.sfx[i], impl.studio.tic, &tic->cart.banks[i].sfx);
        initMusic(impl.banks.music[i], impl.studio.tic, &tic->cart.banks[i].music);
    }

    initWorldMap();
}

static void updateHash()
{
    md5(&impl.studio.tic->cart, sizeof(tic_cartridge), impl.cart.hash.data);
}

static void updateMDate()
{
    impl.cart.mdate = fs_date(impl.console->rom.path);
}

static void updateTitle()
{
    char name[TICNAME_MAX] = TIC_TITLE;

    if(strlen(impl.console->rom.name))
        snprintf(name, TICNAME_MAX, "%s [%s]", TIC_TITLE, impl.console->rom.name);

    tic_sys_title(name);
}

void studioRomSaved()
{
    updateTitle();
    updateHash();
    updateMDate();
}

void studioRomLoaded()
{
    initModules();

    updateTitle();
    updateHash();
    updateMDate();
}

bool studioCartChanged()
{
    CartHash hash;
    md5(&impl.studio.tic->cart, sizeof(tic_cartridge), hash.data);

    return memcmp(hash.data, impl.cart.hash.data, sizeof(CartHash)) != 0;
}

tic_key* getKeymap()
{
    return impl.keycodes;
}

static void processGamepadMapping()
{
    tic_mem* tic = impl.studio.tic;

    for(s32 i = 0; i < KEYMAP_COUNT; i++)
        if(impl.keycodes[i] && tic_api_key(tic, impl.keycodes[i]))
            tic->ram.input.gamepads.data |= 1 << i;
}

static inline bool isGameMenu()
{
    return (impl.mode == TIC_RUN_MODE || impl.mode == TIC_MENU_MODE) && impl.console->showGameMenu;
}

void runProject()
{
    tic_api_reset(impl.studio.tic);

    if(impl.mode == TIC_RUN_MODE)
    {
        initRunMode();
    }
    else setStudioMode(TIC_RUN_MODE);
}

static void saveProject()
{
    CartSaveResult rom = impl.console->save(impl.console);

    if(rom == CART_SAVE_OK)
    {
        char buffer[STUDIO_TEXT_BUFFER_WIDTH];
        char str_saved[] = " saved :)";

        s32 name_len = (s32)strlen(impl.console->rom.name);
        if (name_len + strlen(str_saved) > sizeof(buffer)){
            char subbuf[sizeof(buffer) - sizeof(str_saved) - 5];
            memset(subbuf, '\0', sizeof subbuf);
            strncpy(subbuf, impl.console->rom.name, sizeof subbuf-1);

            snprintf(buffer, sizeof buffer, "%s[...]%s", subbuf, str_saved);
        }
        else
        {
            snprintf(buffer, sizeof buffer, "%s%s", impl.console->rom.name, str_saved);
        }

        showPopupMessage(buffer);
    }
    else if(rom == CART_SAVE_MISSING_NAME) showPopupMessage("error: missing cart name :(");
    else showPopupMessage("error: file not saved :(");
}

static void screen2buffer(u32* buffer, const u32* pixels, const tic_rect* rect)
{
    pixels += rect->y * TIC80_FULLWIDTH;

    for(s32 i = 0; i < rect->h; i++)
    {
        memcpy(buffer, pixels + rect->x, rect->w * sizeof(pixels[0]));
        pixels += TIC80_FULLWIDTH;
        buffer += rect->w;
    }
}

static void setCoverImage()
{
    tic_mem* tic = impl.studio.tic;

    if(impl.mode == TIC_RUN_MODE)
    {
        enum {Pitch = TIC80_FULLWIDTH*sizeof(u32)};

        tic_core_blit(tic, TIC80_PIXEL_COLOR_RGBA8888);

        u32* buffer = malloc(TIC80_WIDTH * TIC80_HEIGHT * sizeof(u32));

        if(buffer)
        {
            enum{OffsetLeft = (TIC80_FULLWIDTH-TIC80_WIDTH)/2, OffsetTop = (TIC80_FULLHEIGHT-TIC80_HEIGHT)/2};

            tic_rect rect = {OffsetLeft, OffsetTop, TIC80_WIDTH, TIC80_HEIGHT};

            screen2buffer(buffer, tic->screen, &rect);

            gif_write_animation(impl.studio.tic->cart.cover.data, &impl.studio.tic->cart.cover.size,
                TIC80_WIDTH, TIC80_HEIGHT, (const u8*)buffer, 1, TIC80_FRAMERATE, 1);

            free(buffer);

            showPopupMessage("cover image saved :)");
        }
    }
}

static void stopVideoRecord(const char* name)
{
    if(impl.video.buffer)
    {
        {
            s32 size = 0;
            u8* data = malloc(FRAME_SIZE * impl.video.frame);
            s32 i = 0;
            char filename[TICNAME_MAX];

            gif_write_animation(data, &size, TIC80_FULLWIDTH, TIC80_FULLHEIGHT, (const u8*)impl.video.buffer, impl.video.frame, TIC80_FRAMERATE, getConfig()->gifScale);

            // Find an available filename to save.
            do
            {
                snprintf(filename, sizeof filename, name, ++i);
            }
            while(tic_fs_exists(impl.fs, filename));

            // Now that it has found an available filename, save it.
            if(tic_fs_save(impl.fs, filename, data, size, true))
            {
                char msg[TICNAME_MAX];
                sprintf(msg, "%s saved :)", filename);
                showPopupMessage(msg);

                tic_sys_open_path(tic_fs_path(impl.fs, filename));
            }
            else showPopupMessage("error: file not saved :(");
        }

        free(impl.video.buffer);
        impl.video.buffer = NULL;
    }

    impl.video.record = false;
}

static void startVideoRecord()
{
    if(impl.video.record)
    {
        stopVideoRecord(VideoGif);
    }
    else
    {
        impl.video.frames = getConfig()->gifLength * TIC80_FRAMERATE;
        impl.video.buffer = malloc(FRAME_SIZE * impl.video.frames);

        if(impl.video.buffer)
        {
            impl.video.record = true;
            impl.video.frame = 0;
        }
    }
}

static void takeScreenshot()
{
    impl.video.frames = 1;
    impl.video.buffer = malloc(FRAME_SIZE);

    if(impl.video.buffer)
    {
        impl.video.record = true;
        impl.video.frame = 0;
    }
}

static inline bool keyWasPressedOnce(s32 key)
{
    tic_mem* tic = impl.studio.tic;

    return tic_api_keyp(tic, key, -1, -1);
}

#if defined(CRT_SHADER_SUPPORT)
void switchCrtMonitor()
{
    impl.config->data.crtMonitor = !impl.config->data.crtMonitor;
}
#endif

static void processShortcuts()
{
    tic_mem* tic = impl.studio.tic;

    if(impl.mode == TIC_START_MODE) return;
    if(impl.mode == TIC_CONSOLE_MODE && !impl.console->active) return;

    bool alt = tic_api_key(tic, tic_key_alt);
    bool ctrl = tic_api_key(tic, tic_key_ctrl);

#if defined(CRT_SHADER_SUPPORT)
    if(keyWasPressedOnce(tic_key_f6)) switchCrtMonitor();
#endif

    if(isGameMenu())
    {
        if(keyWasPressedOnce(tic_key_escape))
        {
            impl.mode == TIC_MENU_MODE ? hideGameMenu() : showGameMenu();
        }
        else if(keyWasPressedOnce(tic_key_f11)) tic_sys_fullscreen();
        else if(keyWasPressedOnce(tic_key_return))
        {
            if(alt) tic_sys_fullscreen();
        }
        else if(keyWasPressedOnce(tic_key_f7)) setCoverImage();
        else if(keyWasPressedOnce(tic_key_f8)) takeScreenshot();
        else if(keyWasPressedOnce(tic_key_r))
        {
            if(ctrl) runProject();
        }
        else if(keyWasPressedOnce(tic_key_f9)) startVideoRecord();

        return;
    }

    if(alt)
    {
        if(keyWasPressedOnce(tic_key_grave)) setStudioMode(TIC_CONSOLE_MODE);
        else if(keyWasPressedOnce(tic_key_1)) setStudioMode(TIC_CODE_MODE);
        else if(keyWasPressedOnce(tic_key_2)) setStudioMode(TIC_SPRITE_MODE);
        else if(keyWasPressedOnce(tic_key_3)) setStudioMode(TIC_MAP_MODE);
        else if(keyWasPressedOnce(tic_key_4)) setStudioMode(TIC_SFX_MODE);
        else if(keyWasPressedOnce(tic_key_5)) setStudioMode(TIC_MUSIC_MODE);
        else if(keyWasPressedOnce(tic_key_return)) tic_sys_fullscreen();
    }
    else if(ctrl)
    {
        if(keyWasPressedOnce(tic_key_pageup)) changeStudioMode(-1);
        else if(keyWasPressedOnce(tic_key_pagedown)) changeStudioMode(1);
        else if(keyWasPressedOnce(tic_key_q)) exitStudio();
        else if(keyWasPressedOnce(tic_key_r)) runProject();
        else if(keyWasPressedOnce(tic_key_return)) runProject();
        else if(keyWasPressedOnce(tic_key_s)) saveProject();
    }
    else
    {
        if(keyWasPressedOnce(tic_key_f1)) setStudioMode(TIC_CODE_MODE);
        else if(keyWasPressedOnce(tic_key_f2)) setStudioMode(TIC_SPRITE_MODE);
        else if(keyWasPressedOnce(tic_key_f3)) setStudioMode(TIC_MAP_MODE);
        else if(keyWasPressedOnce(tic_key_f4)) setStudioMode(TIC_SFX_MODE);
        else if(keyWasPressedOnce(tic_key_f5)) setStudioMode(TIC_MUSIC_MODE);
        else if(keyWasPressedOnce(tic_key_f7)) setCoverImage();
        else if(keyWasPressedOnce(tic_key_f8)) takeScreenshot();
        else if(keyWasPressedOnce(tic_key_f9)) startVideoRecord();
        else if(keyWasPressedOnce(tic_key_f11)) tic_sys_fullscreen();
        else if(keyWasPressedOnce(tic_key_escape))
        {
            Code* code = impl.code;

            if(impl.mode == TIC_CODE_MODE && code->mode != TEXT_EDIT_MODE)
            {
                code->escape(code);
                return;
            }

            if(impl.mode == TIC_DIALOG_MODE)
            {
                impl.dialog->escape(impl.dialog);
                return;
            }

            setStudioMode(impl.mode == TIC_CONSOLE_MODE ? impl.prevMode : TIC_CONSOLE_MODE);
        }
    }
}

static void reloadConfirm(bool yes, void* data)
{
    if(yes)
        impl.console->updateProject(impl.console);
}

static void updateStudioProject()
{
    if(impl.mode != TIC_START_MODE)
    {
        Console* console = impl.console;

        u64 date = fs_date(console->rom.path);

        if(impl.cart.mdate && date > impl.cart.mdate)
        {
            if(studioCartChanged())
            {
                static const char* Rows[] =
                {
                    "",
                    "CART HAS CHANGED!",
                    "",
                    "DO YOU WANT",
                    "TO RELOAD IT?"
                };

                showDialog(Rows, COUNT_OF(Rows), reloadConfirm, NULL);
            }
            else console->updateProject(console);
        }
    }
}

static void drawRecordLabel(u32* frame, s32 sx, s32 sy, const u32* color)
{
    static const u16 RecLabel[] =
    {
        0b0111001100110011,
        0b1111101010100100,
        0b1111101100110100,
        0b1111101010100100,
        0b0111001010110011,
    };

    for(s32 y = 0; y < 5; y++)
    {
        for(s32 x = 0; x < sizeof RecLabel[0]*BITS_IN_BYTE; x++)
        {
            if(RecLabel[y] & (1 << x))
                memcpy(&frame[sx + 15 - x + ((y+sy) << TIC80_FULLWIDTH_BITS)], color, sizeof *color);
        }
    }
}

static bool isRecordFrame(void)
{
    return impl.video.record;
}

static void recordFrame(u32* pixels)
{
    if(impl.video.record)
    {
        if(impl.video.frame < impl.video.frames)
        {
            tic_rect rect = {0, 0, TIC80_FULLWIDTH, TIC80_FULLHEIGHT};
            screen2buffer(impl.video.buffer + (TIC80_FULLWIDTH*TIC80_FULLHEIGHT) * impl.video.frame, pixels, &rect);

            if(impl.video.frame % TIC80_FRAMERATE < TIC80_FRAMERATE / 2)
            {
                const u32* pal = tic_tool_palette_blit(&impl.config->cart.bank0.palette.scn, TIC80_PIXEL_COLOR_RGBA8888);
                drawRecordLabel(pixels, TIC80_WIDTH-24, 8, &pal[tic_color_red]);
            }

            impl.video.frame++;

        }
        else
        {
            stopVideoRecord(impl.video.frame == 1 ? ScreenGif : VideoGif);
        }
    }
}

static void drawPopup()
{
    if(impl.popup.counter > 0)
    {
        impl.popup.counter--;

        s32 anim = 0;

        enum{Dur = TIC80_FRAMERATE/2};

        if(impl.popup.counter < Dur)
            anim = -((Dur - impl.popup.counter) * (TIC_FONT_HEIGHT+1) / Dur);
        else if(impl.popup.counter >= (POPUP_DUR - Dur))
            anim = (((POPUP_DUR - Dur) - impl.popup.counter) * (TIC_FONT_HEIGHT+1) / Dur);

        tic_api_rect(impl.studio.tic, 0, anim, TIC80_WIDTH, TIC_FONT_HEIGHT+1, tic_color_red);
        tic_api_print(impl.studio.tic, impl.popup.message, 
            (s32)(TIC80_WIDTH - strlen(impl.popup.message)*TIC_FONT_WIDTH)/2,
            anim + 1, tic_color_white, true, 1, false);
    }
}

static void renderStudio()
{
    tic_mem* tic = impl.studio.tic;

    showTooltip("");

    {
        const tic_sfx* sfx = NULL;
        const tic_music* music = NULL;

        switch(impl.mode)
        {
        case TIC_RUN_MODE:
            sfx = &impl.studio.tic->ram.sfx;
            music = &impl.studio.tic->ram.music;
            break;
        case TIC_START_MODE:
        case TIC_DIALOG_MODE:
        case TIC_MENU_MODE:
        case TIC_SURF_MODE:
            sfx = &impl.config->cart.bank0.sfx;
            music = &impl.config->cart.bank0.music;
            break;
        default:
            sfx = getSfxSrc();
            music = getMusicSrc();
        }

        sfx2ram(&tic->ram, sfx);
        music2ram(&tic->ram, music);

        tic_core_tick_start(impl.studio.tic);
    }

    switch(impl.mode)
    {
    case TIC_START_MODE:    impl.start->tick(impl.start); break;
    case TIC_CONSOLE_MODE:  impl.console->tick(impl.console); break;
    case TIC_RUN_MODE:      impl.run->tick(impl.run); break;
    case TIC_CODE_MODE:     
        {
            Code* code = impl.code;
            code->tick(code);
        }
        break;
    case TIC_SPRITE_MODE:   
        {
            Sprite* sprite = impl.banks.sprite[impl.bank.index.sprites];
            sprite->tick(sprite);       
        }
        break;
    case TIC_MAP_MODE:
        {
            Map* map = impl.banks.map[impl.bank.index.map];
            map->tick(map);
        }
        break;
    case TIC_SFX_MODE:
        {
            Sfx* sfx = impl.banks.sfx[impl.bank.index.sfx];
            sfx->tick(sfx);
        }
        break;
    case TIC_MUSIC_MODE:
        {
            Music* music = impl.banks.music[impl.bank.index.music];
            music->tick(music);
        }
        break;

    case TIC_WORLD_MODE:    impl.world->tick(impl.world); break;
    case TIC_DIALOG_MODE:   impl.dialog->tick(impl.dialog); break;
    case TIC_MENU_MODE:     impl.menu->tick(impl.menu); break;
    case TIC_SURF_MODE:     impl.surf->tick(impl.surf); break;
    default: break;
    }

    if(getConfig()->noSound)
        memset(tic->ram.registers, 0, sizeof tic->ram.registers);

    tic_core_tick_end(impl.studio.tic);

    switch(impl.mode)
    {
    case TIC_RUN_MODE: break;
    case TIC_SURF_MODE:
    case TIC_MENU_MODE:
        tic->input.data = -1;
        break;
    default:
        tic->input.data = -1;
        tic->input.gamepad = 0;
    }
}

static void updateSystemFont()
{
    tic_mem* tic = impl.studio.tic;

    memset(impl.systemFont.data, 0, sizeof(tic_font));

    for(s32 i = 0; i < TIC_FONT_CHARS; i++)
        for(s32 y = 0; y < TIC_SPRITESIZE; y++)
            for(s32 x = 0; x < TIC_SPRITESIZE; x++)
                if(tic_tool_peek4(&impl.config->cart.bank0.sprites.data[i], TIC_SPRITESIZE*y + x))
                    impl.systemFont.data[i*BITS_IN_BYTE+y] |= 1 << x;

    memcpy(tic->ram.font.data, impl.systemFont.data, sizeof(tic_font));
}

void studioConfigChanged()
{
    Code* code = impl.code;
    if(code->update)
        code->update(code);

    updateSystemFont();

    tic_sys_update_config();
}

static void initKeymap()
{
    tic_fs* fs = impl.fs;

    s32 size = 0;
    u8* data = (u8*)tic_fs_load(fs, KEYMAP_DAT_PATH, &size);

    if(data)
    {
        if(size == KEYMAP_SIZE)
            memcpy(getKeymap(), data, KEYMAP_SIZE);

        free(data);
    }
}

static void processMouseStates()
{
    for(s32 i = 0; i < COUNT_OF(impl.mouse.state); i++)
        impl.mouse.state[i].click = false;

    tic_mem* tic = impl.studio.tic;

    tic->ram.vram.vars.cursor.sprite = tic_cursor_arrow;
    tic->ram.vram.vars.cursor.system = true;

    for(s32 i = 0; i < COUNT_OF(impl.mouse.state); i++)
    {
        MouseState* state = &impl.mouse.state[i];

        if(!state->down && (tic->ram.input.mouse.btns & (1 << i)))
        {
            state->down = true;
            state->start = tic_api_mouse(tic);
        }
        else if(state->down && !(tic->ram.input.mouse.btns & (1 << i)))
        {
            state->end = tic_api_mouse(tic);

            state->click = true;
            state->down = false;
        }
    }
}

static void studioTick()
{
    tic_mem* tic = impl.studio.tic;

    tic_net_start(impl.net);
    processShortcuts();
    processMouseStates();
    processGamepadMapping();

    renderStudio();
    
    {
        tic_scanline scanline = NULL;
        tic_overline overline = NULL;
        void* data = NULL;

        switch(impl.mode)
        {
        case TIC_SPRITE_MODE:
            {
                Sprite* sprite = impl.banks.sprite[impl.bank.index.sprites];
                overline = sprite->overline;
                scanline = sprite->scanline;
                data = sprite;
            }
            break;
        case TIC_MAP_MODE:
            {
                Map* map = impl.banks.map[impl.bank.index.map];
                overline = map->overline;
                scanline = map->scanline;
                data = map;
            }
            break;
        case TIC_WORLD_MODE:
            {
                overline = impl.world->overline;
                scanline = impl.world->scanline;
                data = impl.world;
            }
            break;
        case TIC_DIALOG_MODE:
            {
                overline = impl.dialog->overline;
                scanline = impl.dialog->scanline;
                data = impl.dialog;
            }
            break;
        case TIC_MENU_MODE:
            {
                overline = impl.menu->overline;
                scanline = impl.menu->scanline;
                data = impl.menu;
            }
            break;
        case TIC_SURF_MODE:
            {
                overline = impl.surf->overline;
                scanline = impl.surf->scanline;
                data = impl.surf;
            }
            break;
        default:
            break;
        }

        if(impl.mode != TIC_RUN_MODE)
        {
            memcpy(tic->ram.vram.palette.data, getConfig()->cart->bank0.palette.scn.data, sizeof(tic_palette));
            memcpy(tic->ram.font.data, impl.systemFont.data, sizeof(tic_font));
        }

        data
            ? tic_core_blit_ex(tic, tic->screen_format, scanline, overline, data)
            : tic_core_blit(tic, tic->screen_format);

        if(isRecordFrame())
            recordFrame(tic->screen);
    }

    drawPopup();

    tic_net_end(impl.net);
}

static void studioClose()
{
    {
        for(s32 i = 0; i < TIC_EDITOR_BANKS; i++)
        {
            freeSprite  (impl.banks.sprite[i]);
            freeMap     (impl.banks.map[i]);
            freeSfx     (impl.banks.sfx[i]);
            freeMusic   (impl.banks.music[i]);
        }

        freeCode    (impl.code);
        freeStart   (impl.start);
        freeConsole (impl.console);
        freeRun     (impl.run);
        freeWorld   (impl.world);
        freeConfig  (impl.config);
        freeDialog  (impl.dialog);
        freeMenu    (impl.menu);
        freeSurf    (impl.surf);
    }

    if(impl.tic80local)
        tic80_delete((tic80*)impl.tic80local);

    tic_net_close(impl.net);
    free(impl.fs);
}

static StartArgs parseArgs(s32 argc, const char **argv)
{
    static const char *const usage[] = 
    {
        "tic80 [cart] [options]",
        NULL,
    };

    StartArgs args = {0};

    struct argparse_option options[] = 
    {
        OPT_HELP(),
        OPT_BOOLEAN('\0',   "skip",         &args.skip,         "skip startup animation"),
        OPT_BOOLEAN('\0',   "nosound",      &args.nosound,      "disable sound output"),
        OPT_BOOLEAN('\0',   "fullscreen",   &args.fullscreen,   "enable fullscreen mode"),
        OPT_STRING('\0',    "fs",           &args.fs,           "path to the file system folder"),
        OPT_INTEGER('\0',   "scale",        &args.scale,        "main window scale"),
#if defined(CRT_SHADER_SUPPORT)
        OPT_BOOLEAN('\0',   "crt",          &args.crt,          "enable CRT monitor effect"),
#endif
        OPT_STRING('\0',    "cmd",          &args.cmd,          "run commands in the console"),
        OPT_END(),
    };

    struct argparse argparse;
    argparse_init(&argparse, options, usage, 0);
    argparse_describe(&argparse, "\n" TIC_NAME " startup options:", NULL);
    argc = argparse_parse(&argparse, argc, argv);

    if(argc == 1)
        args.cart = argv[0];

    return args;
}

#include "png.h"

static void bitcpy(u8* dst, u32 to, const u8* src, u32 from, u32 size)
{
    for(s32 i = 0; i < size; i++, to++, from++)
        if(src[from >> 3] & 1 << (from & 7))
            dst[to >> 3] |= 1 << (to & 7);
        else 
            dst[to >> 3] &= ~(1 << (to & 7));
}

static png_buffer encodeCart(s32 bits, png_buffer cart)
{
    static u8 Cover[] =
    {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x08, 0x06, 0x00, 0x00, 0x00, 0x5c, 0x72, 0xa8, 0x66, 0x00, 0x00, 0x00, 0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xae, 0xce, 0x1c, 0xe9, 0x00, 0x00, 0x07, 0x9b, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0xed, 0xdd, 0xb1, 0x6a, 0x23, 0xdb, 0x01, 0xc7, 0xe1, 0xe3, 0x65, 0x5f, 0x62, 0x09, 0x5c, 0x16, 0x52, 0x6e, 0x91, 0x40, 0x52, 0x0a, 0x57, 0x6e, 0x52, 0xc5, 0x5d, 0xc0, 0x9d, 0x6b, 0x5f, 0x48, 0xe3, 0xc7, 0x70, 0x73, 0x61, 0x5d, 0xbb, 0x33, 0x6c, 0xa7, 0x5b, 0xa5, 0x51, 0x25, 0x54, 0xde, 0x05, 0xbb, 0x48, 0x69, 0x08, 0x81, 0x90, 0xc7, 0x70, 0x9a, 0x8c, 0x39, 0x3b, 0x1a, 0x69, 0x46, 0xb6, 0x24, 0x4b, 0xf3, 0xff, 0x3e, 0x30, 0xb6, 0x75, 0x66, 0x46, 0xf2, 0xa2, 0xf3, 0xd3, 0x68, 0x66, 0xec, 0x3d, 0x29, 0x95, 0xaf, 0xd3, 0xf9, 0x73, 0x01, 0x46, 0xed, 0xe7, 0xf3, 0xd3, 0x93, 0xe6, 0xeb, 0x97, 0x2f, 0xbe, 0x4e, 0xe7, 0xcf, 0xf3, 0xd9, 0xe2, 0x7d, 0x1e, 0x11, 0xb0, 0x37, 0x4f, 0x0f, 0x8f, 0xe5, 0xfb, 0xe2, 0xdb, 0x49, 0x29, 0xff, 0x0f, 0x80, 0xc9, 0x0f, 0x59, 0x9a, 0x08, 0x9c, 0x98, 0xfc, 0x90, 0xe9, 0xe9, 0xe1, 0xb1, 0x7c, 0x78, 0xef, 0x07, 0x01, 0xbc, 0x1f, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x3e, 0x0e, 0x59, 0x68, 0x31, 0xbd, 0xdf, 0xf5, 0xe3, 0x00, 0xb6, 0x6c, 0x72, 0x7e, 0xd1, 0xbb, 0x4c, 0x6f, 0x00, 0x16, 0xd3, 0xfb, 0xf2, 0xe9, 0xf3, 0x97, 0xad, 0x3c, 0x20, 0x60, 0x7f, 0x4e, 0xcf, 0x26, 0xa5, 0xef, 0x00, 0xff, 0xda, 0xb7, 0x00, 0x26, 0x3f, 0x1c, 0xaf, 0xbb, 0x9b, 0xdb, 0x72, 0x7a, 0x36, 0x59, 0xbb, 0x8c, 0x63, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0xd6, 0x06, 0x60, 0x72, 0x7e, 0x51, 0x2e, 0xaf, 0xaf, 0xf6, 0xf5, 0x58, 0x80, 0x2d, 0xba, 0xbc, 0xbe, 0x2a, 0xf3, 0xd9, 0x62, 0xed, 0x32, 0x1f, 0xfb, 0x36, 0x32, 0x9f, 0x2d, 0x44, 0x00, 0x8e, 0x50, 0xdf, 0xe4, 0x2f, 0x65, 0x40, 0x00, 0x86, 0x6e, 0x08, 0x38, 0x3e, 0x8e, 0x01, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x1b, 0x74, 0x16, 0x60, 0x31, 0xbd, 0xdf, 0xf5, 0xe3, 0x00, 0xb6, 0x6c, 0x72, 0x7e, 0xd1, 0xbb, 0x4c, 0x6f, 0x00, 0x16, 0xd3, 0xfb, 0xf2, 0xe9, 0xf3, 0x97, 0xad, 0x3c, 0x20, 0x60, 0x7f, 0x4e, 0xcf, 0x26, 0xbd, 0xa7, 0xf0, 0xd7, 0xbe, 0x05, 0x30, 0xf9, 0xe1, 0x78, 0xdd, 0xdd, 0xdc, 0x96, 0xd3, 0xb3, 0xc9, 0xda, 0x65, 0x1c, 0x03, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0x30, 0x01, 0x80, 0x60, 0x02, 0x00, 0xc1, 0x04, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0xb6, 0x36, 0x00, 0x93, 0xf3, 0x8b, 0x72, 0x79, 0x7d, 0xb5, 0xaf, 0xc7, 0x02, 0x6c, 0xd1, 0xe5, 0xf5, 0x55, 0x99, 0xcf, 0x16, 0x6b, 0x97, 0xf9, 0xd8, 0xb7, 0x91, 0xf9, 0x6c, 0x21, 0x02, 0x70, 0x84, 0xfa, 0x26, 0x7f, 0x29, 0x03, 0x02, 0x30, 0x74, 0x43, 0xc0, 0xf1, 0x71, 0x0c, 0x00, 0x82, 0x09, 0x00, 0x04, 0x13, 0x00, 0x08, 0x26, 0x00, 0x10, 0x4c, 0x00, 0x20, 0x98, 0x00, 0x40, 0xb0, 0x41, 0xa7, 0x01, 0x39, 0x1e, 0x8b, 0xe9, 0xfd, 0x4e, 0xb7, 0x3f, 0x39, 0xbf, 0xd8, 0xe9, 0xf6, 0xd9, 0x2f, 0x01, 0x18, 0x91, 0xa7, 0x87, 0xc7, 0xf2, 0xe9, 0xf3, 0x97, 0x9d, 0xdf, 0xc7, 0xef, 0xff, 0xf8, 0x87, 0x9d, 0xde, 0x07, 0xfb, 0xb3, 0x71, 0x00, 0x4e, 0xcf, 0x26, 0x1b, 0x2d, 0xef, 0x22, 0x22, 0x38, 0x5c, 0x1b, 0x05, 0xa0, 0x99, 0xfc, 0x77, 0x37, 0xb7, 0x83, 0x96, 0xbf, 0xbc, 0xbe, 0x2a, 0xa7, 0x67, 0x93, 0xde, 0x08, 0xd4, 0x51, 0x99, 0xcf, 0x16, 0x4b, 0xdf, 0xd7, 0xcb, 0x74, 0x6d, 0x6b, 0xd3, 0xe5, 0x37, 0xb5, 0xee, 0x67, 0x68, 0x8f, 0x0d, 0xf9, 0x79, 0x77, 0x65, 0x5f, 0x97, 0x6c, 0x8b, 0xfa, 0x78, 0x6c, 0xbc, 0x07, 0x70, 0x77, 0x73, 0x3b, 0x78, 0x17, 0xf0, 0xee, 0xe6, 0x76, 0xd0, 0x93, 0xb2, 0x59, 0xae, 0xd9, 0x76, 0xfb, 0xfb, 0xd3, 0xb3, 0xc9, 0x4b, 0x74, 0x56, 0xdd, 0x77, 0x7d, 0x5f, 0xcd, 0xf2, 0xab, 0xee, 0xbb, 0x8e, 0x43, 0x3b, 0x14, 0xcd, 0x04, 0xae, 0x3f, 0xaf, 0xba, 0x7d, 0x3e, 0x5b, 0xfc, 0xf0, 0xef, 0xd1, 0xde, 0x3b, 0xea, 0xda, 0x76, 0xfd, 0xfd, 0xb6, 0x99, 0x98, 0x6c, 0x6a, 0xe3, 0xb3, 0x00, 0x9b, 0xbc, 0xff, 0x1b, 0xba, 0x6c, 0xb3, 0xdc, 0xaa, 0xcf, 0xcd, 0xd7, 0xf5, 0x44, 0x6b, 0x3e, 0x1a, 0xed, 0xc9, 0xbe, 0xea, 0xbe, 0x9b, 0x38, 0x34, 0x41, 0x69, 0x3e, 0xd7, 0xdb, 0xaa, 0x83, 0xd3, 0xfe, 0xdc, 0x1e, 0x6f, 0x47, 0xa7, 0x6b, 0x99, 0xae, 0xef, 0xe1, 0x10, 0xbc, 0xea, 0x20, 0xe0, 0x90, 0xe3, 0x00, 0xed, 0xdd, 0xe2, 0xf6, 0x6d, 0x6f, 0x51, 0x4f, 0xa4, 0x66, 0xa2, 0x0f, 0xdd, 0xdb, 0xa8, 0xd7, 0x29, 0xa5, 0xbc, 0xec, 0x69, 0xd4, 0xeb, 0xd6, 0xaf, 0xea, 0x5d, 0x31, 0x5a, 0xb5, 0x17, 0xd4, 0xbe, 0xad, 0xfd, 0x78, 0xea, 0xbd, 0x9a, 0x5d, 0xd8, 0xf5, 0x19, 0x80, 0x86, 0x33, 0x01, 0xe3, 0xf1, 0xa6, 0xb3, 0x00, 0x5d, 0x13, 0xa7, 0x6b, 0x12, 0x6e, 0x32, 0x39, 0x4b, 0x29, 0x4b, 0xbb, 0xdd, 0xf5, 0xfa, 0xf3, 0xd9, 0xa2, 0x73, 0x02, 0xb5, 0xb7, 0xdf, 0xbc, 0xe2, 0xb6, 0x97, 0x6d, 0x6f, 0xab, 0x6b, 0xdd, 0x55, 0xeb, 0x34, 0xcb, 0x77, 0xdd, 0x7f, 0xd7, 0xbf, 0x45, 0xbd, 0xed, 0x66, 0x7c, 0xc8, 0xef, 0x68, 0xbf, 0xc6, 0x3e, 0xce, 0x00, 0xd4, 0xf7, 0xe5, 0x4c, 0xc0, 0x38, 0x9c, 0x7c, 0x9d, 0xce, 0x9f, 0x87, 0x3e, 0x21, 0xdb, 0xef, 0x8b, 0xbb, 0x0e, 0xd8, 0xf5, 0xbd, 0xa7, 0x5e, 0xa5, 0x7e, 0x52, 0x3d, 0x3d, 0x3c, 0xbe, 0xdc, 0xde, 0xbe, 0xad, 0xeb, 0x89, 0xd7, 0x5e, 0x7e, 0xdd, 0xb2, 0xed, 0x6d, 0x75, 0xad, 0xbb, 0xea, 0x3e, 0xba, 0xc6, 0xbb, 0x1e, 0x77, 0xfd, 0x56, 0xa5, 0x8e, 0x50, 0xdf, 0xe3, 0x7a, 0x8b, 0xfa, 0xe7, 0xd8, 0x07, 0x01, 0x38, 0x7e, 0x4f, 0x0f, 0x8f, 0x87, 0x73, 0x1d, 0x40, 0xfb, 0xfd, 0xfe, 0xba, 0xf1, 0xbe, 0xb1, 0xbe, 0x27, 0xe7, 0xba, 0xfb, 0x5a, 0xb5, 0x6e, 0xd7, 0x5b, 0x81, 0xbe, 0x6d, 0xb5, 0xf7, 0x40, 0x76, 0x39, 0x69, 0xf6, 0xfd, 0x47, 0x5b, 0x1c, 0x70, 0x1c, 0x87, 0x37, 0x05, 0x60, 0xd5, 0x51, 0xef, 0xda, 0xa6, 0xbb, 0xff, 0x63, 0xb2, 0xcf, 0x57, 0x49, 0x13, 0x92, 0xd7, 0x78, 0x55, 0x00, 0x86, 0x1c, 0xcd, 0x6e, 0x9e, 0xfc, 0xf5, 0x7b, 0x60, 0xbb, 0x8d, 0x70, 0x58, 0x5e, 0x15, 0x80, 0xfa, 0x3d, 0xee, 0x90, 0x8b, 0x60, 0x86, 0x4c, 0xfe, 0x7a, 0xdd, 0x55, 0xdb, 0xd9, 0xf5, 0x05, 0x3f, 0xc7, 0x6c, 0x5f, 0x67, 0x00, 0x1a, 0xce, 0x04, 0x8c, 0xc3, 0xd6, 0xcf, 0x02, 0x6c, 0xfa, 0xfe, 0xbd, 0x94, 0xee, 0xa3, 0xfe, 0xed, 0x8b, 0x6b, 0x9a, 0x09, 0x3e, 0xf4, 0x82, 0x9f, 0x55, 0x07, 0x23, 0xeb, 0xb1, 0x7a, 0xfc, 0x98, 0xed, 0xf3, 0x0c, 0x40, 0x7d, 0x9f, 0xf6, 0xe8, 0x8e, 0xdf, 0x9b, 0x02, 0xd0, 0x5c, 0xb5, 0x57, 0x7f, 0xff, 0x1a, 0xed, 0x2b, 0xff, 0x9a, 0x53, 0x65, 0x5d, 0x57, 0x00, 0x0e, 0xb9, 0xe0, 0xa7, 0xbd, 0x5e, 0x1d, 0x8a, 0x31, 0x4c, 0x78, 0xd8, 0x96, 0x37, 0x9f, 0x05, 0x58, 0x37, 0xe9, 0x87, 0xfe, 0xe2, 0xd0, 0xaa, 0x23, 0xec, 0x5d, 0xb7, 0x0d, 0x3d, 0xa8, 0xd8, 0x77, 0x74, 0x7f, 0x4c, 0x07, 0x27, 0xeb, 0xe3, 0x2c, 0xfb, 0xbc, 0x3f, 0x31, 0x3d, 0x7e, 0x1b, 0x5f, 0x0a, 0xdc, 0x75, 0xe4, 0xbf, 0x7d, 0x59, 0x6e, 0xbd, 0xdc, 0xd0, 0x27, 0xe5, 0xdd, 0xcd, 0xed, 0xd2, 0x36, 0x9a, 0xdb, 0xd6, 0x5d, 0xf2, 0x5b, 0xbf, 0xda, 0xd7, 0xf7, 0x59, 0xaf, 0xd7, 0x7c, 0xbf, 0x8d, 0xbd, 0x95, 0x43, 0x34, 0x9f, 0x2d, 0x5e, 0xfe, 0xff, 0x86, 0x5d, 0x46, 0xad, 0xbe, 0x90, 0xc9, 0xe4, 0x1f, 0x87, 0x8d, 0x2e, 0x04, 0x7a, 0x7a, 0x78, 0xdc, 0xe8, 0x4a, 0xbf, 0x4d, 0x8f, 0xfc, 0x77, 0x5d, 0x6c, 0xd3, 0x77, 0xd1, 0x4e, 0xf3, 0x75, 0xbd, 0x7e, 0x7b, 0xbd, 0xae, 0xb1, 0x75, 0x17, 0xfd, 0x1c, 0xbb, 0x4d, 0xe3, 0xdb, 0xc7, 0x2b, 0xfe, 0x38, 0x3d, 0x3d, 0x3c, 0x6e, 0x16, 0x80, 0x66, 0xa5, 0xb6, 0xf6, 0xe4, 0xac, 0x6f, 0xe7, 0xfd, 0xbc, 0x35, 0x04, 0x26, 0xfe, 0xb8, 0xbd, 0x2a, 0x00, 0x1c, 0x9f, 0xd7, 0xfc, 0x1d, 0x87, 0x52, 0x4c, 0xfc, 0xb1, 0x3b, 0xa8, 0x4b, 0x81, 0xd9, 0x9d, 0xf6, 0x2f, 0x3d, 0xad, 0x0a, 0x81, 0x89, 0x9f, 0x47, 0x00, 0x82, 0xac, 0x0a, 0x81, 0x89, 0x9f, 0x4b, 0x00, 0x02, 0xb5, 0x43, 0x60, 0xe2, 0xe7, 0x12, 0x80, 0x60, 0x26, 0x3e, 0xfe, 0x63, 0x10, 0x08, 0x26, 0x00, 0x10, 0xec, 0x43, 0x29, 0xfb, 0xff, 0x6b, 0x32, 0xc0, 0xfb, 0x6a, 0xe6, 0xfc, 0x87, 0xf6, 0x0d, 0xc0, 0xb8, 0xd5, 0x73, 0xfd, 0xc3, 0xaa, 0x01, 0x60, 0x7c, 0xda, 0x73, 0x7c, 0xe9, 0x2c, 0x80, 0x08, 0x40, 0x8e, 0xce, 0x83, 0x80, 0xff, 0xfd, 0xd7, 0x3f, 0x7f, 0xf8, 0xf8, 0xed, 0x97, 0x9f, 0x4a, 0x29, 0x65, 0xe9, 0xb3, 0x71, 0xe3, 0xc6, 0x0f, 0x7f, 0xbc, 0xf9, 0xe8, 0xb2, 0x14, 0x80, 0xae, 0x05, 0xff, 0xfc, 0xf7, 0x7f, 0x97, 0xdf, 0x7e, 0xf9, 0x69, 0xe9, 0xb3, 0x71, 0xe3, 0xc6, 0x0f, 0x7f, 0xbc, 0xd1, 0x35, 0xb7, 0x4f, 0xbe, 0x4e, 0xe7, 0xcf, 0xf5, 0xb5, 0xe1, 0x5d, 0x0b, 0xfd, 0xe7, 0xd7, 0xbf, 0x2c, 0xdd, 0x56, 0x4a, 0x29, 0xbf, 0xfb, 0xeb, 0x3f, 0x8c, 0x1b, 0x37, 0x7e, 0xe0, 0xe3, 0xb5, 0xf6, 0x9f, 0x8e, 0x73, 0x1d, 0x00, 0x04, 0x13, 0x00, 0x08, 0x36, 0x28, 0x00, 0x5d, 0xbb, 0x12, 0xf5, 0x6d, 0xc6, 0x8d, 0x1b, 0x3f, 0xdc, 0xf1, 0x75, 0x06, 0x1d, 0x03, 0x00, 0xc6, 0x61, 0xe9, 0x18, 0xc0, 0xcf, 0xe7, 0xa7, 0x27, 0xef, 0xf4, 0x58, 0x80, 0x77, 0xf4, 0x7d, 0xf1, 0xed, 0xe4, 0x65, 0xf2, 0xff, 0x69, 0xf2, 0xb7, 0xe7, 0xe6, 0x6b, 0x7b, 0x01, 0x30, 0x3e, 0xf5, 0xab, 0xff, 0xf7, 0xc5, 0xb7, 0x93, 0x52, 0x4a, 0xf9, 0xe1, 0xd5, 0xbf, 0x8e, 0x00, 0x30, 0x4e, 0xcd, 0xe4, 0x2f, 0xa5, 0x94, 0xff, 0x01, 0xcf, 0x06, 0xa1, 0xfb, 0xb3, 0x9d, 0xe2, 0x96, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };

    png_buffer file = { Cover, sizeof Cover };
    png_img png = png_read(file);

    png_buffer enc;
    {
        // encode size
        enum {Size = sizeof cart.size};
        enc.size = cart.size + Size;
        enc.data = malloc(enc.size);
        memcpy(enc.data, &cart.size, Size);
        memcpy(enc.data + Size, cart.data, cart.size);
    }

    for (s32 i = 0, end = (enc.size * 8 + bits - 1) / bits; i < end; i++)
        bitcpy(png.data, i << 3, enc.data, i * bits, bits);

    free(enc.data);

    png_buffer out = png_write(png);
    free(png.data);

    return out;
}

static png_buffer decodeCart(s32 bits, png_buffer file)
{
    png_img png = png_read(file);
    s32 pngSize = png.width * png.height * sizeof(u32);
    s32 encSize = pngSize * bits / 8;
    png_buffer enc = { malloc(encSize), encSize };

    for (s32 i = 0; i < pngSize; i++)
        bitcpy(enc.data, i * bits, png.data, i << 3, bits);

    png_buffer out;

    memcpy(&out.size, enc.data, sizeof out.size);

    if(out.size)
    {
        out.data = malloc(out.size);
        memcpy(out.data, enc.data + sizeof out.size, out.size);
    }

    free(enc.data);
    free(png.data);

    return out;
}

Studio* studioInit(s32 argc, const char **argv, s32 samplerate, const char* folder)
{
    for(s32 bits = 1; bits <= 8; bits++)
    {
        s32 size = (256 * 256 * bits - 8) * sizeof(u32) / 8;

        png_buffer buf = {malloc(size), size };

        for (s32 i = 0; i < size; i++)
            buf.data[i] = rand();

        png_buffer img = encodeCart(bits, buf);

        {
            png_buffer out = decodeCart(bits, img);

            if (out.size == buf.size && memcmp(out.data, buf.data, buf.size) == 0)
                printf("bits %i - OK, size %i\n", bits, size);
            else printf("bits %i - ERROR\n", bits);

            free(out.data);
        }

        free(img.data);
        free(buf.data);
    }

    setbuf(stdout, NULL);

    StartArgs args = parseArgs(argc, argv);

    impl.samplerate = samplerate;
    impl.net = tic_net_create(TIC_WEBSITE);

    {
        const char *path = args.fs ? args.fs : folder;

        if(fs_exists(path))
            impl.fs = tic_fs_create(path, impl.net);
        else
        {
            fprintf(stderr, "error: folder `%s` doesn't exist\n", path);
            exit(1);
        }
    }

    impl.tic80local = (tic80_local*)tic80_create(impl.samplerate);
    impl.studio.tic = impl.tic80local->memory;

    {
        for(s32 i = 0; i < TIC_EDITOR_BANKS; i++)
        {
            impl.banks.sprite[i]   = calloc(1, sizeof(Sprite));
            impl.banks.map[i]      = calloc(1, sizeof(Map));
            impl.banks.sfx[i]      = calloc(1, sizeof(Sfx));
            impl.banks.music[i]    = calloc(1, sizeof(Music));
        }

        impl.code       = calloc(1, sizeof(Code));
        impl.start      = calloc(1, sizeof(Start));
        impl.console    = calloc(1, sizeof(Console));
        impl.run        = calloc(1, sizeof(Run));
        impl.world      = calloc(1, sizeof(World));
        impl.config     = calloc(1, sizeof(Config));
        impl.dialog     = calloc(1, sizeof(Dialog));
        impl.menu       = calloc(1, sizeof(Menu));
        impl.surf       = calloc(1, sizeof(Surf));
    }

    tic_fs_makedir(impl.fs, TIC_LOCAL);
    tic_fs_makedir(impl.fs, TIC_LOCAL_VERSION);
    
    initConfig(impl.config, impl.studio.tic, impl.fs);
    initKeymap();
    initStart(impl.start, impl.studio.tic);
    initConsole(impl.console, impl.studio.tic, impl.fs, impl.net, impl.config, args);
    initSurfMode();
    initRunMode();
    initModules();

    if(args.scale)
        impl.config->data.uiScale = args.scale;

#if defined(CRT_SHADER_SUPPORT)
    impl.config->data.crtMonitor = args.crt;
#endif

    impl.config->data.goFullscreen = args.fullscreen;
    impl.config->data.noSound = args.nosound;

    impl.studio.tick = studioTick;
    impl.studio.close = studioClose;
    impl.studio.updateProject = updateStudioProject;
    impl.studio.exit = exitStudio;
    impl.studio.config = getConfig;

    if(args.skip)
        setStudioMode(TIC_CONSOLE_MODE);

    return &impl.studio;
}

bool hasProjectExt(const char* name)
{
    return tic_tool_has_ext(name, PROJECT_LUA_EXT)
        || tic_tool_has_ext(name, PROJECT_MOON_EXT)
        || tic_tool_has_ext(name, PROJECT_JS_EXT)
        || tic_tool_has_ext(name, PROJECT_WREN_EXT)
        || tic_tool_has_ext(name, PROJECT_SQUIRREL_EXT)
        || tic_tool_has_ext(name, PROJECT_FENNEL_EXT);
}
