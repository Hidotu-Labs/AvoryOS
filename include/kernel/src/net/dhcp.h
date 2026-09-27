#ifndef NET_DHCP_H
#define NET_DHCP_H

#include <stdbool.h>
#include <stdint.h>

struct dhcp_lease {
    uint32_t address;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns[2];        
    uint32_t lease_time;  
    uint32_t renew_time;   
    uint32_t rebind_time;   
    uint32_t server_id;    
    uint64_t obtained_ms;   
};

void dhcp_set_static_fallback(uint32_t address, uint32_t netmask,
                              uint32_t gateway);
bool dhcp_start(void);

const struct dhcp_lease *dhcp_current_lease(void);

bool dhcp_renew(void);

bool net_phase5_init(void);

#endif
