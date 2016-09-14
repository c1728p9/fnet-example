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

#include "mbed.h"
#include "rtos.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "fsl_phy.h"
#include "fsl_port.h"

#include "mbed_interface.h"
#include "SingletonPtr.h"
#include "EmacInterface.h"
#include "EmacInterfaceOnchip.h"

#define LINK_STATS_INC(...)
#define LWIP_DEBUGF(...)

#define PHY_TASK_PERIOD_MS          200
#define ENET_RX_RING_LEN            (16)
#define ENET_TX_RING_LEN            (8)
#define ENET_ETH_MAX_FLEN           (1522) // recommended size for a VLAN frame

/********************************************************************************
 * Internal data
 ********************************************************************************/
#define ENET_BuffSizeAlign(n) ENET_ALIGN(n, ENET_BUFF_ALIGNMENT)
#define ENET_ALIGN(x,align)   ((unsigned int)((x) + ((align)-1)) & (unsigned int)(~(unsigned int)((align)- 1)))
static void init_eth_hardware(void);

enum isr_event_t {
    TX_EVENT = (1 << 0),
    RX_EVENT = (1 << 1),
    TIMER_EVENT = (1 << 2),
};

#define STATE_UNKNOWN           (-1)


typedef struct {
    int connected;
    phy_speed_t speed;
    phy_duplex_t duplex;
} PHY_STATE;

static void enet_mac_rx_isr(void);
static void enet_mac_tx_isr(void);
static void enet_mac_timer_isr(void);
static void ethernet_callback(ENET_Type *base, enet_handle_t *handle, enet_event_t event, void *param);

class MacInterfaceOnchip : public EmacInterface
{
public:
    MacInterfaceOnchip();
    virtual ~MacInterfaceOnchip();
    virtual uint32_t get_mtu_size();
    virtual const char *get_hostname();
    virtual void get_hwaddr(uint8_t *addr);
    virtual void set_hwaddr(uint8_t *addr);
    virtual bool linkoutput(StackMemChain* buf);
    virtual void powerup();
    virtual void powerdown();
    virtual void set_mem_allocator(StackMemory *allocator);
    virtual void set_link_input(linkinput_t input_funct, void *user_data);
    virtual void set_link_state_cb(link_state_change_cb_t state_cb, void *user_data);

private:
    friend void enet_mac_rx_isr(void);
    friend void enet_mac_tx_isr(void);
    friend void enet_mac_timer_isr(void);

    StackMemory *mem;
    linkinput_t input_funct;
    void *user_data;
    link_state_change_cb_t state_change_cb;
    void *user_data_state_change;
    bool powered;
    bool thread_started;

    uint8_t hardware_address[MAC_ADDRESS_SIZE];

    // Platform dependencies
    Thread eth_thread;
    Ticker poll_ticker;
    Mutex TXLockMutex; /**< TX critical section mutex */
    Semaphore xTXDCountSem; /**< TX free buffer counting semaphore */

    // Buffer management
    enet_handle_t g_handle;
    // TX Buffer descriptors
    uint8_t *tx_desc_start_addr;
    // RX Buffer descriptors
    uint8_t *rx_desc_start_addr;
    // RX packet buffer pointers
    struct StackMem *rx_buff[ENET_RX_RING_LEN];
    // TX packet buffer pointers
    struct StackMem *tx_buff[ENET_RX_RING_LEN];
    // RX packet payload pointers
    uint32_t *rx_ptr[ENET_RX_RING_LEN];
    // TX ring buffers
    uint8_t tx_consume_index, tx_produce_index; /**< TX buffers ring */
    uint8_t rx_idx;

    // Phy monitoring variables
    PHY_STATE crt_state;
    PHY_STATE prev_state;

    void update_read_buffer(uint8_t *buf);
    void k64f_tx_reclaim(void);
    bool low_level_init(void);
    bool low_level_deinit(void);
    StackMem *k64f_low_level_input(int idx);
    void k64f_enetif_input(int idx);
    bool k64f_low_level_output(StackMemChain *chain);
    void k64f_phy_event(void);

    // ISR de-escalate task function
    void ethernet_task(void);
};

static SingletonPtr<MacInterfaceOnchip> mac;

EmacInterface *get_onchip_emac(void)
{
    return mac.get();
}

