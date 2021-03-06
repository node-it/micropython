/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
 * Copyright (c) 2015 Daniel Campora
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "py/mpconfig.h"
#include MICROPY_HAL_H
#include "py/obj.h"
#include "py/runtime.h"
#include "py/objlist.h"
#include "py/stream.h"
#include "inc/hw_types.h"
#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_uart.h"
#include "rom_map.h"
#include "interrupt.h"
#include "prcm.h"
#include "uart.h"
#include "pybuart.h"
#include "pybioctl.h"
#include "pybsleep.h"
#include "mpcallback.h"
#include "mpexception.h"
#include "py/mpstate.h"
#include "osi.h"
#include "utils.h"
#include "pin.h"
#include "pybpin.h"
#include "pins.h"

/// \moduleref pyb
/// \class UART - duplex serial communication bus

/******************************************************************************
 DEFINE CONSTANTS
 *******-***********************************************************************/
#define PYBUART_FRAME_TIME_US(baud)             ((11 * 1000000) / baud)
#define PYBUART_2_FRAMES_TIME_US(baud)          (PYBUART_FRAME_TIME_US(baud) * 2)
#define PYBUART_RX_TIMEOUT_US(baud)             (PYBUART_2_FRAMES_TIME_US(baud))

#define PYBUART_TX_WAIT_US(baud)                ((PYBUART_FRAME_TIME_US(baud)) + 1)
#define PYBUART_TX_MAX_TIMEOUT_MS               (5)

#define PYBUART_RX_BUFFER_LEN                   (128)

// interrupt triggers
#define E_UART_TRIGGER_RX_ANY                   (0x01)
#define E_UART_TRIGGER_RX_HALF                  (0x02)
#define E_UART_TRIGGER_RX_FULL                  (0x04)
#define E_UART_TRIGGER_TX_DONE                  (0x08)

/******************************************************************************
 DECLARE PRIVATE FUNCTIONS
 ******************************************************************************/
STATIC void uart_init (pyb_uart_obj_t *self);
STATIC bool uart_rx_wait (pyb_uart_obj_t *self);
STATIC void uart_check_init(pyb_uart_obj_t *self);
STATIC void UARTGenericIntHandler(uint32_t uart_id);
STATIC void UART0IntHandler(void);
STATIC void UART1IntHandler(void);
STATIC void uart_callback_enable (mp_obj_t self_in);
STATIC void uart_callback_disable (mp_obj_t self_in);
STATIC mp_obj_t pyb_uart_deinit(mp_obj_t self_in);

/******************************************************************************
 DEFINE PRIVATE TYPES
 ******************************************************************************/
struct _pyb_uart_obj_t {
    mp_obj_base_t base;
    pyb_uart_id_t uart_id;
    uint reg;
    uint baudrate;
    uint config;
    uint flowcontrol;
    byte *read_buf;                     // read buffer pointer
    volatile uint16_t read_buf_head;    // indexes first empty slot
    uint16_t read_buf_tail;             // indexes first full slot (not full if equals head)
    byte peripheral;
    byte irq_trigger;
    bool callback_enabled;
};

/******************************************************************************
 DECLARE PRIVATE DATA
 ******************************************************************************/
STATIC pyb_uart_obj_t pyb_uart_obj[PYB_NUM_UARTS] = { {.reg = UARTA0_BASE, .baudrate = 0, .read_buf = NULL, .peripheral = PRCM_UARTA0},
                                                      {.reg = UARTA1_BASE, .baudrate = 0, .read_buf = NULL, .peripheral = PRCM_UARTA1} };
STATIC const mp_cb_methods_t uart_cb_methods;

STATIC const mp_obj_t pyb_uart_def_pin[PYB_NUM_UARTS][2] = { {&pin_GP1, &pin_GP2}, {&pin_GP3, &pin_GP4} };

/******************************************************************************
 DEFINE PUBLIC FUNCTIONS
 ******************************************************************************/
void uart_init0 (void) {
    // save references of the UART objects, to prevent the read buffers from being trashed by the gc
    MP_STATE_PORT(pyb_uart_objs)[0] = &pyb_uart_obj[0];
    MP_STATE_PORT(pyb_uart_objs)[1] = &pyb_uart_obj[1];
}

