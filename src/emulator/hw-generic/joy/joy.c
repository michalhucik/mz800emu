/* 
 * File:   joy.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 1. února 2018, 9:20
 * 
 * 
 * ----------------------------- License -------------------------------------
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 * ---------------------------------------------------------------------------
 */

#include "main.h"
#include <stdlib.h>

#include "mzarch/mzarch.h"
#include "joy.h"
#include "cfgmain.h"

#include "iface/iface_joy.h"


st_JOY g_joy;

static void joy_propagatecfg_type ( void *e, void *data ) {
    st_JOY_DEV *joydev = (st_JOY_DEV *) data;
    joydev->type = cfgelement_get_keyword_value ( (CFGELM *) e );
    if ( joydev->type == JOY_TYPE_JOYSTICK ) {
        if ( EXIT_SUCCESS != iface_joy_open_configured_joyid ( joydev->id ) ) {
            g_warning("Joystick device JOY%d is not connected!", joydev->id);
            joydev->type = JOY_TYPE_NONE;
        };
    };
}


static void joy_type_config ( CFGMOD *cmod, en_JOY_DEVID joy_devid ) {
    CFGELM *elm;
    char element_name[] = "joyX_type";
    element_name[3] = '1' + joy_devid;
    elm = cfgmodule_register_new_element ( cmod, element_name, CFGENTYPE_KEYWORD, JOY_TYPE_NONE,
                                           JOY_TYPE_NONE, "NONE",
                                           JOY_TYPE_NUM_KEYPAD, "NUM_KEYPAD",
                                           JOY_TYPE_JOYSTICK, "JOYSTICK",
                                           -1 );

    cfgelement_set_propagate_cb ( elm, joy_propagatecfg_type, (void*) &g_joy.dev[joy_devid] );
    cfgelement_set_handlers ( elm, NULL, (void*) &g_joy.dev[joy_devid].type );
}


void joy_init ( void ) {
    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "JOY" );
    en_JOY_DEVID joy_devid;
    for ( joy_devid = 0; joy_devid < JOY_DEVID_COUNT; joy_devid++ ) {
        g_joy.dev[joy_devid].id = joy_devid;
        joy_type_config ( cmod, joy_devid );
        joy_reset_dev_state ( joy_devid );
    };
    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );
}


uint8_t joy_read_byte ( en_JOY_DEVID joy_devid ) {

    if ( g_joy.dev[joy_devid].type == JOY_TYPE_NONE ) {
        return 0xff;
    } else if ( g_joy.dev[joy_devid].type == JOY_TYPE_JOYSTICK ) {
        return iface_joy_scan ( joy_devid );
    };

    return g_joy.dev[joy_devid].state;
}