MacInterfaceOnchip::MacInterfaceOnchip()
    : eth_thread(), poll_ticker(), TXLockMutex(), xTXDCountSem()
{
    mem = NULL;
    input_funct = NULL;
    user_data = NULL;
    state_change_cb = NULL;
    user_data_state_change = NULL;
    powered = false;
    thread_started = false;
#if (MBED_MAC_ADDRESS_SUM != MBED_MAC_ADDR_INTERFACE)
    hardware_address[0] = MBED_MAC_ADDR_0;
    hardware_address[1] = MBED_MAC_ADDR_1;
    hardware_address[2] = MBED_MAC_ADDR_2;
    hardware_address[3] = MBED_MAC_ADDR_3;
    hardware_address[4] = MBED_MAC_ADDR_4;
    hardware_address[5] = MBED_MAC_ADDR_5;
#else
    mbed_mac_address((char *)hardware_address);
#endif

    memset((void*)&g_handle, 0, sizeof(&g_handle));
    tx_desc_start_addr = NULL;
    rx_desc_start_addr = NULL;
    memset((void*)rx_buff, 0, sizeof(rx_buff));
    memset((void*)tx_buff, 0, sizeof(tx_buff));
    memset((void*)rx_ptr, 0, sizeof(rx_ptr));
    tx_consume_index = 0;
    tx_produce_index = 0;
    rx_idx = 0;

    // Phy monitoring variables
    PHY_STATE crt_state;
    PHY_STATE prev_state;
    PHY_STATE init_state = {STATE_UNKNOWN, (phy_speed_t)STATE_UNKNOWN, (phy_duplex_t)STATE_UNKNOWN};
    crt_state = init_state;
    prev_state = init_state;
}

MacInterfaceOnchip::~MacInterfaceOnchip()
{
    //Do nothing
}

uint32_t MacInterfaceOnchip::get_mtu_size()
{
    return 1500;
}

const char *MacInterfaceOnchip::get_hostname()
{
    return "lwipk64f";
}

void MacInterfaceOnchip::get_hwaddr(uint8_t *addr)
{
    memcpy(addr, hardware_address, sizeof(hardware_address));
}
void MacInterfaceOnchip::set_hwaddr(uint8_t *addr)
{
    memcpy(hardware_address, addr, sizeof(hardware_address));
}
bool MacInterfaceOnchip::linkoutput(StackMemChain* chain)
{
    return k64f_low_level_output(chain);
}

void MacInterfaceOnchip::powerup()
{
    if (!thread_started) {
        eth_thread.start(callback(this, &MacInterfaceOnchip::ethernet_task));
        thread_started = true;
    }
    poll_ticker.attach_us(enet_mac_timer_isr, PHY_TASK_PERIOD_MS * 1000);

    bool success;
    success = low_level_init();
    if (!success) {
        error("Unable to powerup ethernet emac");
    }

    /* Allow the PHY task to detect the initial link state and set up the proper flags */
    eth_thread.signal_set(TIMER_EVENT);
    osDelay(10);
    powered = true;
}

void MacInterfaceOnchip::powerdown()
{
    poll_ticker.detach();
    low_level_deinit();
    powered = false;
}

void MacInterfaceOnchip::set_mem_allocator(StackMemory *allocator)
{
    this->mem = allocator;
}
void MacInterfaceOnchip::set_link_input(linkinput_t input_funct, void *user_data)
{
    this->input_funct = input_funct;
    this->user_data = user_data;
}

void MacInterfaceOnchip::set_link_state_cb(link_state_change_cb_t state_cb, void *user_data)
{
    state_change_cb = state_cb;
    this->user_data_state_change = user_data;
}

/********************************************************************************
 * Buffer management
 ********************************************************************************/
/*
 * This function will queue a new receive buffer
 */
void MacInterfaceOnchip::update_read_buffer(uint8_t *buf)
{
    if (buf != NULL) {
        g_handle.rxBdCurrent->buffer = buf;
    }

    /* Clears status. */
    g_handle.rxBdCurrent->control &= ENET_BUFFDESCRIPTOR_RX_WRAP_MASK;

    /* Sets the receive buffer descriptor with the empty flag. */
    g_handle.rxBdCurrent->control |= ENET_BUFFDESCRIPTOR_RX_EMPTY_MASK;

    /* Increases the buffer descriptor to the next one. */
    if (g_handle.rxBdCurrent->control & ENET_BUFFDESCRIPTOR_RX_WRAP_MASK) {
        g_handle.rxBdCurrent = g_handle.rxBdBase;
        g_handle.rxBdDirty = g_handle.rxBdBase;
    } else {
        g_handle.rxBdCurrent++;
        g_handle.rxBdDirty++;
    }

    /* Actives the receive buffer descriptor. */
    ENET->RDAR = ENET_RDAR_RDAR_MASK;
}