uint32_t uart_rx_any(pyb_uart_obj_t *self) {
    if (self->read_buf_tail != self->read_buf_head) {
        // buffering  via irq
        return (self->read_buf_head > self->read_buf_tail) ? self->read_buf_head - self->read_buf_tail :
                PYBUART_RX_BUFFER_LEN - self->read_buf_tail + self->read_buf_head;
    }
    return MAP_UARTCharsAvail(self->reg) ? 1 : 0;
}

int uart_rx_char(pyb_uart_obj_t *self) {
    if (self->read_buf_tail != self->read_buf_head) {
        // buffering via irq
        int data = self->read_buf[self->read_buf_tail];
        self->read_buf_tail = (self->read_buf_tail + 1) % PYBUART_RX_BUFFER_LEN;
        return data;
    } else {
        // no buffering
        return MAP_UARTCharGetNonBlocking(self->reg);
    }
}

bool uart_tx_char(pyb_uart_obj_t *self, int c) {
    uint32_t timeout = 0;
    while (!MAP_UARTCharPutNonBlocking(self->reg, c)) {
        if (timeout++ > ((PYBUART_TX_MAX_TIMEOUT_MS * 1000) / PYBUART_TX_WAIT_US(self->baudrate))) {
            return false;
        }
        UtilsDelay(UTILS_DELAY_US_TO_COUNT(PYBUART_TX_WAIT_US(self->baudrate)));
    }
    return true;
}

bool uart_tx_strn(pyb_uart_obj_t *self, const char *str, uint len) {
    for (const char *top = str + len; str < top; str++) {
        if (!uart_tx_char(self, *str)) {
            return false;
        }
    }
    return true;
}

void uart_tx_strn_cooked(pyb_uart_obj_t *self, const char *str, uint len) {
    for (const char *top = str + len; str < top; str++) {
        if (*str == '\n') {
            uart_tx_char(self, '\r');
        }
        uart_tx_char(self, *str);
    }
}

mp_obj_t uart_callback_new (pyb_uart_obj_t *self, mp_obj_t handler, mp_int_t priority, byte trigger) {
    // disable the uart interrupts before updating anything
    uart_callback_disable (self);

    if (self->uart_id == PYB_UART_0) {
        MAP_IntPrioritySet(INT_UARTA0, priority);
        MAP_UARTIntRegister(self->reg, UART0IntHandler);
    } else {
        MAP_IntPrioritySet(INT_UARTA1, priority);
        MAP_UARTIntRegister(self->reg, UART1IntHandler);
    }

    // create the callback
    mp_obj_t _callback = mpcallback_new ((mp_obj_t)self, handler, &uart_cb_methods, true);

    // enable the interrupts now
    self->irq_trigger = trigger;
    uart_callback_enable (self);

    return _callback;
}

/******************************************************************************
 DEFINE PRIVATE FUNCTIONS
 ******************************************************************************/
// assumes init parameters have been set up correctly
STATIC void uart_init (pyb_uart_obj_t *self) {
    // Enable the peripheral clock
    MAP_PRCMPeripheralClkEnable(self->peripheral, PRCM_RUN_MODE_CLK | PRCM_SLP_MODE_CLK);

    // Reset the uart
    MAP_PRCMPeripheralReset(self->peripheral);

    // re-allocate the read buffer after resetting the uart (which automatically disables any irqs)
    self->read_buf_head = 0;
    self->read_buf_tail = 0;
    self->read_buf = MP_OBJ_NULL; // free the read buffer before allocating again
    self->read_buf = m_new(byte, PYBUART_RX_BUFFER_LEN);

    // Initialize the UART
    MAP_UARTConfigSetExpClk(self->reg, MAP_PRCMPeripheralClockGet(self->peripheral),
                            self->baudrate, self->config);

    // Enable the FIFO
    MAP_UARTFIFOEnable(self->reg);

    // Configure the FIFO interrupt levels
    MAP_UARTFIFOLevelSet(self->reg, UART_FIFO_TX4_8, UART_FIFO_RX4_8);

    // Configure the flow control mode
    UARTFlowControlSet(self->reg, self->flowcontrol);
}

