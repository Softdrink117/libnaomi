#include <stdio.h>
#include <stdint.h>
#include "naomi/dimmcomms.h"
#include "naomi/timer.h"

#define USE_INTERRUPT_MODE 0

#define REG_A05F6904 ((volatile uint32_t *)0xA05F6904)
#define REG_A05F6914 ((volatile uint32_t *)0xA05F6914)
#define NAOMI_DIMM_COMMAND ((volatile uint16_t *)0xA05F703C)
#define NAOMI_DIMM_OFFSETL ((volatile uint16_t *)0xA05F7040)
#define NAOMI_DIMM_PARAMETERL ((volatile uint16_t *)0xA05F7044)
#define NAOMI_DIMM_PARAMETERH ((volatile uint16_t *)0xA05F7048)
#define NAOMI_DIMM_STATUS ((volatile uint16_t *)0xA05F704C)
#define REG_A05F7418 ((volatile int32_t *)0xA05F7418)

#define CONST_NO_DIMM 0xFFFF
#define CONST_DIMM_HAS_COMMAND 0x8000
#define CONST_DIMM_COMMAND_MASK 0x7E00
#define CONST_DIMM_TOP_MASK 0x1FF

static peek_call_t global_peek_hook = 0;
static poke_call_t global_poke_hook = 0;

int check_has_dimm_inserted(int check_regs_first)
{
    if ((check_regs_first != 0) && (*REG_A05F7418 != 0)) {
        return 0;
    }
    if (*NAOMI_DIMM_COMMAND == CONST_NO_DIMM) {
        return -1;
    }
    return 1;
}

void marshall_dimm_command()
{
    static uint32_t base_address = 0;

    if (*REG_A05F7418 == 0) {
        // Do stuff here
        uint16_t dimm_command = *NAOMI_DIMM_COMMAND;

        if (dimm_command & CONST_DIMM_HAS_COMMAND) {
            // Get the command ID
            unsigned int dimm_command_id = (dimm_command & CONST_DIMM_COMMAND_MASK) >> 9;
            unsigned short retval = 0;
            unsigned short offsetl = 0;
            unsigned short paraml = 0;
            unsigned short paramh = 0;

            switch (dimm_command_id) {
                case 0:
                {
                    /* NOOP command */
                    retval = 1;
                    break;
                }
                case 1:
                {
                    /* Net Dimm firmware calls this "control read." Still not sure what it is. If this returns
                     * a valid nonzero value, the net dimm will request a bunch of pokes at addresses relative
                     * to this return. So its clearly returning the control structure, but what is that? On
                     * an H bios with net dimm 4.02 I get the address 0xc299394. */
                    retval = 1;
                    break;
                }
                case 3:
                {
                    /* Update base address */
                    base_address = (((*NAOMI_DIMM_PARAMETERH) & 0xFFFF) << 16) | ((*NAOMI_DIMM_PARAMETERL) & 0xFFFF);
                    retval = 1;
                    break;
                }
                case 4:
                {
                    /* Peek 8-bit value out of memory */
                    uint32_t address = (((dimm_command & CONST_DIMM_TOP_MASK) << 16) | (*NAOMI_DIMM_OFFSETL) & 0xFFFF) + base_address;

                    if (global_peek_hook)
                    {
                        paraml = global_peek_hook(address, 1) & 0xFF;
                    }

                    retval = 1;
                    break;
                }
                case 5:
                {
                    /* Peek 16-bit value out of memory */
                    uint32_t address = (((dimm_command & CONST_DIMM_TOP_MASK) << 16) | (*NAOMI_DIMM_OFFSETL) & 0xFFFF) + base_address;

                    if ((address & 1) == 0)
                    {
                        if (global_peek_hook)
                        {
                            paraml = global_peek_hook(address, 2) & 0xFFFF;
                        }

                        retval = 1;
                    }
                    else
                    {
                        retval = 0;
                    }

                    break;
                }
                case 6:
                {
                    /* Peek 32-bit value out of memory */
                    uint32_t address = (((dimm_command & CONST_DIMM_TOP_MASK) << 16) | (*NAOMI_DIMM_OFFSETL) & 0xFFFF) + base_address;

                    if ((address & 3) == 0)
                    {
                        if (global_peek_hook)
                        {
                            uint32_t data = global_peek_hook(address, 4);
                            paramh = (data >> 16) & 0xFFFF;
                            paraml = data & 0xFFFF;
                        }

                        retval = 1;
                    }
                    else
                    {
                        retval = 0;
                    }

                    break;
                }
                case 8:
                {
                    /* Poke 8-bit value into memory */
                    uint32_t address = (((dimm_command & CONST_DIMM_TOP_MASK) << 16) | (*NAOMI_DIMM_OFFSETL) & 0xFFFF) + base_address;
                    uint8_t value = (*NAOMI_DIMM_PARAMETERL) & 0xFF;
                    if (global_poke_hook)
                    {
                        global_poke_hook(address, 1, value);
                    }

                    retval = 1;
                    break;
                }
                case 9:
                {
                    /* Poke 16-bit value into memory */
                    uint32_t address = (((dimm_command & CONST_DIMM_TOP_MASK) << 16) | (*NAOMI_DIMM_OFFSETL) & 0xFFFF) + base_address;
                    uint16_t value = (*NAOMI_DIMM_PARAMETERL) & 0xFFFF;

                    if ((address & 1) == 0)
                    {
                        if (global_poke_hook)
                        {
                            global_poke_hook(address, 2, value);
                        }

                        retval = 1;
                    }
                    else
                    {
                        retval = 0;
                    }

                    break;
                }
                case 10:
                {
                    /* Poke 32-bit value into memory */
                    uint32_t address = (((dimm_command & CONST_DIMM_TOP_MASK) << 16) | (*NAOMI_DIMM_OFFSETL) & 0xFFFF) + base_address;
                    uint32_t value = (((*NAOMI_DIMM_PARAMETERH) & 0xFFFF) << 16) | ((*NAOMI_DIMM_PARAMETERL) & 0xFFFF);

                    if ((address & 3) == 0)
                    {
                        if (global_poke_hook)
                        {
                            global_poke_hook(address, 4, value);
                        }

                        retval = 1;
                    }
                    else
                    {
                        retval = 0;
                    }

                    break;
                }
                default:
                {
                    /* Invalid command */
                    retval = 0xFF;
                    break;
                }
            }

            // I don't know why this wait is necessary. If I remove it, the net dimm
            // flat out never receives responses and won't reboot homebrew when sending
            // a new image.
            timer_wait(5);

            // Acknowledge the command, return the response.
            *NAOMI_DIMM_COMMAND = (dimm_command & CONST_DIMM_COMMAND_MASK) | (retval & 0xFF);
            *NAOMI_DIMM_OFFSETL = offsetl;
            *NAOMI_DIMM_PARAMETERL = paraml;
            *NAOMI_DIMM_PARAMETERH = paramh;
            *NAOMI_DIMM_STATUS = *NAOMI_DIMM_STATUS | 0x100;

            do {
                /* Do some spinlop to wait for some other register. */
            } while ((*REG_A05F6904 & 8) != 0);

            /* Send interrupt to the DIMM itself saying we have data. */
            *NAOMI_DIMM_STATUS = *NAOMI_DIMM_STATUS & 0xFFFE;
        }
        else {
            /* Acknowledge the command */
            *NAOMI_DIMM_STATUS = *NAOMI_DIMM_STATUS | 0x100;
            do {
                /* Do some spinloop to wait for some other register. */
            } while ((*REG_A05F6904 & 8) != 0);
        }
    }
#if USE_INTERRUPT_MODE
    else {
        // Some other acknowledge?
        *REG_A05F6914 = *REG_A05F6914 & 0xfffffff7;
    }
#endif
}