/** \brief  Free TX buffers that are complete
 *
 */
void MacInterfaceOnchip::k64f_tx_reclaim()
{
  uint8_t i = 0 ;

  /* Get exclusive access */
  TXLockMutex.lock();

  i = tx_consume_index;
  // Traverse all descriptors, looking for the ones modified by the uDMA
  while((i != tx_produce_index) && (!(g_handle.txBdDirty->control & ENET_BUFFDESCRIPTOR_TX_READY_MASK))) {
      mem->free(tx_buff[i]);
      if (g_handle.txBdDirty->control & ENET_BUFFDESCRIPTOR_TX_WRAP_MASK)
        g_handle.txBdDirty = g_handle.txBdBase;
      else
        g_handle.txBdDirty++;

      i = (i + 1) % ENET_TX_RING_LEN;
  }

  tx_consume_index = i;
  /* Restore access */
  TXLockMutex.unlock();
}

/** \brief  Low level init of the MAC and PHY.
 *
 */
bool MacInterfaceOnchip::low_level_init()
{
  uint8_t i;
  uint32_t sysClock;
  phy_speed_t phy_speed;
  phy_duplex_t phy_duplex;
  uint32_t phyAddr = 0;
  bool link = false;
  enet_config_t config;

  // Allocate RX descriptors
  rx_desc_start_addr = (uint8_t *)calloc(1, sizeof(enet_rx_bd_struct_t) * ENET_RX_RING_LEN + ENET_BUFF_ALIGNMENT);
  if(!rx_desc_start_addr)
    return false;

  // Allocate TX descriptors
  tx_desc_start_addr = (uint8_t *)calloc(1, sizeof(enet_tx_bd_struct_t) * ENET_TX_RING_LEN + ENET_BUFF_ALIGNMENT);
  if(!tx_desc_start_addr)
    return false;

  rx_desc_start_addr = (uint8_t *)ENET_ALIGN(rx_desc_start_addr, ENET_BUFF_ALIGNMENT);
  tx_desc_start_addr = (uint8_t *)ENET_ALIGN(tx_desc_start_addr, ENET_BUFF_ALIGNMENT);

  /* Create buffers for each receive BD */
  for (i = 0; i < ENET_RX_RING_LEN; i++) {
    rx_buff[i] = mem->alloc(ENET_ETH_MAX_FLEN, ENET_BUFF_ALIGNMENT);
    if (NULL == rx_buff[i])
      return false;

    rx_ptr[i] = (uint32_t*)mem->data_ptr(rx_buff[i]);
  }

  tx_consume_index = tx_produce_index = 0;

  /* prepare the buffer configuration. */
  enet_buffer_config_t buffCfg = {
    ENET_RX_RING_LEN,
    ENET_TX_RING_LEN,
    ENET_ALIGN(ENET_ETH_MAX_FLEN, ENET_BUFF_ALIGNMENT),
    0,
    (volatile enet_rx_bd_struct_t *)rx_desc_start_addr,
    (volatile enet_tx_bd_struct_t *)tx_desc_start_addr,
    (uint8_t *)&rx_ptr,
    NULL,
  };

  init_eth_hardware();

  sysClock = CLOCK_GetFreq(kCLOCK_CoreSysClk);

  ENET_GetDefaultConfig(&config);

  PHY_Init(ENET, 0, sysClock);
  PHY_GetLinkStatus(ENET, phyAddr, &link);
  if (link)
  {
    /* Get link information from PHY */
    PHY_GetLinkSpeedDuplex(ENET, phyAddr, &phy_speed, &phy_duplex);
    /* Change the MII speed and duplex for actual link status. */
    config.miiSpeed = (enet_mii_speed_t)phy_speed;
    config.miiDuplex = (enet_mii_duplex_t)phy_duplex;
    config.interrupt = kENET_RxFrameInterrupt | kENET_TxFrameInterrupt;
  }
  config.rxMaxFrameLen = ENET_ETH_MAX_FLEN;
  config.macSpecialConfig = kENET_ControlFlowControlEnable /*| kENET_ControlPromiscuousEnable */;// added kENET_ControlPromiscuousEnable
  config.txAccelerConfig = kENET_TxAccelIsShift16Enabled;
  config.rxAccelerConfig = kENET_RxAccelisShift16Enabled | kENET_RxAccelMacCheckEnabled;
  uint8_t hw_addr[6];
  get_hwaddr(hw_addr);
  ENET_Init(ENET, &g_handle, &config, &buffCfg, hw_addr, sysClock);
  ENET_SetCallback(&g_handle, ethernet_callback, NULL);
  ENET_ActiveRead(ENET);

  return true;
}

