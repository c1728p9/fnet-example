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

#ifndef STACK_MEMORY_H
#define STACK_MEMORY_H

#include<stdint.h>

class StackMem;
class StackMemChain;

class StackMemory
{
public:
    StackMemory() {}
    virtual ~StackMemory() {}
    virtual StackMem *alloc(uint32_t size, uint32_t align) = 0;
    virtual void free(StackMem *mem) = 0;
    virtual uint8_t* data_ptr(StackMem *ptr) = 0;
    virtual uint32_t len(StackMem* ptr) = 0;
    virtual void set_len(StackMem *ptr, uint32_t len) = 0;

    virtual StackMem *dequeue_alloc(StackMemChain **ptr) = 0;
    virtual void enqueue_free(StackMemChain *ptr, StackMem *mem) = 0;
    virtual uint32_t len(StackMemChain* ptr) = 0;
};

#endif
