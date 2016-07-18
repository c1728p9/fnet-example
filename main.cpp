#include "mbed.h"
#include "fnet.h"

Serial pc(USBTX, USBRX);

static void fnet_app_init()
{
    static fnet_uint8_t         stack_heap[30000];
    struct fnet_init_params     init_params;

    /* Input parameters for FNET stack initialization */
    init_params.netheap_ptr = stack_heap;
    init_params.netheap_size = 30000;

    /* Init FNET stack */
    if(fnet_init(&init_params) != FNET_ERR)
    {
        pc.printf("Fnet initialized\r\n");
    }
}

static void udp_listen()
{
    fnet_socket_t udp_socket;
    uint8_t buffer[1500];
    fnet_address_family_t family = AF_INET6; // AF_INET for IPv4

    /* Create listen socket */
    if((udp_socket = fnet_socket(family, SOCK_DGRAM, 0)) == FNET_ERR)
    {
        pc.printf("BENCH: Socket creation error.\n");
        return;
    }

    struct sockaddr         local_addr;
    fnet_memset_zero(&local_addr, sizeof(local_addr));
    local_addr.sa_port = fnet_htons(1234);
    local_addr.sa_family = family;
    if(fnet_socket_bind(udp_socket, &local_addr, sizeof(local_addr)) == FNET_ERR)
    {
        FNET_DEBUG("BENCH: Socket bind error.\n");
        fnet_socket_close(udp_socket);
        return;
    }

    while (1) {

        /* Receive data */
        struct sockaddr addr;
        fnet_size_t addr_len = sizeof(addr);
        fnet_int32_t received = fnet_socket_recvfrom(udp_socket, buffer, sizeof(buffer), 0,
                                                     &addr, &addr_len );
        if(received > 0)
        {
            char ip_str[FNET_IP_ADDR_STR_SIZE];
            pc.printf("Receiving from %s:%d\r\n",  fnet_inet_ntop(addr.sa_family, (fnet_uint8_t *)(addr.sa_data), ip_str, sizeof(ip_str)), fnet_ntohs(addr.sa_port));
            pc.printf("Recieved data %s\r\n", buffer);


            const char resp[] = "recieved packet";
            fnet_int32_t send_ret = fnet_socket_sendto(udp_socket, (fnet_uint8_t *)resp, sizeof(resp), 0, &addr, sizeof(addr));
            pc.printf("Send ret: %i\r\n", send_ret);

        }
    }

    fnet_socket_close(udp_socket);

}

static void fnet_dhcp_addr_cb(fnet_dhcp_desc_t desc, fnet_netif_desc_t netif, void *param)
{
    fnet_ip4_addr_t addr4 = fnet_netif_get_ip4_addr(netif);

    char strbuf[FNET_IP_ADDR_STR_SIZE];
    fnet_inet_ntop(AF_INET, (const void *)&addr4, strbuf ,sizeof(strbuf));
    pc.printf("IPV4 addr: %s\r\n", strbuf);

    fnet_netif_ip6_addr_info_t addr6;
    //fnet_ip6_addr_t addr6 =
    fnet_index_t index = 0;
    while (fnet_netif_get_ip6_addr(netif, index, &addr6)) {
        fnet_inet_ntop(AF_INET6, (const void *)&addr6.address, strbuf ,sizeof(strbuf));
        pc.printf("IPV6 addr[%i]: %s\r\n", index, strbuf);
        index++;
    }

    udp_listen();
}

int main()
{
    pc.baud(115200);
    pc.printf("Hello world\r\n");
    fnet_app_init();

    fnet_netif_desc_t netif;
    struct fnet_dhcp_params dhcp_params;

    // Get current net interface.
    if((netif = fnet_netif_get_default()) == 0) {
        pc.printf("ERROR: Network Interface is not configurated!");
    } else {
        fnet_memset_zero(&dhcp_params, sizeof(struct fnet_dhcp_params));

        // Enable DHCP client.
        fnet_dhcp_desc_t desc = fnet_dhcp_init(netif, &dhcp_params);
        if (desc) {
            // Register DHCP event handler callbacks.
            fnet_dhcp_set_callback_updated(desc, fnet_dhcp_addr_cb, 0);
            //fnet_dhcp_set_callback_discover(desc, fnet_dhcp_addr_cb, 0);
        } else {
            pc.printf("ERROR: DHCP initialization is failed!");
        }
    }

    while (1) {
        fnet_poll_service();
    }
}