/** \brief  Low level deinit of the MAC and PHY.
 *
 */
bool MacInterfaceOnchip::low_level_deinit()
{
    ENET_Deinit(ENET);
    return true;
}

/** \brief  Allocates StackMem and returns the data from the incoming packet.
 *
 *  \param[in] idx   index of packet to be read
 *  \return a StackMem filled with the received packet (including MAC header)
 */
StackMem *MacInterfaceOnchip::k64f_low_level_input(int idx)
{
  volatile enet_rx_bd_struct_t *bdPtr = g_handle.rxBdCurrent;
  StackMem *p = NULL;
  StackMem *temp_rxbuf = NULL;
  uint32_t length = 0;
  const uint16_t err_mask = ENET_BUFFDESCRIPTOR_RX_TRUNC_MASK | ENET_BUFFDESCRIPTOR_RX_CRC_MASK |
                         ENET_BUFFDESCRIPTOR_RX_NOOCTET_MASK | ENET_BUFFDESCRIPTOR_RX_LENVLIOLATE_MASK;


#ifdef LOCK_RX_THREAD
  /* Get exclusive access */
  TXLockMutex.lock();
#endif

  /* Determine if a frame has been received */
  if ((bdPtr->control & err_mask) != 0) {
#if LINK_STATS
    if ((bdPtr->control & ENET_BUFFDESCRIPTOR_RX_LENVLIOLATE_MASK) != 0)
      LINK_STATS_INC(link.lenerr);
    else
      LINK_STATS_INC(link.chkerr);
#endif
    LINK_STATS_INC(link.drop);
    /* Re-use the same buffer in case of error */
    update_read_buffer(NULL);
  } else {
    /* A packet is waiting, get length */
    length = bdPtr->length;

    /* Zero-copy */
    p = rx_buff[idx];
    mem->set_len(p, length);

//    uint8_t* prx = mem->data_ptr(p);
//    printf("emac input:\r\n");
//    printf("  type: 0x%x\r\n", (prx[2 + 6 + 6 + 0] << 8) | (prx[2 + 6 + 6 + 1] << 0));
//    printf("  dst: ");
//    for (int i = 0; i < 6; i++) {
//        printf("%02hhx,", prx[2 + i]);
//    }
//    printf("\r\n");
//    printf("  src: ");
//    for (int i = 0; i < 6; i++) {
//        printf("%02hhx,", prx[2 + 6 + i]);
//    }
//    printf("\r\n");

    /* Attempt to queue new buffer */
    temp_rxbuf = mem->alloc(ENET_ETH_MAX_FLEN, ENET_BUFF_ALIGNMENT);
    if (NULL == temp_rxbuf) {
      /* Drop frame (out of memory) */
      LINK_STATS_INC(link.drop);

      /* Re-queue the same buffer */
      update_read_buffer(NULL);

      LWIP_DEBUGF(UDP_LPC_EMAC | LWIP_DBG_TRACE,
        ("k64f_low_level_input: Packet index %d dropped for OOM\n",
        idx));
#ifdef LOCK_RX_THREAD
      TXLockMutex.unlock();
#endif

      return NULL;
    }

    rx_buff[idx] = temp_rxbuf;
    rx_ptr[idx] = (uint32_t*)mem->data_ptr(rx_buff[idx]);
    update_read_buffer(mem->data_ptr(rx_buff[idx]));
    LWIP_DEBUGF(UDP_LPC_EMAC | LWIP_DBG_TRACE,
      ("k64f_low_level_input: Packet received: %p, size %d (index=%d)\n",
      p, length, idx));

    LINK_STATS_INC(link.recv);
  }

#ifdef LOCK_RX_THREAD
  TXLockMutex.unlock();
#endif

  return p;
}

/** \brief  Attempt to read a packet from the EMAC interface.
 *
 *  \param[in] idx   index of packet to be read
 */