// Waits at most timeout microseconds for at least 1 char to become ready for
// reading (from buf or for direct reading).
// Returns true if something available, false if not.
STATIC bool uart_rx_wait (pyb_uart_obj_t *self) {
    int timeout = PYBUART_RX_TIMEOUT_US(self->baudrate);
    for ( ; ; ) {
        if (uart_rx_any(self)) {
            return true; // we have at least 1 char ready for reading
        }
        if (timeout > 0) {
            UtilsDelay(UTILS_DELAY_US_TO_COUNT(1));
            timeout--;
        }
        else {
            return false;
        }
    }
}

STATIC void UARTGenericIntHandler(uint32_t uart_id) {
    pyb_uart_obj_t *self;
    uint32_t status;
    bool exec_callback = false;

    self = &pyb_uart_obj[uart_id];
    status = MAP_UARTIntStatus(self->reg, true);
    // receive interrupt
    if (status & (UART_INT_RX | UART_INT_RT)) {
        MAP_UARTIntClear(self->reg, UART_INT_RX | UART_INT_RT);
        while (UARTCharsAvail(self->reg)) {
            int data = MAP_UARTCharGetNonBlocking(self->reg);
            if (pyb_stdio_uart == self && data == user_interrupt_char) {
                // raise an exception when interrupts are finished
                mpexception_keyboard_nlr_jump();
            }
            // there's always a read buffer available
            else {
                uint16_t next_head = (self->read_buf_head + 1) % PYBUART_RX_BUFFER_LEN;
                if (next_head != self->read_buf_tail) {
                    // only store data if room in buf
                    self->read_buf[self->read_buf_head] = data;
                    self->read_buf_head = next_head;
                }
            }
        }

        if (self->irq_trigger & E_UART_TRIGGER_RX_ANY) {
            exec_callback = true;
        }

        if (exec_callback && self->callback_enabled) {
            // call the user defined handler
            mp_obj_t _callback = mpcallback_find(self);
            mpcallback_handler(_callback);
        }
    }
}

STATIC void uart_check_init(pyb_uart_obj_t *self) {
    // not initialized
    if (!self->baudrate) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, mpexception_os_request_not_possible));
    }
}

STATIC void UART0IntHandler(void) {
    UARTGenericIntHandler(0);
}

STATIC void UART1IntHandler(void) {
    UARTGenericIntHandler(1);
}

STATIC void uart_callback_enable (mp_obj_t self_in) {
    pyb_uart_obj_t *self = self_in;
    // check for any of the rx interrupt types
    if (self->irq_trigger & (E_UART_TRIGGER_RX_ANY | E_UART_TRIGGER_RX_HALF | E_UART_TRIGGER_RX_FULL)) {
        MAP_UARTIntClear(self->reg, UART_INT_RX | UART_INT_RT);
        MAP_UARTIntEnable(self->reg, UART_INT_RX | UART_INT_RT);
    }
    self->callback_enabled = true;
}

STATIC void uart_callback_disable (mp_obj_t self_in) {
    pyb_uart_obj_t *self = self_in;
    self->callback_enabled = false;
}

/******************************************************************************/
/* Micro Python bindings                                                      */

STATIC void pyb_uart_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    pyb_uart_obj_t *self = self_in;
    if (self->baudrate > 0) {
        mp_printf(print, "UART(%u, baudrate=%u, bits=", self->uart_id, self->baudrate);
        switch (self->config & UART_CONFIG_WLEN_MASK) {
        case UART_CONFIG_WLEN_5:
            mp_print_str(print, "5");
            break;
        case UART_CONFIG_WLEN_6:
            mp_print_str(print, "6");
            break;
        case UART_CONFIG_WLEN_7:
            mp_print_str(print, "7");
            break;
        case UART_CONFIG_WLEN_8:
            mp_print_str(print, "8");
            break;
        default:
            break;
        }
        if ((self->config & UART_CONFIG_PAR_MASK) == UART_CONFIG_PAR_NONE) {
            mp_print_str(print, ", parity=None");
        } else {
            mp_printf(print, ", parity=%u", (self->config & UART_CONFIG_PAR_MASK) == UART_CONFIG_PAR_EVEN ? 0 : 1);
        }
        mp_printf(print, ", stop=%u)", (self->config & UART_CONFIG_STOP_MASK) == UART_CONFIG_STOP_ONE ? 1 : 2);
    }
    else {
        mp_printf(print, "UART(%u)", self->uart_id);
    }
}

