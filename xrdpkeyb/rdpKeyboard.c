/*
Copyright 2013-2017 Jay Sorg

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

xrdp keyboard module

*/

#if defined(HAVE_CONFIG_H)
#include "config_ac.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* this should be before all X11 .h files */
#include <xorg-server.h>
#include <xorgVersion.h>

/* all driver need this */
#include <xf86.h>
#include <xf86_OSproc.h>

#include <xf86Xinput.h>

#include <mipointer.h>
#include <fb.h>
#include <micmap.h>
#include <mi.h>

#include <xkbsrv.h>

#include <X11/keysym.h>

#include "rdp.h"
#include "rdpInput.h"
#include "rdpDraw.h"
#include "rdpMisc.h"

/******************************************************************************/
#define LOG_LEVEL 1
#define LLOGLN(_level, _args) \
    do { if (_level < LOG_LEVEL) { ErrorF _args ; ErrorF("\n"); } } while (0)

#define MIN_KEY_CODE 8
#define MAX_KEY_CODE 255

static char g_evdev_str[] = "evdev";
static char g_pc104_str[] = "pc104";
static char g_us_str[] = "us";
static char g_empty_str[] = "";
static char g_Keyboard_str[] = "Keyboard";

static char g_xrdp_keyb_name[] = XRDP_KEYB_NAME;

static int
rdpLoadLayout(rdpKeyboard *keyboard, struct xrdp_client_info *client_info);

/******************************************************************************/
static void
rdpEnqueueKey(DeviceIntPtr device, int type, int scancode)
{
    if (type == KeyPress)
    {
        xf86PostKeyboardEvent(device, scancode, TRUE);
    }
    else
    {
        xf86PostKeyboardEvent(device, scancode, FALSE);
    }
}

/******************************************************************************/
static void
sendDownUpKeyEvent(DeviceIntPtr device, int type, int x_scancode)
{
    /* need this cause rdp and X11 repeats are different */
    /* if type is keydown, send keyup + keydown */
    if (type == KeyPress)
    {
        rdpEnqueueKey(device, KeyRelease, x_scancode);
        rdpEnqueueKey(device, KeyPress, x_scancode);
    }
    else
    {
        rdpEnqueueKey(device, KeyRelease, x_scancode);
    }
}

/******************************************************************************/
static void
check_keysa(rdpKeyboard *keyboard)
{
    if (keyboard->ctrl_down != 0)
    {
        rdpEnqueueKey(keyboard->device, KeyRelease, keyboard->ctrl_down);
        keyboard->ctrl_down = 0;
    }

    if (keyboard->alt_down != 0)
    {
        rdpEnqueueKey(keyboard->device, KeyRelease, keyboard->alt_down);
        keyboard->alt_down = 0;
    }

    if (keyboard->shift_down != 0)
    {
        rdpEnqueueKey(keyboard->device, KeyRelease, keyboard->shift_down);
        keyboard->shift_down = 0;
    }
}

/**
 * @param down   - true for KeyDown events, false otherwise
 * @param param1 - ASCII code of pressed key
 * @param param2 -
 * @param param3 - scancode of pressed key
 * @param param4 -
 ******************************************************************************/
static void
KbdAddEvent(rdpKeyboard *keyboard, int down, int param1, int param2,
            int param3, int param4)
{
    int rdp_scancode;
    int x_scancode;
    int is_ext;
    int is_spe;
    int type;

    type = down ? KeyPress : KeyRelease;
    rdp_scancode = param3;
    is_ext = param4 & 256; /* 0x100 */
    is_spe = param4 & 512; /* 0x200 */
    x_scancode = param1;

    switch (rdp_scancode)
    {
        case 42: /* left shift              */
        case 54: /* right shift             */
        case 56: /* left - right alt button */
        case 58: /* caps lock               */
        case 70: /* scroll lock             */
        case 89: /* left meta               */
        case 90: /* right meta              */
        case 91: /* left win key            */
        case 92: /* right win key           */
        case 93: /* menu key                */
            rdpEnqueueKey(keyboard->device, type, x_scancode);
            break;

        case 15: /* tab */

            if (!down && !keyboard->tab_down)
            {
                /* leave x_scancode 0 here, we don't want the tab key up */
                check_keysa(keyboard);
            }
            else
            {
                sendDownUpKeyEvent(keyboard->device, type, 23);
            }

            keyboard->tab_down = down;
            break;

        case 29: /* left or right ctrl */

            /* this is to handle special case with pause key sending control first */
            if (is_spe)
            {
                if (down)
                {
                    keyboard->pause_spe = 1;
                    /* leave x_scancode 0 here, we don't want the control key down */
                }
            }
            else
            {
                x_scancode = is_ext ? 105 : 37;
                keyboard->ctrl_down = down ? x_scancode : 0;
                rdpEnqueueKey(keyboard->device, type, x_scancode);
            }

            break;

        case 69: /* Pause or Num Lock */

            if (keyboard->pause_spe)
            {
                x_scancode = 127;

                if (!down)
                {
                    keyboard->pause_spe = 0;
                }
            }
            else
            {
                x_scancode = keyboard->ctrl_down ? 127 : 77;
            }

            sendDownUpKeyEvent(keyboard->device, type, x_scancode);
            break;

        default:
            sendDownUpKeyEvent(keyboard->device, type, x_scancode);
            break;
    }
}