void MacInterfaceOnchip::k64f_enetif_input(int idx)
{
  StackMem *p;

  /* move received packet into a new buffer */
  p = k64f_low_level_input(idx);
  if (p == NULL)
    return;

  /* send data to the stack */
  input_funct(user_data, p);
}

/** \brief  Low level output of a packet. Never call this from an
 *          interrupt context, as it may block until TX descriptors
 *          become available.
 *
 *  \param[in] p the MAC packet to send (e.g. IP packet including MAC addresses and type)
 *  \return ERR_OK if the packet could be sent or an err_t value if the packet couldn't be sent
 */
bool MacInterfaceOnchip::k64f_low_level_output(StackMemChain *chain)
{
  StackMem *q;
  StackMem *temp_buf;
  uint8_t *psend = NULL, *dst;


  temp_buf = mem->alloc(mem->len(chain), ENET_BUFF_ALIGNMENT);
  if (NULL == temp_buf)
    return false;

  psend = mem->data_ptr(temp_buf);


  for (q = mem->dequeue_alloc(&chain), dst = psend; q != NULL; q = mem->dequeue_alloc(&chain)) {
    memcpy(dst, mem->data_ptr(q), mem->len(q));
    dst += mem->len(q);
  }
//  printf("emac output:\r\n");
//  printf("  type: 0x%x\r\n", (psend[2 + 6 + 6 + 0] << 8) | (psend[2 + 6 + 6 + 0] << 0));
//  printf("  dst: ");
//  for (int i = 0; i < 6; i++) {
//      printf("%02hhx,", psend[2 + i]);
//  }
//  printf("\r\n");
//  printf("  src: ");
//  for (int i = 0; i < 6; i++) {
//      printf("%02hhx,", psend[2 + 6 + i]);
//  }
//  printf("\r\n");

  // TODO - how did this work? - nothing signals this
  /* Wait until a descriptor is available for the transfer. */
  /* THIS WILL BLOCK UNTIL THERE ARE A DESCRIPTOR AVAILABLE */
  while (g_handle.txBdCurrent->control & ENET_BUFFDESCRIPTOR_TX_READY_MASK)
      xTXDCountSem.wait(osWaitForever);

  /* Get exclusive access */
  TXLockMutex.lock();

  /* Save the buffer so that it can be freed when transmit is done */
  tx_buff[tx_produce_index] = temp_buf;
  tx_produce_index = (tx_produce_index + 1) % ENET_TX_RING_LEN;

  /* Setup transfers */
  g_handle.txBdCurrent->buffer = mem->data_ptr(temp_buf);
  g_handle.txBdCurrent->length = mem->len(temp_buf);
  g_handle.txBdCurrent->control |= (ENET_BUFFDESCRIPTOR_TX_READY_MASK | ENET_BUFFDESCRIPTOR_TX_LAST_MASK);

  /* Increase the buffer descriptor address. */
  if (g_handle.txBdCurrent->control & ENET_BUFFDESCRIPTOR_TX_WRAP_MASK)
    g_handle.txBdCurrent = g_handle.txBdBase;
  else
    g_handle.txBdCurrent++;

  /* Active the transmit buffer descriptor. */
  ENET->TDAR = ENET_TDAR_TDAR_MASK;

  LINK_STATS_INC(link.xmit);

  /* Restore access */
  TXLockMutex.unlock();

  return true;
}

/** \brief  Checks the state of the phy
 *
 */
void MacInterfaceOnchip::k64f_phy_event() {
    bool connection_status;
    uint32_t phyAddr = 0;
    uint32_t rcr = 0;

    // Get current status
    PHY_GetLinkStatus(ENET, phyAddr, &connection_status);
    crt_state.connected = connection_status ? 1 : 0;
    // Get the actual PHY link speed
    PHY_GetLinkSpeedDuplex(ENET, phyAddr, &crt_state.speed, &crt_state.duplex);

    // Compare with previous state
    if (crt_state.connected != prev_state.connected) {
      if (crt_state.connected)
          state_change_cb(user_data_state_change, true);
      else
          state_change_cb(user_data_state_change, false);
    }

    if (crt_state.speed != prev_state.speed) {
      rcr = ENET->RCR;
      rcr &= ~ENET_RCR_RMII_10T_MASK;
      rcr |= ENET_RCR_RMII_10T(!crt_state.speed);
      ENET->RCR = rcr;
    }

    prev_state = crt_state;
}

/** \brief  Task for de-escalate of ethernet events
 *
 */