STATIC const mp_arg_t pyb_uart_init_args[] = {
    { MP_QSTR_baudrate,     MP_ARG_REQUIRED | MP_ARG_INT,  },
    { MP_QSTR_bits,         MP_ARG_KW_ONLY  | MP_ARG_INT,  {.u_int = 8} },
    { MP_QSTR_parity,       MP_ARG_KW_ONLY  | MP_ARG_OBJ,  {.u_obj = mp_const_none} },
    { MP_QSTR_stop,         MP_ARG_KW_ONLY  | MP_ARG_INT,  {.u_int = 1} },
    { MP_QSTR_pins,         MP_ARG_KW_ONLY  | MP_ARG_OBJ,  {.u_obj = MP_OBJ_NULL} },
};
STATIC mp_obj_t pyb_uart_init_helper(pyb_uart_obj_t *self, mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    // parse args
    mp_arg_val_t args[MP_ARRAY_SIZE(pyb_uart_init_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(pyb_uart_init_args), pyb_uart_init_args, args);

    // get the baudrate
    if (args[0].u_int <= 0) {
        goto error;
    }
    uint baudrate = args[0].u_int;
    uint config;
    switch (args[1].u_int) {
    case 5:
        config = UART_CONFIG_WLEN_5;
        break;
    case 6:
        config = UART_CONFIG_WLEN_6;
        break;
    case 7:
        config = UART_CONFIG_WLEN_7;
        break;
    case 8:
        config = UART_CONFIG_WLEN_8;
        break;
    default:
        goto error;
        break;
    }
    // parity
    if (args[2].u_obj == mp_const_none) {
        config |= UART_CONFIG_PAR_NONE;
    } else {
        config |= ((mp_obj_get_int(args[2].u_obj) & 1) ? UART_CONFIG_PAR_ODD : UART_CONFIG_PAR_EVEN);
    }
    // stop bits
    config |= (args[3].u_int == 1 ? UART_CONFIG_STOP_ONE : UART_CONFIG_STOP_TWO);

    // assign the pins
    mp_obj_t pins_o = args[4].u_obj;
    uint flowcontrol = UART_FLOWCONTROL_NONE;
    if (pins_o != mp_const_none) {
        mp_obj_t *pins;
        mp_uint_t n_pins = 2;
        if (pins_o == MP_OBJ_NULL) {
            // use the default pins
            pins = (mp_obj_t *)pyb_uart_def_pin[self->uart_id];
        } else {
            mp_obj_get_array(pins_o, &n_pins, &pins);
            if (n_pins != 2 && n_pins != 4) {
                goto error;
            }
            if (n_pins == 4) {
                if (pins[PIN_TYPE_UART_RTS] != mp_const_none && pins[PIN_TYPE_UART_RX] == mp_const_none) {
                    goto error;  // RTS pin given in TX only mode
                } else if (pins[PIN_TYPE_UART_CTS] != mp_const_none && pins[PIN_TYPE_UART_TX] == mp_const_none) {
                    goto error;  // CTS pin given in RX only mode
                } else {
                    if (pins[PIN_TYPE_UART_RTS] != mp_const_none) {
                        flowcontrol |= UART_FLOWCONTROL_RX;
                    }
                    if (pins[PIN_TYPE_UART_CTS] != mp_const_none) {
                        flowcontrol |= UART_FLOWCONTROL_TX;
                    }
                }
            }
        }
        pin_assign_pins_af (pins, n_pins, PIN_TYPE_STD_PU, PIN_FN_UART, self->uart_id);
    }

    self->baudrate = baudrate;
    self->config = config;
    self->flowcontrol = flowcontrol;

    // initialize and enable the uart
    uart_init (self);
    // register it with the sleep module
    pybsleep_add ((const mp_obj_t)self, (WakeUpCB_t)uart_init);
    // enable the callback
    uart_callback_new (self, mp_const_none, INT_PRIORITY_LVL_3, E_UART_TRIGGER_RX_ANY);

    return mp_const_none;

error:
    nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, mpexception_value_invalid_arguments));
}

