#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <mutex.h>
#include <thread.h>
#include <msg.h>

#include <posix_io.h>
#include <board_uart0.h>

#include "bordermultiplex.h"
#include "ieee802154_frame.h"
#include "flowcontrol.h"
#include "sixlowborder.h"
#include "sixlowip.h"
#include "sixlownd.h"
#include "serialnumber.h"
#include "sixlowerror.h"

#define READER_STACK_SIZE   512

char serial_reader_stack[READER_STACK_SIZE];
uint16_t serial_reader_pid;

void serial_reader_f(void);

uint8_t border_initialize(transceiver_type_t trans,ipv6_addr_t *border_router_addr) {
    ipv6_addr_t addr;
    
    serial_reader_pid = thread_create(
            serial_reader_stack, READER_STACK_SIZE,
            PRIORITY_MAIN-1, CREATE_STACKTEST,
            serial_reader_f, "serial_reader");
    
    if (border_router_addr == NULL) {
        border_router_addr = &addr;
        
        addr = flowcontrol_init();
    }
    
    /* only allow addresses generated accoding to 
     * RFC 4944 (Section 6) & RFC 2464 (Section 4) from short address 
     * -- for now
     */
    if (    border_router_addr->uint16[4] != HTONS(IEEE_802154_PAN_ID ^ 0x0200) ||
            border_router_addr->uint16[5] != HTONS(0x00FF) ||
            border_router_addr->uint16[6] != HTONS(0xFE00)
        ) {
        return SIXLOWERROR_ADDRESS;
    }
    
    // radio-address is 8-bit so this must be tested extra
    if (border_router_addr->uint8[14] != 0) {
        return SIXLOWERROR_ADDRESS;
    }
    
    sixlowpan_init(trans,border_router_addr->uint8[15],1);
    
    ipv6_init_iface_as_router();
    
    
    return SUCCESS;
}

uint16_t border_get_serial_reader() {
    return serial_reader_pid;
}

void serial_reader_f(void) {
    int main_pid = 0;
    int bytes;
    msg_t m;
    border_packet_t *uart_buf;
    
    posix_open(uart0_handler_pid, 0);
    
    msg_receive(&m);
    main_pid = m.sender_pid;
    
    while(1) {
        posix_open(uart0_handler_pid, 0);
        bytes = readpacket(get_serial_in_buffer(0), BORDER_BUFFER_SIZE);
        if (bytes < 0) {
            switch (bytes) {
                case (-SIXLOWERROR_ARRAYFULL):{
                    printf("ERROR: Array was full\n");
                    break;
                }
                default:{
                    printf("ERROR: unknown\n");
                    break;
                }
            }
            continue;
        }
        
        uart_buf = (border_packet_t*)get_serial_in_buffer(0);
        if (uart_buf->reserved == 0) {
            if (uart_buf->type == BORDER_PACKET_CONF_TYPE) {
                border_conf_header_t *conf_packet = (border_conf_header_t*)uart_buf;
                if (conf_packet->conftype == BORDER_CONF_SYN) {
                    m.content.ptr = (char *)conf_packet;
                    msg_send(&m, main_pid, 1);
                    continue;
                }
            }
            flowcontrol_deliver_from_uart(uart_buf, bytes);
        }
    }
}

void border_send_ipv6_over_lowpan(struct ipv6_hdr_t *packet, uint8_t aro_flag, uint8_t sixco_flag) {
    uint16_t offset = IPV6_HDR_LEN+HTONS(packet->length);
    
    packet->flowlabel = HTONS(packet->flowlabel);
    packet->length = HTONS(packet->length);
    
    memset(buffer, 0, BUFFER_SIZE);
    memcpy(buffer+LL_HDR_LEN, packet, offset);
    
    if (packet->nextheader == PROTO_NUM_ICMPV6) {
        struct icmpv6_hdr_t *icmp_buf = (struct icmpv6_hdr_t *)(packet + IPV6_HDR_LEN);
        if (icmp_buf->type == ICMP_REDIRECT) {
            return;
        }
        // Here, other ICMPv6 message types for ND may follow.
    }
    
    lowpan_init((ieee_802154_long_t*)&(packet->destaddr.uint16[4]), (uint8_t*)packet);
}

void border_process_lowpan(void) {
    msg_t m;
    struct ipv6_hdr_t *ipv6_buf;
    
    while(1) {
        msg_receive(&m);
        ipv6_buf = (struct ipv6_hdr_t *)m.content.ptr;
        // TODO: Bei ICMPv6-Paketen entsprechende LoWPAN-Optionen verarbeiten und entfernen
        multiplex_send_ipv6_over_uart(ipv6_buf);
    }
}