/******************************************************************************/
/* notes -
     scroll lock doesn't seem to be a modifier in X
*/
static void
KbdSync(rdpKeyboard *keyboard, int param1)
{
    int xkb_state;

    xkb_state = XkbStateFieldFromRec(&(keyboard->device->key->xkbInfo->state));

    if ((!(xkb_state & 0x02)) != (!(param1 & 4))) /* caps lock */
    {
        LLOGLN(0, ("KbdSync: toggling caps lock"));
        KbdAddEvent(keyboard, 1, 58, 0, 58, 0);
        KbdAddEvent(keyboard, 0, 58, 49152, 58, 49152);
    }

    if ((!(xkb_state & 0x10)) != (!(param1 & 2))) /* num lock */
    {
        LLOGLN(0, ("KbdSync: toggling num lock"));
        KbdAddEvent(keyboard, 1, 69, 0, 69, 0);
        KbdAddEvent(keyboard, 0, 69, 49152, 69, 49152);
    }

    if ((!(keyboard->scroll_lock_down)) != (!(param1 & 1))) /* scroll lock */
    {
        LLOGLN(0, ("KbdSync: toggling scroll lock"));
        KbdAddEvent(keyboard, 1, 70, 0, 70, 0);
        KbdAddEvent(keyboard, 0, 70, 49152, 70, 49152);
    }
}

/******************************************************************************/
static int
rdpInputKeyboard(rdpPtr dev, int msg, long param1, long param2,
                 long param3, long param4)
{
    rdpKeyboard *keyboard;

    keyboard = &(dev->keyboard);
    LLOGLN(10, ("rdpInputKeyboard:"));
    switch (msg)
    {
        case 15: /* key down */
        case 16: /* key up */
            KbdAddEvent(keyboard, msg == 15, param1, param2, param3, param4);
            break;
        case 17: /* from RDP_INPUT_SYNCHRONIZE */
            KbdSync(keyboard, param1);
            break;
        case 18:
            rdpLoadLayout(keyboard, (struct xrdp_client_info *) param1);
            break;

    }
    return 0;
}

/******************************************************************************/
static void
rdpkeybDeviceOn(void)
{
    LLOGLN(0, ("rdpkeybDeviceOn:"));
}

/******************************************************************************/
static void
rdpkeybDeviceOff(void)
{
    LLOGLN(0, ("rdpkeybDeviceOff:"));
}

/******************************************************************************/
static void
rdpkeybBell(int volume, DeviceIntPtr pDev, pointer ctrl, int cls)
{
    LLOGLN(0, ("rdpkeybBell:"));
}

/******************************************************************************/
static CARD32
rdpInDeferredRepeatCallback(OsTimerPtr timer, CARD32 now, pointer arg)
{
    DeviceIntPtr pDev;
    DeviceIntPtr it;
    Bool found;

    LLOGLN(0, ("rdpInDeferredRepeatCallback:"));
    TimerFree(timer);
    pDev = (DeviceIntPtr) arg;
    found = FALSE;
    it = inputInfo.devices;
    while (it != NULL)
    {
        if (it == pDev)
        {
            found = TRUE;
            break;
        }
        it = it->next;
    }
    if (found)
    {
        XkbSetRepeatKeys(pDev, -1, AutoRepeatModeOff);
    }
    return 0;
}