STATIC mp_obj_t pyb_uart_make_new(mp_obj_t type_in, mp_uint_t n_args, mp_uint_t n_kw, const mp_obj_t *args) {
    // check arguments
    mp_arg_check_num(n_args, n_kw, 1, MP_ARRAY_SIZE(pyb_uart_init_args), true);

    // work out the uart id
    int32_t uart_id = mp_obj_get_int(args[0]);

    if (uart_id < PYB_UART_0 || uart_id > PYB_UART_1) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, mpexception_os_resource_not_avaliable));
    }

    // get the correct uart instance
    pyb_uart_obj_t *self = &pyb_uart_obj[uart_id];
    self->base.type = &pyb_uart_type;
    self->uart_id = uart_id;

    if (n_args > 1 || n_kw > 0) {
        // start the peripheral
        mp_map_t kw_args;
        mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
        pyb_uart_init_helper(self, n_args - 1, args + 1, &kw_args);
    }

    return self;
}

STATIC mp_obj_t pyb_uart_init(mp_uint_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    return pyb_uart_init_helper(args[0], n_args - 1, args + 1, kw_args);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(pyb_uart_init_obj, 1, pyb_uart_init);

STATIC mp_obj_t pyb_uart_deinit(mp_obj_t self_in) {
    pyb_uart_obj_t *self = self_in;

    // unregister it with the sleep module
    pybsleep_remove (self);
    // invalidate the baudrate
    self->baudrate = 0;
    // free the read buffer
    m_del(byte, self->read_buf, PYBUART_RX_BUFFER_LEN);
    MAP_UARTIntDisable(self->reg, UART_INT_RX | UART_INT_RT);
    MAP_UARTDisable(self->reg);
    MAP_PRCMPeripheralClkDisable(self->peripheral, PRCM_RUN_MODE_CLK | PRCM_SLP_MODE_CLK);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(pyb_uart_deinit_obj, pyb_uart_deinit);

STATIC mp_obj_t pyb_uart_any(mp_obj_t self_in) {
    pyb_uart_obj_t *self = self_in;
    uart_check_init(self);
    return mp_obj_new_int(uart_rx_any(self));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(pyb_uart_any_obj, pyb_uart_any);

STATIC mp_obj_t pyb_uart_sendbreak(mp_obj_t self_in) {
    pyb_uart_obj_t *self = self_in;
    uart_check_init(self);
    // send a break signal for at least 2 complete frames
    MAP_UARTBreakCtl(self->reg, true);
    UtilsDelay(UTILS_DELAY_US_TO_COUNT(PYBUART_2_FRAMES_TIME_US(self->baudrate)));
    MAP_UARTBreakCtl(self->reg, false);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(pyb_uart_sendbreak_obj, pyb_uart_sendbreak);

STATIC mp_obj_t pyb_uart_callback (mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    mp_arg_val_t args[mpcallback_INIT_NUM_ARGS];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, mpcallback_INIT_NUM_ARGS, mpcallback_init_args, args);

    // check if any parameters were passed
    pyb_uart_obj_t *self = pos_args[0];
    uart_check_init(self);
    mp_obj_t _callback = mpcallback_find((mp_obj_t)self);
    if (kw_args->used > 0) {

        // convert the priority to the correct value
        uint priority = mpcallback_translate_priority (args[2].u_int);

        // check the power mode
        if (PYB_PWR_MODE_ACTIVE != args[4].u_int) {
            nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, mpexception_value_invalid_arguments));
        }

        // register a new callback
        // FIXME triggers!!
        return uart_callback_new (self, args[1].u_obj, mp_obj_get_int(args[3].u_obj), priority);
    } else if (!_callback) {
        _callback = mpcallback_new (self, mp_const_none, &uart_cb_methods, false);
    }
    return _callback;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(pyb_uart_callback_obj, 1, pyb_uart_callback);

STATIC const mp_map_elem_t pyb_uart_locals_dict_table[] = {
    // instance methods
    { MP_OBJ_NEW_QSTR(MP_QSTR_init),        (mp_obj_t)&pyb_uart_init_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_deinit),      (mp_obj_t)&pyb_uart_deinit_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_any),         (mp_obj_t)&pyb_uart_any_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_sendbreak),   (mp_obj_t)&pyb_uart_sendbreak_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_callback),    (mp_obj_t)&pyb_uart_callback_obj },

    /// \method read([nbytes])
    { MP_OBJ_NEW_QSTR(MP_QSTR_read),        (mp_obj_t)&mp_stream_read_obj },
    /// \method readall()
    { MP_OBJ_NEW_QSTR(MP_QSTR_readall),     (mp_obj_t)&mp_stream_readall_obj },
    /// \method readline()
    { MP_OBJ_NEW_QSTR(MP_QSTR_readline),    (mp_obj_t)&mp_stream_unbuffered_readline_obj},
    /// \method readinto(buf[, nbytes])
    { MP_OBJ_NEW_QSTR(MP_QSTR_readinto),    (mp_obj_t)&mp_stream_readinto_obj },
    /// \method write(buf)
    { MP_OBJ_NEW_QSTR(MP_QSTR_write),       (mp_obj_t)&mp_stream_write_obj },

    // class constants
    { MP_OBJ_NEW_QSTR(MP_QSTR_RX_ANY),      MP_OBJ_NEW_SMALL_INT(E_UART_TRIGGER_RX_ANY) },
};