void MacInterfaceOnchip::ethernet_task(void)
{
    while (1) {
        osEvent event = eth_thread.signal_wait(0);
        if (event.status != osEventSignal) {
            continue;
        }

        if (event.value.signals & TX_EVENT) {
            k64f_tx_reclaim();
        }
        if (event.value.signals & RX_EVENT) {
            while ((g_handle.rxBdCurrent->control & ENET_BUFFDESCRIPTOR_RX_EMPTY_MASK) == 0) {
                k64f_enetif_input(rx_idx);
                rx_idx = (rx_idx + 1) % ENET_RX_RING_LEN;
            }
        }
        if (event.value.signals & TIMER_EVENT) {
            k64f_phy_event();
        }
    }
}


/** \brief Ethernet receive interrupt handler
 *
 *  This function handles the receive interrupt of K64F.
 */
static void enet_mac_rx_isr()
{
    mac->eth_thread.signal_set(RX_EVENT);
}

/** \brief Ethernet transmit interrupt handler
 *
 *  This function handles the transmit interrupt of K64F.
 */
static void enet_mac_tx_isr()
{
    mac->eth_thread.signal_set(TX_EVENT);
}

/** \brief timer interrupt handler
 *
 *  This function handles the timer interrupt of K64F.
 */
static void enet_mac_timer_isr()
{
    mac->eth_thread.signal_set(TIMER_EVENT);
}

/** \brief Ethernet callback
 *
 *  This function passes ethernet events on to the ISR handlers
 */
static void ethernet_callback(ENET_Type *base, enet_handle_t *handle, enet_event_t event, void *param)
{
    switch (event)
    {
      case kENET_RxEvent:
        enet_mac_rx_isr();
        break;
      case kENET_TxEvent:
        enet_mac_tx_isr();
        break;
      default:
        break;
    }
}

static void init_eth_hardware(void)
{
    port_pin_config_t configENET = {0};

    /* Disable MPU. */
    MPU->CESR &= ~MPU_CESR_VLD_MASK;

    CLOCK_EnableClock(kCLOCK_PortC);
    CLOCK_EnableClock(kCLOCK_PortB);
    /* Affects PORTC_PCR16 register */
    PORT_SetPinMux(PORTC, 16u, kPORT_MuxAlt4);
    /* Affects PORTC_PCR17 register */
    PORT_SetPinMux(PORTC, 17u, kPORT_MuxAlt4);
    /* Affects PORTC_PCR18 register */
    PORT_SetPinMux(PORTC, 18u, kPORT_MuxAlt4);
    /* Affects PORTC_PCR19 register */
    PORT_SetPinMux(PORTC, 19u, kPORT_MuxAlt4);
    /* Affects PORTB_PCR1 register */
    PORT_SetPinMux(PORTB, 1u, kPORT_MuxAlt4);

    configENET.openDrainEnable = kPORT_OpenDrainEnable;
    configENET.mux = kPORT_MuxAlt4;
    configENET.pullSelect = kPORT_PullUp;
    /* Ungate the port clock */
    CLOCK_EnableClock(kCLOCK_PortA);
    /* Affects PORTB_PCR0 register */
    PORT_SetPinConfig(PORTB, 0u, &configENET);

    /* Affects PORTA_PCR13 register */
    PORT_SetPinMux(PORTA, 13u, kPORT_MuxAlt4);
    /* Affects PORTA_PCR12 register */
    PORT_SetPinMux(PORTA, 12u, kPORT_MuxAlt4);
    /* Affects PORTA_PCR14 register */
    PORT_SetPinMux(PORTA, 14u, kPORT_MuxAlt4);
    /* Affects PORTA_PCR5 register */
    PORT_SetPinMux(PORTA, 5u, kPORT_MuxAlt4);
    /* Affects PORTA_PCR16 register */
    PORT_SetPinMux(PORTA, 16u, kPORT_MuxAlt4);
    /* Affects PORTA_PCR17 register */
    PORT_SetPinMux(PORTA, 17u, kPORT_MuxAlt4);
    /* Affects PORTA_PCR15 register */
    PORT_SetPinMux(PORTA, 15u, kPORT_MuxAlt4);
    /* Affects PORTA_PCR28 register */
    PORT_SetPinMux(PORTA, 28u, kPORT_MuxAlt4);

    /* Select the Ethernet timestamp clock source */
    CLOCK_SetEnetTime0Clock(0x2);
}

/**
 * @}
 */

/* --------------------------------- End Of File ------------------------------ */