/******************************************************************************/
static void
rdpkeybChangeKeyboardControl(DeviceIntPtr pDev, KeybdCtrl *ctrl)
{
    XkbControlsPtr ctrls;

    LLOGLN(0, ("rdpkeybChangeKeyboardControl:"));
    ctrls = 0;
    if (pDev != 0)
    {
        if (pDev->key != 0)
        {
            if (pDev->key->xkbInfo != 0)
            {
                if (pDev->key->xkbInfo->desc != 0)
                {
                    if (pDev->key->xkbInfo->desc->ctrls != 0)
                    {
                        ctrls = pDev->key->xkbInfo->desc->ctrls;
                    }
                }
            }
        }
    }
    if (ctrls != 0)
    {
        if (ctrls->enabled_ctrls & XkbRepeatKeysMask)
        {
            LLOGLN(0, ("rdpkeybChangeKeyboardControl: autoRepeat on"));
            /* schedule to turn off the autorepeat after 100 ms so any app
             * polling it will be happy it's on */
            TimerSet(NULL, 0, 100, rdpInDeferredRepeatCallback, pDev);
        }
        else
        {
            LLOGLN(0, ("rdpkeybChangeKeyboardControl: autoRepeat off"));
        }
    }
}

/******************************************************************************/
static int
rdpkeybControl(DeviceIntPtr device, int what)
{
    DevicePtr pDev;
    XkbRMLVOSet set;
    rdpPtr dev;

    LLOGLN(0, ("rdpkeybControl: what %d", what));
    pDev = (DevicePtr)device;

    switch (what)
    {
        case DEVICE_INIT:
            memset(&set, 0, sizeof(set));
            set.rules = g_evdev_str;
            set.model = g_pc104_str;
            set.layout = g_us_str;
            set.variant = g_empty_str;
            set.options = g_empty_str;
            InitKeyboardDeviceStruct(device, &set, rdpkeybBell,
                                     rdpkeybChangeKeyboardControl);
            dev = rdpGetDevFromScreen(NULL);
            dev->keyboard.device = device;
            rdpRegisterInputCallback(0, rdpInputKeyboard);
            break;
        case DEVICE_ON:
            pDev->on = 1;
            rdpkeybDeviceOn();
            break;
        case DEVICE_OFF:
            pDev->on = 0;
            rdpkeybDeviceOff();
            break;
        case DEVICE_CLOSE:
            if (pDev->on)
            {
                rdpkeybDeviceOff();
            }
            break;
    }
    return Success;
}

#if XORG_VERSION_CURRENT < XORG_VERSION_NUMERIC(1, 9, 0, 1, 0)

/* debian 6
   ubuntu 10.04 */

/******************************************************************************/
static InputInfoPtr
rdpkeybPreInit(InputDriverPtr drv, IDevPtr dev, int flags)
{
    InputInfoPtr info;

    LLOGLN(0, ("rdpkeybPreInit: drv %p dev %p, flags 0x%x",
           drv, dev, flags));
    info = xf86AllocateInput(drv, 0);
    info->name = dev->identifier;
    info->device_control = rdpkeybControl;
    info->flags = XI86_CONFIGURED | XI86_ALWAYS_CORE | XI86_SEND_DRAG_EVENTS |
                  XI86_CORE_KEYBOARD | XI86_KEYBOARD_CAPABLE;
    info->type_name = "Keyboard";
    info->fd = -1;
    info->conf_idev = dev;

    return info;
}

#else

/* debian 7
   ubuntu 12.04 */

/******************************************************************************/
static int
rdpkeybPreInit(InputDriverPtr drv, InputInfoPtr info, int flags)
{
    LLOGLN(0, ("rdpkeybPreInit: drv %p info %p, flags 0x%x",
           drv, info, flags));
    info->device_control = rdpkeybControl;
    info->type_name = g_Keyboard_str;

    return 0;
}

#endif

/******************************************************************************/
static void
rdpkeybUnInit(InputDriverPtr drv, InputInfoPtr info, int flags)
{
    LLOGLN(0, ("rdpkeybUnInit: drv %p info %p, flags 0x%x",
           drv, info, flags));
    rdpUnregisterInputCallback(rdpInputKeyboard);
}

