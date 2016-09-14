/*
 * fnet_main.cpp
 *
 *  Created on: Sep 11, 2016
 *      Author: Russ
 */

// FNET MAC
#include "mbed.h"
#include "rtos.h"
#include "fnet.h"
#include "fnet_eth_prv.h"
#include "fnet_prot.h"
#include "EmacInterface.h"
#include "StackMemory.h"
#include "fnet_eth_prv.h"

typedef struct {
    EmacInterface *emac;
    bool connected;
} emac_state_t;

class StackMemoryFnet : public StackMemory
{
public:
    StackMemoryFnet();
    virtual ~StackMemoryFnet();
    virtual StackMem *alloc(uint32_t size, uint32_t align);
    virtual void free(StackMem *mem);
    virtual uint8_t* data_ptr(StackMem *ptr);
    virtual uint32_t len(StackMem* ptr);
    virtual void set_len(StackMem *ptr, uint32_t len);

    virtual StackMem *dequeue_alloc(StackMemChain **ptr);
    virtual void enqueue_free(StackMemChain *ptr, StackMem *mem);
    virtual uint32_t len(StackMemChain* ptr);
};

static void fnet_fec_output(fnet_netif_t *netif, fnet_uint16_t type, const fnet_mac_addr_t dest_addr, fnet_netbuf_t *nb);
static void fnet_fec_multicast_join(fnet_netif_t *netif, fnet_mac_addr_t multicast_addr );
static void fnet_fec_multicast_leave(fnet_netif_t *netif, fnet_mac_addr_t multicast_addr );

static fnet_return_t fnet_fec_init(struct fnet_netif *netif);             /* Initialization function.*/
static void fnet_fec_release(struct fnet_netif *netif);          /* Shutdown function.*/
static fnet_return_t fnet_fec_get_hw_addr(struct fnet_netif *netif, fnet_uint8_t *hw_addr);
static fnet_return_t fnet_fec_set_hw_addr(struct fnet_netif *netif, fnet_uint8_t *hw_addr);
static fnet_bool_t fnet_fec_is_connected(struct fnet_netif *netif);
static fnet_return_t fnet_fec_get_statistics(struct fnet_netif *netif, struct fnet_netif_statistics *statistics);

// Emac callbacks
static void link_input_cb(void *user_data, StackMem* chain);
static void state_change_cb(void *user_data, bool up);

static emac_state_t *get_state(struct fnet_netif * netif)
{
    return (emac_state_t*)((fnet_eth_if_t*)netif->if_ptr)->if_cpu_ptr;
}
static EmacInterface *get_mac(struct fnet_netif * netif)
{
    return get_state(netif)->emac;
}

/*****************************************************************************
 * FEC general API structure.
 ******************************************************************************/
static const fnet_netif_api_t fnet_fec_api =
{
    FNET_NETIF_TYPE_ETHERNET,       /* Data-link type. */
    sizeof(fnet_mac_addr_t),
    fnet_fec_init,                  /* Initialization function.*/
    fnet_fec_release,               /* Shutdown function.*/
#if FNET_CFG_IP4
    fnet_eth_output_ip4,            /* IPv4 Transmit function.*/
#endif
    fnet_eth_change_addr_notify,    /* Address change notification function.*/
    fnet_eth_drain,                 /* Drain function.*/
    fnet_fec_get_hw_addr,
    fnet_fec_set_hw_addr,
    fnet_fec_is_connected,
    fnet_fec_get_statistics
#if FNET_CFG_MULTICAST
#if FNET_CFG_IP4
    , fnet_eth_multicast_join_ip4
    , fnet_eth_multicast_leave_ip4
#endif
#if FNET_CFG_IP6
    , fnet_eth_multicast_join_ip6
    , fnet_eth_multicast_leave_ip6
#endif
#endif
#if FNET_CFG_IP6
    , fnet_eth_output_ip6           /* IPv6 Transmit function.*/
#endif
};

static fnet_eth_if_t interface =
{
      0               /* Points to CPU-specific control data structure of the interface. */
    , 0
    , fnet_fec_output
#if FNET_CFG_MULTICAST
    , fnet_fec_multicast_join
    , fnet_fec_multicast_leave
#endif /* FNET_CFG_MULTICAST */
};