void dimm_comms_poll()
{
    // Copy BIOS DIMM service routine basics.
    static int dimm_present = 0;
    static int dimm_init = 0;

    if (dimm_init == 0) {
        dimm_init = 1;
        dimm_present = check_has_dimm_inserted(1);

#if USE_INTERRUPT_MODE
        if ((*REG_A05F6914 & 8) == 0) {
            *REG_A05F6914 = *REG_A05F6914 | 8;
        }
#endif
    }

    if (dimm_present == 1) {
        marshall_dimm_command();
    }
}

void dimm_comms_attach_hooks(peek_call_t peek_hook, poke_call_t poke_hook)
{
    global_peek_hook = peek_hook;
    global_poke_hook = poke_hook;
}

void dimm_comms_detach_hooks()
{
    global_peek_hook = 0;
    global_poke_hook = 0;
}

// These are hooks that implement peek/poke as actual memory address handlers.
uint32_t __address_peek_memory(unsigned int address, int size)
{
    if (size == 1)
    {
        return *((volatile uint8_t *)address);
    }
    if (size == 2)
    {
        return *((volatile uint16_t *)address);
    }
    if (size == 4)
    {
        return *((volatile uint32_t *)address);
    }
    return 0;
}

void __address_poke_memory(unsigned int address, int size, uint32_t data)
{
    if (size == 1)
    {
        *((volatile uint8_t *)address) = data & 0xFF;
    }
    if (size == 2)
    {
        *((volatile uint16_t *)address) = data & 0xFFFF;
    }
    if (size == 4)
    {
        *((volatile uint32_t *)address) = data;
    }
}

void dimm_comms_attach_default_hooks()
{
    global_peek_hook = &__address_peek_memory;
    global_poke_hook = &__address_poke_memory;
}