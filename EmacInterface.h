/* EmacInterface
 * Copyright (c) 2016 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef EMAC_INTERFACE_H
#define EMAC_INTERFACE_H

#include<stdint.h>
#include "StackMemory.h"

#define MAC_ADDRESS_SIZE    6

typedef void (*linkinput_t) (void *user_data, StackMem* chain);
typedef void (*link_state_change_cb_t) (void *user_data, bool up);

class EmacInterface
{
public:
    EmacInterface() {}
    virtual ~EmacInterface() {}
    virtual uint32_t get_mtu_size() = 0;
    virtual const char *get_hostname() = 0;
    virtual void get_hwaddr(uint8_t *addr) = 0;
    virtual void set_hwaddr(uint8_t *addr) = 0;
    virtual bool linkoutput(StackMemChain* buf) = 0;
    virtual void powerup() = 0;
    virtual void powerdown() = 0;

    virtual void set_mem_allocator(StackMemory *allocator) = 0;
    virtual void set_link_input(linkinput_t input_funct, void *user_data) = 0;
    virtual void set_link_state_cb(link_state_change_cb_t state_cb, void *user_data) = 0;
};


#endif