static fnet_netif_t netif =
{
    0,                          /* Pointer to the next net_if structure.*/
    0,                          /* Pointer to the previous net_if structure.*/
    "eth0",     /* Network interface name.*/
    FNET_CFG_CPU_ETH0_MTU,      /* Maximum transmission unit.*/
    &interface,           /* Points to interface specific data structure.*/
    &fnet_fec_api               /* Interface API */
};

static emac_state_t state;
//typedef struct {
//    EmacInterface *interface;
//} fnet_fec_data;

EmacInterface *get_onchip_mac(void);

void fnet_add_emac(EmacInterface *emac)
{
    static StackMemoryFnet mem;
    uint8_t mac_addr[6];

    // Setup netif
    //netif.name - todo - set to enet0, enet1, ..., enetN
    netif.mtu = emac->get_mtu_size();
    netif.if_ptr = (void*)&interface;
    netif.api = &fnet_fec_api;

    // Setup interface
    interface.if_cpu_ptr = (void*)&state;

    // Setup this driver's state
    memset((void*)&state, 0, sizeof(state));
    state.emac = emac;
    state.connected = false;

    // Setup emac
    emac->set_mem_allocator(&mem);
    emac->set_link_input(link_input_cb, (void*)&netif);
    emac->set_link_state_cb(state_change_cb, (void*)&netif);

    emac->get_hwaddr(mac_addr);
    fnet_return_t result = fnet_netif_init(&netif, mac_addr, sizeof(fnet_mac_addr_t));
    if (result != FNET_OK) {
        error("Failed to init netif");
    }
    fnet_netif_set_default(&netif);
}

static void fnet_fec_output(fnet_netif_t *netif, fnet_uint16_t type, const fnet_mac_addr_t dest_addr, fnet_netbuf_t *nb)
{
    //TODO - check for memory leaks
    EmacInterface *mac = get_mac(netif);
    fnet_eth_header_t *ethheader;

    if((nb != 0) && (nb->total_length <= netif->mtu))
    {
        fnet_netbuf_t *temp_nb = fnet_netbuf_new(FNET_ETH_HDR_SIZE + 2, FNET_TRUE);
        nb = fnet_netbuf_concat(temp_nb, nb);

        *(uint8_t*)(nb->data_ptr + 0) = 0xFF;
        *(uint8_t*)(nb->data_ptr + 1) = 0x00;

        //ethheader = (fnet_eth_header_t *)fnet_ntohl((fnet_uint32_t)nb->data_ptr);
        ethheader = (fnet_eth_header_t *)((uint8_t*)nb->data_ptr + 2);

        //TODO - make sure checksum doesn't need to be cleared

        fnet_memcpy (ethheader->destination_addr, dest_addr, sizeof(fnet_mac_addr_t));

        mac->get_hwaddr((uint8_t*)&ethheader->source_addr);

        ethheader->type = fnet_htons(type);

        mac->linkoutput((StackMemChain*)nb);


#if !FNET_CFG_CPU_ETH_MIB
//        ((fnet_eth_if_t *)(netif->if_ptr))->statistics.tx_packet++;
#endif
    }

    fnet_netbuf_free_chain(nb);
}

static void fnet_fec_multicast_join(fnet_netif_t *netif, fnet_mac_addr_t multicast_addr)
{
    //TODO - support multicast
    //error("Multicast unsupported");
}
static void fnet_fec_multicast_leave(fnet_netif_t *netif, fnet_mac_addr_t multicast_addr)
{
    //TODO - support multicast
    //error("Multicast unsupported");
}

static fnet_return_t fnet_fec_init(struct fnet_netif *netif)
{
    EmacInterface *mac = get_mac(netif);
    mac->powerup();
    return FNET_OK;
}
static void fnet_fec_release(struct fnet_netif *netif)
{
    EmacInterface *mac = get_mac(netif);
    mac->powerdown();
}
static fnet_return_t fnet_fec_get_hw_addr(struct fnet_netif *netif, fnet_uint8_t *hw_addr)
{
    EmacInterface *mac = get_mac(netif);
    mac->set_hwaddr(hw_addr);
    return FNET_OK;
}
static fnet_return_t fnet_fec_set_hw_addr(struct fnet_netif *netif, fnet_uint8_t *hw_addr)
{
    EmacInterface *mac = get_mac(netif);
    mac->get_hwaddr(hw_addr);
    return FNET_OK;
}
static fnet_bool_t fnet_fec_is_connected(struct fnet_netif *netif)
{
    emac_state_t *state = get_state(netif);
    return state->connected ? FNET_TRUE : FNET_FALSE;
}
static fnet_return_t fnet_fec_get_statistics(struct fnet_netif *netif, struct fnet_netif_statistics *statistics)
{
    //TODO - support multicast
    error("Statistics unsupported");
    return FNET_ERR;
}