STATIC MP_DEFINE_CONST_DICT(pyb_uart_locals_dict, pyb_uart_locals_dict_table);

STATIC mp_uint_t pyb_uart_read(mp_obj_t self_in, void *buf_in, mp_uint_t size, int *errcode) {
    pyb_uart_obj_t *self = self_in;
    byte *buf = buf_in;
    uart_check_init(self);

    // make sure we want at least 1 char
    if (size == 0) {
        return 0;
    }

    // wait for first char to become available
    if (!uart_rx_wait(self)) {
        // we can either return 0 to indicate EOF (then read() method returns b'')
        // or return EAGAIN error to indicate non-blocking (then read() method returns None)
        return 0;
    }

    // read the data
    byte *orig_buf = buf;
    for ( ; ; ) {
        *buf++ = uart_rx_char(self);
        if (--size == 0 || !uart_rx_wait(self)) {
            // return number of bytes read
            return buf - orig_buf;
        }
    }
}

STATIC mp_uint_t pyb_uart_write(mp_obj_t self_in, const void *buf_in, mp_uint_t size, int *errcode) {
    pyb_uart_obj_t *self = self_in;
    const char *buf = buf_in;
    uart_check_init(self);

    // write the data
    if (!uart_tx_strn(self, buf, size)) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_OSError, mpexception_os_operation_failed));
    }
    return size;
}

STATIC mp_uint_t pyb_uart_ioctl(mp_obj_t self_in, mp_uint_t request, mp_uint_t arg, int *errcode) {
    pyb_uart_obj_t *self = self_in;
    mp_uint_t ret;
    uart_check_init(self);

    if (request == MP_IOCTL_POLL) {
        mp_uint_t flags = arg;
        ret = 0;
        if ((flags & MP_IOCTL_POLL_RD) && uart_rx_any(self)) {
            ret |= MP_IOCTL_POLL_RD;
        }
        if ((flags & MP_IOCTL_POLL_WR) && MAP_UARTSpaceAvail(self->reg)) {
            ret |= MP_IOCTL_POLL_WR;
        }
    } else {
        *errcode = EINVAL;
        ret = MP_STREAM_ERROR;
    }
    return ret;
}

STATIC const mp_stream_p_t uart_stream_p = {
    .read = pyb_uart_read,
    .write = pyb_uart_write,
    .ioctl = pyb_uart_ioctl,
    .is_text = false,
};

STATIC const mp_cb_methods_t uart_cb_methods = {
    .init = pyb_uart_callback,
    .enable = uart_callback_enable,
    .disable = uart_callback_disable,
};

const mp_obj_type_t pyb_uart_type = {
    { &mp_type_type },
    .name = MP_QSTR_UART,
    .print = pyb_uart_print,
    .make_new = pyb_uart_make_new,
    .getiter = mp_identity,
    .iternext = mp_stream_unbuffered_iter,
    .stream_p = &uart_stream_p,
    .locals_dict = (mp_obj_t)&pyb_uart_locals_dict,
};