/******************************************************************************/
static InputDriverRec rdpkeyb =
{
    PACKAGE_VERSION_MAJOR,  /* version   */
    g_xrdp_keyb_name,       /* name      */
    NULL,                   /* identify  */
    rdpkeybPreInit,         /* preinit   */
    rdpkeybUnInit,          /* uninit    */
    NULL,                   /* module    */
    0                       /* ref count */
};

/******************************************************************************/
static pointer
rdpkeybPlug(pointer module, pointer options, int *errmaj, int *errmin)
{
    LLOGLN(0, ("rdpkeybPlug:"));
    xf86AddInputDriver(&rdpkeyb, module, 0);
    return module;
}

/******************************************************************************/
static void
rdpkeybUnplug(pointer p)
{
    LLOGLN(0, ("rdpkeybUnplug:"));
}

/******************************************************************************/
static int
reload_xkb(DeviceIntPtr keyboard, XkbRMLVOSet *set)
{
    XkbSrvInfoPtr xkbi;
    XkbDescPtr xkb;
    KeySymsPtr keySyms;
    KeyCode first_key;
    CARD8 num_keys;
    DeviceIntPtr pDev;

    /* free some stuff so we can call InitKeyboardDeviceStruct again */
    xkbi = keyboard->key->xkbInfo;
    xkb = xkbi->desc;
    XkbFreeKeyboard(xkb, 0, TRUE);
    free(xkbi);
    keyboard->key->xkbInfo = NULL;
    free(keyboard->kbdfeed);
    keyboard->kbdfeed = NULL;
    free(keyboard->key);
    keyboard->key = NULL;

    /* init keyboard and reload the map */
    if (!InitKeyboardDeviceStruct(keyboard, set, rdpkeybBell,
                                  rdpkeybChangeKeyboardControl))
    {
        LLOGLN(0, ("rdpLoadLayout: InitKeyboardDeviceStruct failed"));
        return 1;
    }

    /* notify the X11 clients eg. X_ChangeKeyboardMapping */
    keySyms = XkbGetCoreMap(keyboard);
    if (keySyms)
    {
        first_key = keySyms->minKeyCode;
        num_keys = (keySyms->maxKeyCode - keySyms->minKeyCode) + 1;
        XkbApplyMappingChange(keyboard, keySyms, first_key, num_keys,
                              NULL, serverClient);
        for (pDev = inputInfo.devices; pDev; pDev = pDev->next)
        {
            if ((pDev->coreEvents || pDev == keyboard) && pDev->key)
            {
                XkbApplyMappingChange(pDev, keySyms, first_key, num_keys,
                                      NULL, serverClient);
            }
        }
        free(keySyms->map);
        free(keySyms);
    }
    else
    {
        return 1;
    }
    return 0;
}

/******************************************************************************/
static int
rdpLoadLayout(rdpKeyboard *keyboard, struct xrdp_client_info *client_info)
{
    XkbRMLVOSet set;

    int keylayout = client_info->keylayout;

    LLOGLN(0, ("rdpLoadLayout: keylayout 0x%8.8x variant %s display %s",
               keylayout, client_info->variant, display));
    memset(&set, 0, sizeof(set));
    set.rules = g_evdev_str;

    set.model = g_pc104_str;
    set.layout = g_us_str;
    set.variant = g_empty_str;
    set.options = g_empty_str;

    if (strlen(client_info->model) > 0)
    {
        set.model = client_info->model;
    }
    if (strlen(client_info->variant) > 0)
    {
        set.variant = client_info->variant;
    }
    if (strlen(client_info->layout) > 0)
    {
        set.layout = client_info->layout;
    }
    if (strlen(client_info->options) > 0)
    {
        set.options = client_info->options;
    }

    reload_xkb(keyboard->device, &set);
    reload_xkb(inputInfo.keyboard, &set);

    return 0;
}

/******************************************************************************/
static XF86ModuleVersionInfo rdpkeybVersionRec =
{
    XRDP_KEYB_NAME,
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PACKAGE_VERSION_MAJOR,
    PACKAGE_VERSION_MINOR,
    PACKAGE_VERSION_PATCHLEVEL,
    ABI_CLASS_XINPUT,
    ABI_XINPUT_VERSION,
    MOD_CLASS_XINPUT,
    { 0, 0, 0, 0 }
};

/******************************************************************************/
_X_EXPORT XF86ModuleData xrdpkeybModuleData =
{
    &rdpkeybVersionRec,
    rdpkeybPlug,
    rdpkeybUnplug
};