static void link_input_cb(void *user_data, StackMem* chain)
{
    //TODO - check for memory leaks
    struct fnet_netif *netif = (struct fnet_netif *)user_data;
    EmacInterface *emac = get_mac(netif);
    fnet_netbuf_t *nb = (fnet_netbuf_t *)chain;

    fnet_mac_addr_t local_mac_addr;

    // Strip off preamble and SFD
    fnet_netbuf_trim(&nb, 2);

    /* Point to the ethernet header.*/
    //fnet_eth_header_t *ethheader = (fnet_eth_header_t *)fnet_ntohl((fnet_uint32_t)nb->data_ptr);
    fnet_eth_header_t *ethheader = (fnet_eth_header_t *)nb->data_ptr;

    /* Just ignore our own "bounced" packets.*/
    emac->get_hwaddr((uint8_t*)&local_mac_addr);
    if(!fnet_memcmp(ethheader->source_addr, local_mac_addr, sizeof(local_mac_addr)))
    {
        fnet_netbuf_free_chain(nb);
        return;
    }

    //fnet_eth_trace("\nRX", ethheader); /* Print ETH header.*/

    fnet_netbuf_trim(&nb, FNET_ETH_HDR_SIZE);

    //TODO - set broadcast and multicast flags
    /* Network-layer input.*/
    fnet_eth_prot_input(netif, nb, ethheader->type);
}

static void state_change_cb(void *user_data, bool up)
{
    struct fnet_netif *netif = (struct fnet_netif *)user_data;
    emac_state_t *state = get_state(netif);
    state->connected = up;
}


StackMemoryFnet::StackMemoryFnet()
{

}

StackMemoryFnet::~StackMemoryFnet()
{

}

StackMem *StackMemoryFnet::alloc(uint32_t size, uint32_t align)
{
    fnet_netbuf_t *nb = fnet_netbuf_new(size + align, FNET_TRUE);
    uint32_t remainder = (uint32_t)nb->data_ptr % align;
    uint32_t offset = align - remainder;
    if (offset >= align) {
        offset -= align;
    }
    fnet_netbuf_trim(&nb, offset);
    return (StackMem *)nb;
}

void StackMemoryFnet::free(StackMem *ptr)
{
    fnet_netbuf_t *nb = (fnet_netbuf_t *)ptr;
    fnet_netbuf_free_chain(nb);
}

uint8_t* StackMemoryFnet::data_ptr(StackMem *ptr)
{
    fnet_netbuf_t *nb = (fnet_netbuf_t *)ptr;
    return (uint8_t*)nb->data_ptr;
}

uint32_t StackMemoryFnet::len(StackMem* ptr)
{
    fnet_netbuf_t *nb = (fnet_netbuf_t *)ptr;
    return nb->length;
}

void StackMemoryFnet::set_len(StackMem *ptr, uint32_t len)
{
    fnet_netbuf_t *nb = (fnet_netbuf_t *)ptr;
    MBED_ASSERT(NULL == nb->next);
    if (len > nb->length) {
        error("set_len can only decrease size");
    }
    // change must be negative
    fnet_int32_t change = len - nb->length;
    fnet_netbuf_trim(&nb, change);
}

StackMem *StackMemoryFnet::dequeue_alloc(StackMemChain **ptr)
{
    fnet_netbuf_t **p_nb = (fnet_netbuf_t **)ptr;
    fnet_netbuf_t *nb = *p_nb;
    *p_nb = nb->next;
    return (StackMem *)nb;
}

void StackMemoryFnet::enqueue_free(StackMemChain *ptr, StackMem *mem)
{
    //TODO
}

uint32_t StackMemoryFnet::len(StackMemChain* ptr)
{
    fnet_netbuf_t *nb = (fnet_netbuf_t *)ptr;
    return nb->total_length;
}


